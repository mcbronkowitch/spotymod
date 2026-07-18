# Bench firmware Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A standalone `bench/` firmware app plus a one-command host driver that measures the real cycle cost of the spotymod engine, of nine DaisySP expansion candidates, and of SRAM-vs-SDRAM buffer access on the actual Daisy Seed — and commits the numbers.

**Architecture:** `bench/` is its own libDaisy app (own `main.cpp`, own `Makefile`, own build dir) that compiles the portable `engine/` sources and links the **full** DaisySP. It never ships, so LGPL linkage and full-library access are fine. Measurement is two-stage: an **offline runner** walks a static workload table, timing each with the Cortex-M7 **DWT cycle counter** outside any interrupt context, then an **anchor mode** re-runs three reference workloads inside a real `AudioCallback` with `daisy::CpuLoadMeter` to calibrate the offline optimism. Results leave the board as marker-delimited lines over **ARM semihosting**, on the same SWD link the probe already owns — one cable, no second USB connection. `bench/run.py` builds, loads the image into SRAM through the debug probe, captures openocd's output, parses it, and writes two committed files under `docs/bench/`. The original firmware (`main.cpp`, `app.cpp`, `src/`, root `Makefile`) is not touched at any point.

**Tech Stack:** C++17, arm-none-eabi-gcc + libDaisy `core/Makefile` (DaisyToolchain), full DaisySP, openocd 0.12 (xPack, ships with DaisyToolchain), Python 3 standard library only for the host driver.

**Spec:** `docs/superpowers/specs/2026-07-18-bench-firmware-design.md`

---

## Global Constraints

- Repository: `c:\Users\bernd\Documents\AI\Spotykach` (the `spotymod` fork). Branch: `main`. Work directly on `main`.
- **The shipping firmware is untouchable.** No edit to `main.cpp`, `app.cpp`, `app.h`, `meter.h`, `common.h`, `alt_sram.lds`, the root `Makefile`, `CMakeLists.txt`, `src/**`, or `third_party/**`. If a bench workload appears to need a change in `engine/**`, stop and report instead of editing — measuring must not perturb the thing measured.
- **Measurement conditions, fixed for every workload and printed in the result header:** 48 kHz, block size **96**, block budget **960 000 cycles** (= 2 ms at 480 MHz), D-cache and I-cache enabled, `-ffast-math -funroll-loops`, compiled-in git short hash.
- **Every workload is deterministic and checksummed.** Seeded RNG, seeded noise / fixed test tones for input-consuming workloads. The runner folds each block's returned float into a 32-bit FNV-1a hash and prints it. This is what defeats dead-code elimination — do not add `volatile` hacks instead.
- Toolchain, already verified present on this machine:
  - `arm-none-eabi-gcc` → `C:\Program Files\DaisyToolchain\bin\arm-none-eabi-gcc`
  - `openocd` 0.12.0 → `C:\Program Files\DaisyToolchain\bin\openocd`, scripts at `C:\Program Files\DaisyToolchain\openocd\scripts` (has `interface/cmsis-dap.cfg`, `interface/stlink.cfg`, `interface/jlink.cfg`, `target/stm32h7x.cfg`)
  - `make` → `C:\Program Files\DaisyToolchain\bin\make`
- **libDaisy's `core/Makefile` derives object names with `$(notdir ...)`.** Two source files with the same basename anywhere in `CPP_SOURCES` silently clobber each other. Every filename introduced by this plan is globally unique across `bench/` + the twelve `engine/` sources — keep it that way.
- `.gitignore` already contains `**/build`, so `bench/build/` is ignored with no change needed. `docs/bench/` is **not** ignored — its files are the deliverable. Beware that `*.log` **is** ignored globally: never give a result file a `.log` extension, or it will vanish from `git status` without an error.
- This is the first Daisy-target build in the repo to link DaisySP at all — the shipping firmware sets `DAISYSP_DIR` only for the `libs` target and links none of it.
- **Every `make` invocation in this repo needs `MAKE=make`.** GNU Make auto-detects `$(MAKE)` as `C:/Program Files/DaisyToolchain/.../make.exe` and substitutes it unquoted into recipe shell commands, so `/bin/sh` splits it at the space and reports `C:/Program: No such file or directory`. Plain `make` is already first on `PATH`. This is an invocation workaround, never a reason to patch a Makefile.
- Commit messages follow the repo's style (`feat(bench): …`, `chore(bench): …`, `docs(bench): …`). Every commit trailer:
  ```
  Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
  ```
- Do not add a `Claude`/`Anthropic` co-author trailer.
- There are no unit tests for this work. The Daisy Seed **is** the test rig: every task ends with a real run on real hardware and an observed output line. "It compiles" is never an acceptance criterion here.

---

## Deviations from the spec (deliberate, already decided)

**1. Transport is ARM semihosting, not RTT and not USB-CDC.** The spec names RTT as preferred with serial as fallback. Both were rejected after testing on the actual desk:

- **RTT** needs a SEGGER RTT control block compiled into the firmware. Neither libDaisy nor this repo vendors `SEGGER_RTT.c`, so RTT would mean vendoring third-party sources into a repo whose `third_party/` this plan is forbidden to touch.
- **USB-CDC** works, but costs a second USB cable to the PC, `pyserial`, COM-port discovery, and a `StartLog(true)` enumeration handshake — machinery whose only job is to move thirty short lines.

**Semihosting sends those lines back over the SWD link the probe already owns.** One cable, no extra host dependency, and openocd's own stdout *is* the capture. Verified on this desk before implementation: openocd 0.12 answers `arm semihosting enable` with `semihosting is enabled` against this ST-Link V3.

The deliberate cost: **the bench binary requires an attached openocd.** Without one, the first `bkpt 0xAB` halts the core forever. That is acceptable precisely here — the bench is never shipped and runs by definition under the probe.

The spec's `--transport` flag survives in `run.py` with `semihost` as the default and single supported value; USB-CDC is documented in `bench/README.md` as the escape hatch if semihosting ever proves inadequate.

**2. The workload registry is a static table, not self-registration.** The spec says "registry order is execution order". Constructor-based self-registration across translation units has unspecified initialization order, which would make execution order depend on link order and silently reorder results between builds. Each `workloads_*.cpp` therefore exports a `const Workload[]` array plus its count, and `main.cpp` runs the three arrays in a fixed order. The spec's real property — "a new workload is one table row" — is preserved exactly.

**3. Family 1's "Oliverb solo" doubles as family 3's SRAM reverb.** The spec's family 3 wants an Oliverb SRAM-vs-SDRAM A/B, and `AmbientReverb` carries its 128 KB buffer as an inline member — so an A/B means two 128 KB objects. `alt_sram.lds` gives the `.bss` region (`SRAM`, `0x24040000`) only **256 KB**, and the SDRAM grain proxy wants an SRAM counterpart too. Rather than allocate two SRAM reverbs, there is exactly **one** SRAM `AmbientReverb` (used by family 1's solo workload *and* as the A side of family 3's pair) and one `DSY_SDRAM_BSS` one. Total SRAM demand for bench-owned buffers: 128 KB reverb + 64 KB arena = 192 KB. Task 5 verifies this against the map file and carries the documented fallback.

---

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `bench/Makefile` | libDaisy-pattern build: engine sources + full DaisySP, BOOT_SRAM linkage, git hash baked in | Create |
| `bench/main.cpp` | Entry point: hardware init, header line, offline sweep over the three tables, anchor mode, footer line | Create |
| `bench/cycles.h` | DWT cycle counter enable/read (header-only) | Create |
| `bench/workload.h` | `Workload` struct, `Result` struct, run constants, the three table externs | Create |
| `bench/runner.cpp` | Warm-up + measured loop, checksum fold, 10×-budget abort | Create |
| `bench/report.h` / `bench/report.cpp` | Logger setup and the `BENCH_*` / `ANCHOR` line formatting | Create |
| `bench/mem.h` / `bench/mem.cpp` | The SRAM arena, the SDRAM arena, the two `AmbientReverb` instances, the `FxMem` echo buffers | Create |
| `bench/workloads_system.cpp` | Family 1 — own system, decomposed | Create |
| `bench/workloads_daisysp.cpp` | Family 2 — nine DaisySP candidates + a bare-MorphOsc component reference | Create |
| `bench/workloads_memory.cpp` | Family 3 — SRAM/SDRAM A/B proxies | Create |
| `bench/anchor.cpp` | Anchor mode: real `AudioCallback` + `CpuLoadMeter` over three reference workloads | Create |
| `bench/openocd/spotykach-sram.cfg` | openocd Tcl: halt, load ELF into SRAM, set VTOR/MSP/PC, resume | Create |
| `bench/run.py` | The one command: build → load → capture → parse → write result files | Create |
| `bench/README.md` | How to run it, what the columns mean, what the fallbacks are | Create |
| `docs/bench/` | Committed results (`YYYY-MM-DD-<shorthash>.md` + `.csv`) | Create (dir) |
| `docs/roadmap.md` | Record the bench and the headline numbers | Modify |
| `docs/superpowers/specs/2026-07-18-sampler-texture-deck-design.md` | Its SDRAM caveat gets the measured number | Modify |

The residency-repo research document is updated in Task 8 and lives outside this repository:
`C:\Users\bernd\Documents\AI\Synthux Design Residency\docs\superpowers\specs\2026-07-18-spotykach-engine-expansion-research.md`.

---

### Task 1: Bring-up — submodules, a bench app that boots, and a probe that runs it

The whole plan rests on one unproven claim: that a debug probe can load a BOOT_SRAM image into the Seed's SRAM and start it, with no button dance. Task 1 proves exactly that and nothing else. The bench binary here prints two lines and stops. If this task fails, every later task is worthless — so it is deliberately its own gate.

**Files:**
- Create: `bench/Makefile`, `bench/main.cpp`, `bench/report.h`, `bench/report.cpp`, `bench/openocd/spotykach-sram.cfg`, `bench/run.py`
- Test: the Daisy Seed on the desk, with the debug probe attached to SWD. **One cable — the Seed's micro-USB is not needed.**

**Interfaces:**
- Consumes: nothing.
- Produces: `bench::logf(const char* fmt, ...)` and `bench::log_line(const char*)` (semihosting writers); `bench::report_begin(const char* githash)` and `bench::report_end()`; `bench/run.py --build-only`, `--no-build`, `--interface`, `--timeout`, `--transport`; the `BENCH_BEGIN` / `BENCH_END` marker contract every later task appends between.

- [ ] **Step 1: Initialise the submodules and build the libraries**

`lib/DaisySP` is checked out, but **`lib/libDaisy` is not initialised** — the directory is empty, so `bench/Makefile` cannot even parse until this runs (its `include $(SYSTEM_FILES_DIR)/Makefile` would fail). Note that libDaisy points at the **bleeptools fork**, not electro-smith; `--init` picks up the right one from `.gitmodules`.

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
git submodule update --init --recursive
ls lib/libDaisy/core/Makefile lib/DaisySP/Makefile
make libs
```

Expected: both paths exist after the update; `make libs` ends with `lib/libDaisy/build/libdaisy.a` and `lib/DaisySP/build/libdaisysp.a` present.

```bash
ls -l lib/libDaisy/build/libdaisy.a lib/DaisySP/build/libdaisysp.a
```

If `make libs` fails, stop and report the compiler error — do not work around it by patching submodule sources.

- [ ] **Step 2: Confirm the shipping firmware still builds (the untouched-baseline gate)**

```bash
make -j8 2>&1 | tail -5
```

Expected: a `build/spotykach.elf` / `.bin` and a size line. Record the reported `text`/`data`/`bss` figures in your report — the same command at the end of Task 8 must produce byte-identical output, proving the original firmware was never disturbed.

- [ ] **Step 3: Write the report layer**

Create `bench/report.h`:

```cpp
#pragma once

namespace bench {

// Output leaves over the SWD link the probe already owns (ARM semihosting),
// not over a second USB cable -- see the plan's deviation note 1. openocd
// services the breakpoint and prints the string on its own stdout.
//
// DELIBERATE CONSEQUENCE: this binary requires an attached openocd. Without
// one, the first bkpt 0xAB halts the core forever. Fine here -- the bench is
// never shipped and runs by definition under the probe.
void log_line(const char* s);
void logf(const char* fmt, ...);

// Marker contract, parsed by run.py. Anything printed outside these markers
// is free-form and ignored by the parser.
void report_begin(const char* githash);
void report_end();

} // namespace bench
```

Create `bench/report.cpp`:

```cpp
#include "report.h"
#include <cstdio>
#include <cstdarg>

namespace bench {
namespace {

// ARM semihosting SYS_WRITE0: r0 = op, r1 = pointer to a NUL-terminated
// string. The bkpt is the call. This is the whole transport.
constexpr int kSysWrite0 = 0x04;

inline void sh_write0(const char* s)
{
    register int         r0 asm("r0") = kSysWrite0;
    register const char* r1 asm("r1") = s;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
}

char g_buf[256];

} // namespace

void log_line(const char* s)
{
    sh_write0(s);
}

void logf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_buf, sizeof(g_buf), fmt, ap);
    va_end(ap);
    sh_write0(g_buf);
}

void report_begin(const char* githash)
{
    // Fixed measurement conditions, echoed so a result file is self-describing.
    logf("BENCH_BEGIN,%s,480000000,96,dcache+icache\n", githash);
}

void report_end()
{
    log_line("BENCH_END\n");
}

} // namespace bench
```

Every line this file emits ends in an explicit `\n` — `SYS_WRITE0` writes exactly the bytes it is given and adds nothing. A missing newline silently glues two result rows together, and `run.py`'s parser would drop both.

- [ ] **Step 4: Write the bench entry point**

Create `bench/main.cpp`:

```cpp
#include <daisy_seed.h>
#include "report.h"

#ifndef BENCH_GIT_HASH
#define BENCH_GIT_HASH "unknown"
#endif

static daisy::DaisySeed hw;

int main(void)
{
    hw.Init();                 // 480 MHz boost, caches on, SDRAM up
    hw.SetAudioBlockSize(96);
    hw.SetAudioSampleRate(daisy::SaiHandle::Config::SampleRate::SAI_48KHZ);

    // No logger start and no host handshake: semihosting writes are synchronous
    // through the probe, so the first line cannot be lost to enumeration timing.
    bench::report_begin(BENCH_GIT_HASH);
    bench::report_end();

    while (1) { hw.DelayMs(1000); }
}
```

- [ ] **Step 5: Write the bench Makefile**

Create `bench/Makefile`. Note the `../` prefixes: `make` runs from `bench/`, so every path is relative to that directory.

```make
# Standalone benchmark app. Never shipped -> full DaisySP (LGPL) is fine here.
# The shipping firmware is built by the ROOT Makefile and is untouched.

TARGET = bench

CPP_STANDARD = -std=c++17

LIBDAISY_DIR = ../lib/libDaisy
DAISYSP_DIR  = ../lib/DaisySP

# Full DaisySP, including the LGPL modules -- family 2 measures them.
USE_DAISYSP_LGPL = 1

# Same linkage as the shipping firmware, so the memory map the SRAM/SDRAM
# workloads observe is the map the firmware will actually have.
APP_TYPE = BOOT_SRAM
LDSCRIPT = ../alt_sram.lds

# The engine includes DaisySP headers relative to Source/ ("Effects/overdrive.h",
# "Filters/svf.h", "Utility/dsp.h"), so that directory has to be on the path.
C_INCLUDES = -I. -I../engine/ -I../third_party/ -I../lib/DaisySP/Source/
C_USR_FLAGS = -ffast-math -funroll-loops

GIT_HASH = $(shell git -C .. rev-parse --short HEAD)
C_DEFS += -DBENCH_GIT_HASH=\"$(GIT_HASH)\"

CPP_SOURCES = \
	main.cpp \
	report.cpp

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
```

- [ ] **Step 6: Build the bench app**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach/bench
make -j8 2>&1 | tail -20
```

Expected: `build/bench.elf` and a size summary. `text + data` must be well under 256 KB — that is the `SRAM_EXEC` region in `alt_sram.lds` (`0x24000000`, 256 K). If it already overflows here, stop and report; the RAM-load workflow is not viable and the spec's `BOOT_SRAM`-via-bootloader fallback has to be adopted before continuing.

- [ ] **Step 7: Confirm the debug probe**

The probe and its transport have **already been verified on this desk** — the values below are measured, not guessed. Do not re-derive them; just confirm the connection still stands.

The probe is an **ST-Link V3** (`STLINK V3J7M2`, `USB\VID_0483&PID_374E`), a composite of ST-Link Debug + mass storage + a virtual COM port. Two consequences, both already tested:

- The interface config is **`stlink-dap.cfg`**, not `cmsis-dap.cfg` and not `stlink.cfg`. Plain `stlink.cfg` auto-selects the **`hla_swd`** transport, under which `transport select swd` is an error and low-level control is taken over by the ST-Link firmware. `stlink-dap.cfg` gives a real DAP.
- The transport line is **`transport select dapdirect_swd`**.

Confirm:

```bash
openocd -s "C:/Program Files/DaisyToolchain/openocd/scripts" \
        -f interface/stlink-dap.cfg -f target/stm32h7x.cfg \
        -c "adapter speed 4000" -c "transport select dapdirect_swd" \
        -c "init" -c "halt" -c "read_memory 0x24000000 32 2" -c "resume" -c "shutdown"
```

Expected (this is the output already observed): `SWD DPIDR 0x6ba02477`, `Cortex-M7 r1p1 processor detected`, a halt line reporting `pc: 0x080014aa` (the Daisy bootloader, running from internal flash), and two words read back from the base of SRAM. Target voltage reads ~3.26 V.

Also already verified on this desk: openocd answers `arm semihosting enable` with `semihosting is enabled` against this probe. That is the capture path — neither the probe's own VCP (which would need the Seed's UART pins wired across) nor the Seed's micro-USB is used, so SWD is the only cable this bench needs.

- [ ] **Step 8: Write the openocd load script**

Create `bench/openocd/spotykach-sram.cfg`:

```tcl
# Load a BOOT_SRAM image straight into SRAM and start it -- no bootloader,
# no button dance, no flash wear. IMAGE is passed by run.py via -c "set IMAGE ...".

adapter speed 4000
# dapdirect_swd, NOT swd: this desk's probe is an ST-Link V3, and only
# stlink-dap.cfg gives a real DAP. Plain stlink.cfg would auto-select hla_swd,
# under which this line is an error.
transport select dapdirect_swd

init
reset halt

load_image $IMAGE

# The transport. Every bkpt 0xAB the firmware executes is serviced here and
# its string printed on openocd's own output, which run.py reads.
arm semihosting enable

# ENABLE THE FPU (CPACR, CP10+CP11 full access). Do not remove this line.
#
# A BOOT_SRAM image normally starts via the Daisy bootloader, and the
# bootloader is what performs the low-level CPU setup. Jumping straight to the
# app's reset vector -- which is exactly what this script does -- skips it, so
# the FPU stays off and the FIRST floating-point instruction takes a
# UsageFault (CFSR bit 19, NOCP) that escalates to HardFault (HFSR bit 30,
# FORCED). Symptom without this line: the core sits in HardFault_Handler
# forever and BENCH_BEGIN never appears. Diagnosed and fixed on hardware
# 2026-07-18.
mww 0xE000ED88 0x00F00000

# alt_sram.lds puts the vector table at the base of SRAM_EXEC. Point VTOR
# there, then take the initial MSP and reset vector out of it by hand --
# the bootloader normally does this.
set base 0x24000000
mww 0xE000ED08 $base
set sp [read_memory [expr {$base + 0}] 32 1]
set pc [read_memory [expr {$base + 4}] 32 1]
reg msp $sp
reg pc $pc

resume

# NO shutdown. openocd must stay attached for the whole run: it is both the
# semihosting server and the capture. run.py terminates it once BENCH_END
# arrives, or when its timeout expires.
```

- [ ] **Step 9: Write the host driver (bring-up version)**

Create `bench/run.py`. This version builds, loads and captures; parsing and result files land in Task 7.

```python
#!/usr/bin/env python3
"""Build the bench firmware, load it into the Seed's SRAM through the debug
probe, and capture its semihosting output. One command, one cable."""

import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
OPENOCD = r"C:\Program Files\DaisyToolchain\bin\openocd.exe"
SCRIPTS = r"C:\Program Files\DaisyToolchain\openocd\scripts"
ELF = os.path.join(HERE, "build", "bench.elf")


def build():
    subprocess.run(["make", "-j8"], cwd=HERE, check=True)


def run_once(interface, timeout):
    """Load and run the image, reading openocd's output until BENCH_END.

    openocd is both the loader and the semihosting server, so it stays alive
    for the whole run and its stdout IS the capture. Returns the captured
    lines, or None on timeout -- a hang writes nothing.
    """
    cmd = [
        OPENOCD,
        "-s", SCRIPTS,
        "-f", "interface/%s" % interface,
        "-f", "target/stm32h7x.cfg",
        "-c", "set IMAGE {%s}" % ELF.replace("\\", "/"),
        "-f", os.path.join(HERE, "openocd", "spotykach-sram.cfg"),
    ]
    # openocd logs to stderr and semihosting output can land on either --
    # merge them so no line is missed.
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, bufsize=1)
    deadline = time.time() + timeout
    lines, done = [], False
    try:
        # iter(readline) NOT `for raw in proc.stdout`: the latter uses Python's
        # read-ahead buffer, which on Windows blocks until the pipe fills and
        # deadlocks this loop. readline() returns per line. Verified on hardware.
        for raw in iter(proc.stdout.readline, ""):
            line = raw.rstrip("\r\n")
            print(line)
            lines.append(line)
            if line.startswith("BENCH_END"):
                done = True
                break
            if time.time() > deadline:
                break
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
    return lines if done else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--interface", default="stlink-dap.cfg",
                    help="openocd interface cfg; this desk's probe is an ST-Link V3")
    ap.add_argument("--transport", default="semihost", choices=["semihost"],
                    help="capture transport (USB-CDC fallback: see bench/README.md)")
    ap.add_argument("--timeout", type=float, default=300.0,
                    help="seconds to wait for BENCH_END")
    ap.add_argument("--build-only", action="store_true")
    ap.add_argument("--no-build", action="store_true")
    args = ap.parse_args()

    if not args.no_build:
        build()
    if args.build_only:
        return 0

    lines = run_once(args.interface, args.timeout)
    if lines is None:
        print("ERROR: BENCH_END never arrived (timeout or openocd exited)",
              file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 10: Run it on hardware**

No host dependencies beyond openocd — nothing to install.

```bash
cd /c/Users/bernd/Documents/AI/Spotykach/bench
python run.py
```

Expected: openocd's own banner and load chatter, then these two lines, then exit code 0.

```
BENCH_BEGIN,<shorthash>,480000000,96,dcache+icache
BENCH_END
```

**This exact sequence has already been run successfully on hardware by the controller** — the two lines above are the observed output, and both the FPU enable and the `readline()` capture were added to the files above precisely because their absence broke it. If it fails for you, something differs from that run; report the difference rather than reworking the approach.

Failure modes worth distinguishing before reporting:
- **`load_image` errors** — the ELF's load addresses do not match what the target accepts. Report the openocd error verbatim.
- **The core sits in `HardFault_Handler`** — read the fault status registers before theorising: `mdw 0xE000ED28 4` gives CFSR, HFSR, MMFAR, BFAR. `CFSR = 0x00080000` is UFSR bit 19 (NOCP) and means the FPU is off, i.e. the `mww 0xE000ED88 0x00F00000` line is missing or ineffective. Note that the PC reported in the halt line will be inside `HardFault_Handler` itself and tells you nothing — the faulting address is in the stacked exception frame.
- **The image loads but nothing is printed and there is no fault** — the app is not reaching `report_begin`. Add `-c "targets"` after `resume` and report the core state and PC. A PC in the `0x0800xxxx` range means it never left the bootloader.
- **openocd prints a line but `run.py` never sees it** — host-side stream buffering. Confirm by running the same openocd command by hand; if the line appears there, report it rather than adding sleeps.

- [ ] **Step 11: Commit**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
git add bench/
git commit -m "$(cat <<'EOF'
feat(bench): standalone bench app boots and talks, loaded via the debug probe

Own Makefile in the libDaisy pattern, own main, BOOT_SRAM linkage against the
firmware's alt_sram.lds. openocd writes the image straight into SRAM and sets
VTOR/MSP/PC by hand, so there is no bootloader button dance and no flash wear;
results come back over ARM semihosting on the same SWD link, so SWD is the only
cable involved and the host driver needs nothing beyond openocd.

Prints BENCH_BEGIN/BENCH_END and nothing else yet -- this commit exists to
prove the probe workflow before any measurement depends on it.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 2: The measurement core — DWT cycle counter, workload table, runner

The machinery that turns a callable into a row. It lands with two trivial workloads (an empty block and a fixed arithmetic load with a known cycle cost) so the runner itself can be sanity-checked before any engine code is behind it.

**Files:**
- Create: `bench/cycles.h`, `bench/workload.h`, `bench/runner.cpp`
- Modify: `bench/report.h`, `bench/report.cpp` (add the row line), `bench/main.cpp` (drive the table), `bench/Makefile` (new sources)
- Test: hardware run via `python run.py`

**Interfaces:**
- Consumes: `bench::report_begin/report_end` and the `bench::Log` alias from Task 1.
- Produces:
  - `struct bench::Workload { const char* family; const char* name; void (*setup)(); float (*process)(); }` — `process()` renders **exactly one block of 96 samples** and returns a value derived from its output.
  - `struct bench::Result { uint32_t avg_cyc; uint32_t max_cyc; uint32_t checksum; bool timed_out; }`
  - `bench::Result bench::run_workload(const Workload&)`
  - `void bench::report_row(const Workload&, const Result&)`
  - `constexpr size_t bench::kBlock = 96; constexpr uint32_t bench::kBudgetCycles = 960000;`
  - `extern const Workload bench::kCoreWorkloads[]; extern const int bench::kCoreCount;` — Family 1's table lives here from Task 3 on; Task 2 seeds it with the two sanity rows.

- [ ] **Step 1: Write the cycle counter**

Create `bench/cycles.h`:

```cpp
#pragma once
#include <cstdint>
#include <daisy_seed.h>   // pulls in the CMSIS core headers for CoreDebug/DWT

namespace bench {

// The Cortex-M7 DWT cycle counter. Free-running at the core clock (480 MHz),
// so one tick is one cycle and the block budget is a plain integer compare.
// The M7 gates DWT registers behind a lock; LAR must be unlocked first, which
// the M3/M4 examples floating around the web omit.
inline void cycles_init()
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->LAR = 0xC5ACCE55;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

inline uint32_t cycles_now()
{
    return DWT->CYCCNT;
}

} // namespace bench
```

If `DWT->LAR` does not compile, the CMSIS `DWT_Type` in this libDaisy revision omits the lock register. Do not delete the line — replace it with the direct write, which is the same thing:

```cpp
    *reinterpret_cast<volatile uint32_t*>(0xE0001FB0) = 0xC5ACCE55;
```

- [ ] **Step 2: Write the workload contract**

Create `bench/workload.h`:

```cpp
#pragma once
#include <cstdint>
#include <cstddef>

namespace bench {

// Fixed measurement conditions (spec "Measurement conditions").
constexpr size_t   kBlock        = 96;        // samples per process() call
constexpr float    kSampleRate   = 48000.f;
constexpr uint32_t kBudgetCycles = 960000;    // 96/48000 s at 480 MHz
constexpr int      kWarmupBlocks = 100;
constexpr int      kMeasBlocks   = 1000;

// A workload is one table row. process() renders exactly kBlock samples and
// returns a value derived from what it produced -- the runner folds that into
// a checksum, which is what keeps -ffast-math from deleting the whole thing.
struct Workload {
    const char* family;
    const char* name;
    void  (*setup)();
    float (*process)();
};

struct Result {
    uint32_t avg_cyc   = 0;
    uint32_t max_cyc   = 0;
    uint32_t checksum  = 0;
    bool     timed_out = false;
};

Result run_workload(const Workload& w);

// The three family tables. Static, not self-registering: table order is
// execution order and must not depend on link order (plan deviation 2).
extern const Workload kCoreWorkloads[];
extern const int      kCoreCount;

} // namespace bench
```

- [ ] **Step 3: Write the runner**

Create `bench/runner.cpp`:

```cpp
#include "workload.h"
#include "cycles.h"
#include <cstring>

namespace bench {

namespace {
// FNV-1a over the raw float bits. Exact, order-sensitive, and cheap enough
// to sit inside the measured loop without distorting it (a handful of cycles
// against a 960 000-cycle budget).
inline uint32_t fold(uint32_t h, float v)
{
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    return (h ^ bits) * 16777619u;
}
} // namespace

Result run_workload(const Workload& w)
{
    Result r;
    w.setup();

    // Warm-up: settle caches, envelopes, delay lines and any smoothing state
    // so the measured window sees steady-state cost, not attack transients.
    uint32_t h = 2166136261u;
    for (int i = 0; i < kWarmupBlocks; ++i) h = fold(h, w.process());

    uint64_t total = 0;                 // 1000 x up to 9.6M cycles overflows 32 bits
    for (int i = 0; i < kMeasBlocks; ++i) {
        const uint32_t t0 = cycles_now();
        const float v = w.process();
        const uint32_t dt = cycles_now() - t0;   // wraps correctly on unsigned
        h = fold(h, v);
        total += dt;
        if (dt > r.max_cyc) r.max_cyc = dt;
        if (dt > kBudgetCycles * 10u) {          // runaway: abort, do not hang
            r.timed_out = true;
            r.checksum = h;
            return r;
        }
    }

    r.avg_cyc  = static_cast<uint32_t>(total / kMeasBlocks);
    r.checksum = h;
    return r;
}

} // namespace bench
```

- [ ] **Step 4: Add the row line to the report layer**

Append to `bench/report.h`, inside `namespace bench`:

```cpp
struct Workload;
struct Result;

void report_row(const Workload& w, const Result& r);
```

and add the include at the top of the file, above the `namespace bench {`:

```cpp
#include "workload.h"
```

(then drop the two forward declarations again — `workload.h` defines both).

Append to `bench/report.cpp`:

```cpp
namespace {
// Percent of the block budget in hundredths, printed as a fixed-point pair.
// Integer maths keeps the output exact and keeps float formatting (and its
// newlib bulk) out of the binary.
inline uint32_t pct_x100(uint32_t cyc)
{
    return static_cast<uint32_t>((static_cast<uint64_t>(cyc) * 10000ull)
                                 / bench::kBudgetCycles);
}
} // namespace

namespace bench {

void report_row(const Workload& w, const Result& r)
{
    if (r.timed_out) {
        logf("BENCH,%s,%s,TIMEOUT,%lu,,,%08lx\n",
             w.family, w.name,
             (unsigned long)r.max_cyc, (unsigned long)r.checksum);
        return;
    }
    const uint32_t pa = pct_x100(r.avg_cyc);
    const uint32_t pm = pct_x100(r.max_cyc);
    logf("BENCH,%s,%s,%lu,%lu,%lu.%02lu,%lu.%02lu,%08lx\n",
         w.family, w.name,
         (unsigned long)r.avg_cyc, (unsigned long)r.max_cyc,
         (unsigned long)(pa / 100), (unsigned long)(pa % 100),
         (unsigned long)(pm / 100), (unsigned long)(pm % 100),
         (unsigned long)r.checksum);
}

} // namespace bench
```

- [ ] **Step 5: Seed the family-1 table with two sanity rows**

Create `bench/workloads_system.cpp`:

```cpp
#include "workload.h"
#include <cmath>

namespace bench {
namespace {

// --- baseline: the fixed cost of the harness itself -------------------------
void setup_empty() {}
float proc_empty()
{
    // Nothing but the call and the loop the runner wraps around it. Every
    // other row is read as "cost above this".
    return 0.f;
}

// --- calibration: a load whose cycle cost is knowable by hand ---------------
// 96 samples x one sinf each. Not a musical workload; it exists so a human can
// tell "the counter works" from "the counter reads zero".
float g_phase = 0.f;
void setup_sinf() { g_phase = 0.f; }
float proc_sinf()
{
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) {
        g_phase += 0.01f;
        acc += sinf(g_phase);
    }
    return acc;
}

} // namespace

const Workload kCoreWorkloads[] = {
    { "system", "empty_callback",  setup_empty, proc_empty },
    { "system", "sinf_x96",        setup_sinf,  proc_sinf  },
};
const int kCoreCount = sizeof(kCoreWorkloads) / sizeof(kCoreWorkloads[0]);

} // namespace bench
```

- [ ] **Step 6: Drive the table from main**

In `bench/main.cpp`, add the includes under `#include "report.h"`:

```cpp
#include "workload.h"
#include "cycles.h"
```

and replace the two-line body

```cpp
    bench::report_begin(BENCH_GIT_HASH);
    bench::report_end();
```

with:

```cpp
    bench::cycles_init();

    bench::report_begin(BENCH_GIT_HASH);
    for (int i = 0; i < bench::kCoreCount; ++i) {
        const bench::Workload& w = bench::kCoreWorkloads[i];
        bench::report_row(w, bench::run_workload(w));
    }
    bench::report_end();
```

- [ ] **Step 7: Add the new sources to the Makefile**

In `bench/Makefile`, replace the `CPP_SOURCES` block:

```make
CPP_SOURCES = \
	main.cpp \
	report.cpp \
	runner.cpp \
	workloads_system.cpp
```

- [ ] **Step 8: Run on hardware**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach/bench
python run.py
```

Expected, three lines between the markers:

```
BENCH_BEGIN,<shorthash>,480000000,96,dcache+icache
BENCH,system,empty_callback,<small>,<small>,0.00,0.00,<hex>
BENCH,system,sinf_x96,<n>,<n>,<p>,<p>,<hex>
BENCH_END
```

Three properties must hold before moving on, and all three are the point of this task:
- `empty_callback` avg is a **small non-zero** number (call + loop overhead, order of tens of cycles). Exactly `0` means the counter never started — go back to `cycles_init`.
- `sinf_x96` lands around **16 000 cycles, ~1.7 % of budget** — that is 96 calls at roughly 170 cycles each, which is ordinary for newlib's `sinf` on a Cortex-M7. (Measured 2026-07-18: avg 16360, max 16866. An earlier draft of this plan guessed "hundreds to low thousands" for the whole block; that was miscalibrated by an order of magnitude and is corrected here.) A value in the **millions** is the real failure signature — it means the DWT is counting something other than core cycles. Incidentally this row is the number behind `morph_osc.h`'s "no libm sinf — CPU budget constraint" note.
- Both checksums are non-zero, and a second `python run.py` reproduces them **identically**.

- [ ] **Step 9: Commit**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
git add bench/
git commit -m "$(cat <<'EOF'
feat(bench): DWT cycle counter, workload table and the offline runner

100 warm-up blocks then 1000 measured, avg/max cycles against the 960 000-cycle
block budget, FNV-1a checksum folded per block so -ffast-math cannot delete the
work, and a 10x-budget abort so a runaway workload is a TIMEOUT row instead of a
hung run.

Two sanity rows for now (empty block, 96 sinf) -- enough to tell a working
counter from a dead one before engine code hides behind it.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 3: Family 1 — the own-system table

The budget question. Eight rows that decompose the instrument, ending with the one number the whole exercise exists for: worst-case `Instrument::process`.

**Files:**
- Create: `bench/mem.h`, `bench/mem.cpp`
- Modify: `bench/workloads_system.cpp` (replace the sanity rows), `bench/Makefile` (engine sources + `mem.cpp`)
- Test: hardware run

**Interfaces:**
- Consumes: `bench::Workload`, `bench::run_workload`, `bench::kBlock`, `bench::kSampleRate` from Task 2.
- Produces:
  - `float* bench::sram_arena()` — 64 KB (16 384 floats), plain `.bss`, lands in `alt_sram.lds`'s `SRAM` region
  - `float* bench::sdram_arena()` — 8 MB (2 097 152 floats), `DSY_SDRAM_BSS`
  - `spky::AmbientReverb& bench::reverb_sram()` — the single SRAM reverb (family 1's solo row *and* family 3's A side, plan deviation 3)
  - `spky::AmbientReverb& bench::reverb_sdram()` — the `DSY_SDRAM_BSS` twin
  - `const spky::FxMem& bench::fx_mem()` — four `Flux::kMaxSamples` echo buffers in SDRAM plus `&reverb_sram()`
  - `const float* bench::test_input()` — `kBlock` samples of seeded noise, for workloads that consume input

- [ ] **Step 1: Write the memory arena**

Create `bench/mem.h`:

```cpp
#pragma once
#include "instrument.h"
#include "fx/reverb.h"

namespace bench {

// 64 KB. Comfortably past the H7's 32 KB D-cache, so an access pattern over it
// is honestly cache-hostile, while leaving room next to the 128 KB SRAM reverb
// inside alt_sram.lds's 256 KB SRAM region (plan deviation 3).
constexpr size_t kSramFloats  = 16 * 1024;
constexpr size_t kSdramFloats = 2 * 1024 * 1024;   // 8 MB

float* sram_arena();
float* sdram_arena();

spky::AmbientReverb& reverb_sram();
spky::AmbientReverb& reverb_sdram();

const spky::FxMem& fx_mem();
const float*       test_input();   // kBlock seeded-noise samples

} // namespace bench
```

Create `bench/mem.cpp`:

```cpp
#include "mem.h"
#include "workload.h"
#include <daisy_seed.h>

namespace bench {
namespace {

float  g_sram[kSramFloats];
float  DSY_SDRAM_BSS g_sdram[kSdramFloats];

spky::AmbientReverb g_rev_sram;
spky::AmbientReverb DSY_SDRAM_BSS g_rev_sdram;

// FLUX echo storage: 4 x 240 000 floats = 3.75 MB. SDRAM in the shipping
// firmware too -- 937 KB per channel does not fit anywhere else.
float DSY_SDRAM_BSS g_echo[2][2][spky::Flux::kMaxSamples];

float g_input[kBlock];
bool  g_input_ready = false;

spky::FxMem g_mem;
bool        g_mem_ready = false;

} // namespace

float* sram_arena()  { return g_sram; }
float* sdram_arena() { return g_sdram; }

spky::AmbientReverb& reverb_sram()  { return g_rev_sram; }
spky::AmbientReverb& reverb_sdram() { return g_rev_sdram; }

const spky::FxMem& fx_mem()
{
    if (!g_mem_ready) {
        for (int p = 0; p < 2; ++p) {
            g_mem.echo[p][0] = g_echo[p][0];
            g_mem.echo[p][1] = g_echo[p][1];
        }
        g_mem.reverb = &g_rev_sram;
        g_mem_ready = true;
    }
    return g_mem;
}

const float* test_input()
{
    if (!g_input_ready) {
        // Seeded LCG, not rand() -- the checksum is a determinism check and
        // must not depend on library state.
        uint32_t s = 22222u;
        for (size_t i = 0; i < kBlock; ++i) {
            s = s * 1664525u + 1013904223u;
            g_input[i] = (static_cast<float>(s >> 8) / 8388608.f) - 1.f;
        }
        g_input_ready = true;
    }
    return g_input;
}

} // namespace bench
```

- [ ] **Step 2: Write the family-1 table**

Replace the whole contents of `bench/workloads_system.cpp`:

```cpp
#include "workload.h"
#include "mem.h"
#include "instrument.h"
#include "mod/super_modulator.h"
#include "mod/lane_id.h"
#include "center/center.h"
#include "parts/part.h"
#include "synth/synth_engine.h"
#include "fx/part_fx.h"

namespace bench {
namespace {

using namespace spky;

float g_out_l[kBlock], g_out_r[kBlock];

// --- 1. baseline ------------------------------------------------------------
void setup_empty() {}
float proc_empty() { return 0.f; }

// --- 2. modulation plane only ----------------------------------------------
// Two SuperModulators plus the Center, no voices, no FX: the lanes budget the
// design spec estimates at 4-6 %.
//
// Center::update needs two Parts to write its hooks into, so two live here --
// but they are never process()ed. What this row measures is the mod plane and
// the control tick, not the parts.
SuperModulator g_mod_a, g_mod_b;
Center         g_center;
Part           g_hook_a, g_hook_b;

void setup_mod()
{
    g_mod_a.init(kSampleRate, 1u);
    g_mod_b.init(kSampleRate, 2u);
    g_center.init(kSampleRate, 11u);
    g_hook_a.init(kSampleRate, 1u);
    g_hook_b.init(kSampleRate, 2u);
    g_mod_a.set_rate(0.5f); g_mod_b.set_rate(0.6f);
    g_mod_a.set_density(0.7f); g_mod_b.set_density(0.7f);
}
float proc_mod()
{
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) {
        g_mod_a.process();
        g_mod_b.process();
        acc += g_mod_a.lane_output(LANE_PITCH) + g_mod_b.lane_output(LANE_PITCH);
    }
    // Control rate: one tick per Center::kCtrlInterval (96) samples, which is
    // exactly one per block. Running it per sample would measure a cadence the
    // firmware never has.
    g_center.update(g_mod_a, g_mod_b, g_hook_a, g_hook_b);
    return acc;
}

// --- 3-5. synth voices, 1 / 2 / 4 -------------------------------------------
// Does polyphony scale linearly? One SynthEngine, triggered often enough that
// the requested number of voices is genuinely sounding for the whole window.
SynthEngine g_synth;
int         g_synth_voices = 1;
int         g_trig_ctr = 0;

void setup_synth_n(int n)
{
    g_synth_voices = n;
    g_synth.set_seed(3u);
    g_synth.init(kSampleRate);
    g_synth.set_decay(1.f);       // 8x cycle: notes stay up, no silent blocks
    g_synth.set_cycle(2.f);
    g_synth.set_flow(false);
    g_trig_ctr = 0;
    for (int v = 0; v < n; ++v) g_synth.trigger(0.3f + 0.1f * v);
}
void setup_synth_1() { setup_synth_n(1); }
void setup_synth_2() { setup_synth_n(2); }
void setup_synth_4() { setup_synth_n(4); }

float proc_synth()
{
    float acc = 0.f, l, r;
    for (size_t i = 0; i < kBlock; ++i) {
        // Retrigger the whole set periodically so envelopes never all decay
        // out from under the measurement.
        if (++g_trig_ctr >= 24000) {
            g_trig_ctr = 0;
            for (int v = 0; v < g_synth_voices; ++v) g_synth.trigger(0.3f + 0.1f * v);
        }
        g_synth.process(l, r);
        acc += l + r;
    }
    return acc;
}

// --- 6. FX blocks, one at a time -------------------------------------------
// PartFx carries GRIT, FLUX and COMP; each row turns on exactly one so the
// 8-10 % FX estimate decomposes. `FxBlock` is an enum class with only Flux and
// Grit -- COMP is not a block, it is set_comp(amount) and bypasses bit-exactly
// at 0, so the selector here is a plain int, not an FxBlock.
enum FxSel { SEL_GRIT = 0, SEL_FLUX = 1, SEL_COMP = 2 };

PartFx g_fx;
float  g_fxv[FXT_COUNT];

void setup_fx(int sel)
{
    const FxMem& m = fx_mem();
    g_fx.init(kSampleRate, m.echo[0][0], m.echo[0][1]);
    // immediate = true: the soft switches would otherwise fade in over the
    // warm-up and the measured window would see a partly-engaged chain.
    g_fx.set_fx_on(FxBlock::Grit, sel == SEL_GRIT, true);
    g_fx.set_fx_on(FxBlock::Flux, sel == SEL_FLUX, true);
    g_fx.set_comp(sel == SEL_COMP ? 0.8f : 0.f);
    g_fx.set_grit_mix(1.f);
    g_fx.set_flux_mix(1.f);
    g_fx.set_bpm(120.f);

    // Already-modulated target values, as Part::fx_target_value() would hand
    // them over. Fixed here: this row measures the FX, not the modulation.
    g_fxv[FXT_GRIT_INT]  = 0.8f;
    g_fxv[FXT_FLUX_TIME] = 0.5f;
    g_fxv[FXT_FX_MIX]    = 1.f;
    g_fxv[FXT_REV_SEND]  = 0.5f;
    g_fxv[FXT_FLUX_FB]   = 0.7f;
}
void setup_fx_grit() { setup_fx(SEL_GRIT); }
void setup_fx_flux() { setup_fx(SEL_FLUX); }
void setup_fx_comp() { setup_fx(SEL_COMP); }

float proc_fx()
{
    const float* in = test_input();
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) {
        float l = in[i], r = in[i] * 0.9f, sl = 0.f, sr = 0.f;
        g_fx.process(l, r, sl, sr, g_fxv);
        acc += l + r + sl + sr;
    }
    return acc;
}

// --- 7. Oliverb solo --------------------------------------------------------
// The reverb question (estimate 15-25 %), and the first METER measurement the
// firmware-shell spec has been carrying as a TODO. Worst case: big room,
// blooming decay, dense diffusion, both LFOs up.
void setup_reverb()
{
    AmbientReverb& v = reverb_sram();
    v.init(kSampleRate);
    v.clear();
    v.set_size(0.9f);
    v.set_decay(0.95f);        // above the 1.0 loop-gain crossing: bloom
    v.set_tone(0.8f);
    v.set_diffusion(0.9f);
    v.set_diffuser_mod_depth(1.f);
    v.set_mod_depth(1.f);
}
float proc_reverb()
{
    const float* in = test_input();
    AmbientReverb& v = reverb_sram();
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) {
        float l, r;
        v.process(in[i], in[i] * 0.9f, l, r);
        acc += l + r;
    }
    return acc;
}

// --- 8-9. the whole instrument ---------------------------------------------
Instrument g_inst;

void setup_inst_common()
{
    g_inst.init(kSampleRate, fx_mem());
    g_inst.set_tempo_bpm(120.f);
}

// Init patch: the typical load.
void setup_inst_init()
{
    setup_inst_common();
}

// Worst case: 8 voices, 4-note COLOR on both parts, every FX block on, high
// diffusion, echo at maximum. THE number.
void setup_inst_worst()
{
    setup_inst_common();
    for (int p = 0; p < PART_COUNT; ++p) {
        g_inst.set_color(p, 1.f);          // 4-note chords -> 4 voices per part
        g_inst.set_density(p, 1.f);
        g_inst.set_depth(p, 1.f);
        g_inst.set_rate(p, 0.8f);
        g_inst.set_fx_on(p, FxBlock::Grit, true);
        g_inst.set_fx_on(p, FxBlock::Flux, true);
        g_inst.set_grit_mix(p, 1.f);
        g_inst.set_flux_mix(p, 1.f);
        g_inst.set_comp(p, 1.f);
        g_inst.set_voice_decay(p, 1.f);
        g_inst.trigger_manual(p);
    }
    g_inst.set_reverb_mix(0.5f);
    g_inst.set_reverb_size(1.f);
    g_inst.set_reverb_decay(0.95f);
    g_inst.set_reverb_diffusion(0.9f);
    g_inst.set_reverb_smear(1.f);
    g_inst.set_reverb_mod(1.f);
    g_inst.set_master_drive(1.f);
}

int g_inst_ctr = 0;
float proc_inst()
{
    const float* in = test_input();
    g_inst.process(in, in, g_out_l, g_out_r, kBlock);
    // Keep the voices busy: a fire every ~half second on both parts.
    if (++g_inst_ctr >= 250) {
        g_inst_ctr = 0;
        g_inst.trigger_manual(PART_A);
        g_inst.trigger_manual(PART_B);
    }
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) acc += g_out_l[i] + g_out_r[i];
    return acc;
}

} // namespace

const Workload kCoreWorkloads[] = {
    { "system", "empty_callback",     setup_empty,     proc_empty   },
    { "system", "mod_plane_2x_center",setup_mod,       proc_mod     },
    { "system", "synth_1_voice",      setup_synth_1,   proc_synth   },
    { "system", "synth_2_voices",     setup_synth_2,   proc_synth   },
    { "system", "synth_4_voices",     setup_synth_4,   proc_synth   },
    { "system", "fx_grit",            setup_fx_grit,   proc_fx      },
    { "system", "fx_flux_sdram",      setup_fx_flux,   proc_fx      },
    { "system", "fx_comp",            setup_fx_comp,   proc_fx      },
    { "system", "oliverb_solo_sram",  setup_reverb,    proc_reverb  },
    { "system", "instrument_init",    setup_inst_init, proc_inst    },
    { "system", "instrument_worst",   setup_inst_worst,proc_inst    },
};
const int kCoreCount = sizeof(kCoreWorkloads) / sizeof(kCoreWorkloads[0]);

} // namespace bench
```

Every signature above was read off the headers, but three are easy to get subtly wrong and worth re-confirming before you build:

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
grep -n "void update\|void init" engine/center/center.h
grep -n "void process\|void set_fx_on\|void set_comp" engine/fx/part_fx.h
grep -n "enum class FxBlock\|enum FxTargetId" -A 8 engine/fx/part_fx.h
```

Expected: `Center::update` takes **four** arguments (`SuperModulator&, SuperModulator&, Part&, Part&`), `Center::init` takes `(float, uint32_t)`, `PartFx::process` takes **five** (`float& l, float& r, float& send_l, float& send_r, const float* fxv`), and `FxBlock` is an `enum class` with exactly `Flux` and `Grit` — no `Comp` member. If any of that has moved, adapt the call and say so in your report; the *shape* of each workload is what matters, not the spelling.

The spec's matrix asks for a `synth 2x4 voices (both parts)` row. That is `instrument_worst` — COLOR at 1.0 gives 4-note chords on both parts, so all eight voices sound. A separate row would be a second, less honest version of the same measurement.

- [ ] **Step 3: Add the engine sources to the Makefile**

In `bench/Makefile`, replace the `CPP_SOURCES` block:

```make
CPP_SOURCES = \
	main.cpp \
	report.cpp \
	runner.cpp \
	mem.cpp \
	workloads_system.cpp \
	../engine/mod/lane.cpp \
	../engine/mod/super_modulator.cpp \
	../engine/parts/part.cpp \
	../engine/instrument.cpp \
	../engine/center/center.cpp \
	../engine/fx/grit.cpp \
	../engine/fx/flux.cpp \
	../engine/fx/reverb.cpp \
	../engine/fx/part_fx.cpp \
	../engine/fx/comp.cpp \
	../engine/synth/voice.cpp \
	../engine/synth/synth_engine.cpp
```

This is exactly the list `CMakeLists.txt` gives the `render` target, which is the definition of "the whole engine".

- [ ] **Step 4: Build and check the size**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach/bench
make -j8 2>&1 | tail -12
```

Expected: a size line. `text + data` must stay under 256 KB (`SRAM_EXEC`) and `bss` under 256 KB (`SRAM`). The 3.75 MB of echo buffers and the SDRAM reverb do **not** count toward either — they live in the SDRAM region. If `bss` overflows, report the map file's top consumers rather than guessing:

```bash
arm-none-eabi-nm --size-sort -S build/bench.elf | tail -20
```

- [ ] **Step 5: Run on hardware**

```bash
python run.py
```

Expected: eleven `BENCH,system,...` rows between the markers. Sanity-check the shape of the table before believing any single number:
- `empty_callback` stays near zero.
- `synth_1 < synth_2 < synth_4`, and the per-voice increment is roughly constant (measured 2026-07-18: ~41 000 cycles per voice, plus a fixed ~23 000 of shared control-rate work). **Each row must actually sound the number of voices its name claims** — assert it with `active_voices()`, do not assume. `SynthEngine::_do_trigger` allocates round-robin over *inactive* voices, and with a long decay none ever frees, so a naive retrigger loop silently *accumulates* voices and makes `synth_1` drift toward 4.
- `instrument_worst > instrument_init`, and `instrument_worst` exceeds the corrected component sum by a modest margin, not a factor. The correct sum is:

  `mod_plane + 2×synth_4 + 2×(fx_grit + fx_flux + fx_comp) + oliverb_solo`

  — the mod plane counts, and the FX rows count **twice** because both parts run their own chain. (An earlier draft omitted the mod plane and counted FX once; that inflated the ratio to a spurious 2.06× and read as an anomaly. With the correct sum the measured ratio is ~1.23×, the residual being `Part::process`'s per-sample chord refresh, the quantizer, the limiter and MORPH.)
- No row is `TIMEOUT`.
- Every checksum is non-zero, and a second run reproduces all eleven identically.

If `instrument_worst` comes in *below* `oliverb_solo_sram`, the reverb is not actually running inside the instrument — check `set_reverb_mix` and `reverb_asleep()` before recording the number.

- [ ] **Step 6: Commit**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
git add bench/
git commit -m "$(cat <<'EOF'
feat(bench): family 1 -- the own-system table, decomposed

Eleven rows from an empty block to the worst case: mod plane, synth polyphony
1/2/4, GRIT/FLUX/COMP one at a time, Oliverb solo (the first METER measurement
the firmware-shell spec has been owing), init patch, and the full instrument at
8 voices with every FX up.

Memory arena lands with it: 64 KB SRAM, 8 MB SDRAM, one SRAM reverb shared with
family 3's A side, and the 3.75 MB of FLUX echo buffers in SDRAM.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 4: Family 2 — DaisySP candidates against our own voice

Nine expansion candidates, each as one sounding voice, plus a bare `MorphOsc` as a component reference. The point is not the absolute numbers; it is that every candidate reads as a multiple of **one real spotymod voice** — and that anchor is family 1's `synth_1_voice` row, not the bare oscillator. A real `Voice` (`engine/synth/voice.cpp`) drives two `MorphOsc` in unison plus a sub-oscillator, an SVF and an envelope, costing about **7.3×** a bare kernel. Anchoring on the kernel inflates every ratio by that factor and misranks the table.

**Files:**
- Create: `bench/workloads_daisysp.cpp`
- Modify: `bench/workload.h` (the second table extern), `bench/main.cpp` (run it), `bench/Makefile` (new source)
- Test: hardware run

**Interfaces:**
- Consumes: `bench::Workload`, `bench::kBlock`, `bench::kSampleRate`.
- Produces: `extern const Workload bench::kVoiceWorkloads[]; extern const int bench::kVoiceCount;`

- [ ] **Step 1: Verify the DaisySP class names and signatures**

The submodule is only present as of Task 1, and this is the one table built entirely from code nobody in this repo has called before. Check before writing:

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
grep -rn "class ModalVoice\|class StringVoice\|class Resonator" lib/DaisySP/Source/PhysicalModeling/
grep -rn "class FormantOscillator\|class VosimOscillator\|class HarmonicOscillator\|class GrainletOscillator\|class ZOscillator\|class VariableShapeOscillator" lib/DaisySP/Source/Synthesis/
grep -n "void Init\|float Process\|void SetFreq\|void Trig" lib/DaisySP/Source/PhysicalModeling/modalvoice.h
```

Record the actual `Init` / `Process` / setter signatures. The table below is written to the DaisySP API as of the vendored revision; where a signature differs, **adapt the call and note it in your report** — do not drop the row.

- [ ] **Step 2: Write the candidate table**

Create `bench/workloads_daisysp.cpp`:

```cpp
#include "workload.h"
#include "daisysp.h"
#include "synth/morph_osc.h"
#include "synth/env.h"

namespace bench {
namespace {

using namespace daisysp;

// Every candidate runs as ONE sounding voice at one pitch, retriggered often
// enough that it never falls silent inside the measured window. Answers the
// engine-expansion research's open question 1 with cycles instead of analogies.
constexpr float kFreq = 220.f;
constexpr int   kTrigEvery = 12000;   // 0.25 s

int g_ctr = 0;

// --- component reference: ONE BARE MorphOsc ---------------------------------
// NOT the anchor for "our voice" -- read the note below. This row exists to
// price a single oscillator kernel, so the six Plaits oscillator candidates
// (which are also bare kernels) have a like-for-like comparison.
//
// A real spotymod Voice is ~7.3x this: engine/synth/voice.cpp drives TWO
// MorphOsc instances in unison plus a sub-oscillator, an SVF and an envelope.
// The decision-relevant anchor is family 1's `synth_1_voice` row, which
// measures exactly that full pipeline. Ratios that steer engine selection are
// computed against THAT, not against this row.
spky::MorphOsc g_morph;

void setup_morph()
{
    g_morph.init(kSampleRate);
    g_morph.set_freq(kFreq);
    g_morph.set_morph(0.5f);
}
float proc_morph()
{
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) acc += g_morph.process();
    return acc;
}

// --- physical modelling -----------------------------------------------------
ModalVoice  g_modal;
StringVoice g_string;
Resonator   g_reso;

void setup_modal()
{
    g_modal.Init(kSampleRate);
    g_modal.SetFreq(kFreq);
    g_modal.SetStructure(0.6f);
    g_modal.SetBrightness(0.6f);
    g_modal.SetDamping(0.5f);
    g_ctr = 0;
}
float proc_modal()
{
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) {
        if (++g_ctr >= kTrigEvery) { g_ctr = 0; g_modal.Trig(); }
        acc += g_modal.Process();
    }
    return acc;
}

void setup_string()
{
    g_string.Init(kSampleRate);
    g_string.SetFreq(kFreq);
    g_string.SetBrightness(0.6f);
    g_string.SetDamping(0.6f);
    g_ctr = 0;
}
float proc_string()
{
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) {
        if (++g_ctr >= kTrigEvery) { g_ctr = 0; g_string.Trig(); }
        acc += g_string.Process();
    }
    return acc;
}

void setup_reso()
{
    g_reso.Init(0.3f, 24, kSampleRate);
    g_reso.SetFreq(kFreq);
    g_reso.SetStructure(0.6f);
    g_reso.SetBrightness(0.6f);
    g_reso.SetDamping(0.5f);
    g_ctr = 0;
}
float proc_reso()
{
    // Resonator is excited, not triggered: feed it a short impulse train.
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) {
        float ex = 0.f;
        if (++g_ctr >= kTrigEvery) { g_ctr = 0; ex = 1.f; }
        acc += g_reso.Process(ex);
    }
    return acc;
}

// --- Plaits-derived oscillator kernels --------------------------------------
FormantOscillator       g_formant;
VosimOscillator         g_vosim;
HarmonicOscillator      g_harm;
GrainletOscillator      g_grainlet;
ZOscillator             g_zosc;
VariableShapeOscillator g_varshape;

void setup_formant()
{
    g_formant.Init(kSampleRate);
    g_formant.SetFormantFreq(600.f);
    g_formant.SetCarrierFreq(kFreq);
    g_formant.SetPhaseShift(0.3f);
}
float proc_formant()
{
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) acc += g_formant.Process();
    return acc;
}

void setup_vosim()
{
    g_vosim.Init(kSampleRate);
    g_vosim.SetFreq(kFreq);
    g_vosim.SetForm1Freq(600.f);
    g_vosim.SetForm2Freq(900.f);
    g_vosim.SetShape(0.5f);
}
float proc_vosim()
{
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) acc += g_vosim.Process();
    return acc;
}

void setup_harm()
{
    g_harm.Init(kSampleRate);
    g_harm.SetFreq(kFreq);
    g_harm.SetFirstHarmIdx(1);
    for (int i = 0; i < 8; ++i) g_harm.SetSingleAmp(0.5f, i);
}
float proc_harm()
{
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) acc += g_harm.Process();
    return acc;
}

void setup_grainlet()
{
    g_grainlet.Init(kSampleRate);
    g_grainlet.SetFreq(kFreq);
    g_grainlet.SetFormantFreq(600.f);
    g_grainlet.SetShape(0.5f);
    g_grainlet.SetBleed(0.3f);
}
float proc_grainlet()
{
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) acc += g_grainlet.Process();
    return acc;
}

void setup_zosc()
{
    g_zosc.Init(kSampleRate);
    g_zosc.SetFreq(kFreq);
    g_zosc.SetFormantFreq(600.f);
    g_zosc.SetShape(0.5f);
    g_zosc.SetMode(0.5f);
}
float proc_zosc()
{
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) acc += g_zosc.Process();
    return acc;
}

void setup_varshape()
{
    g_varshape.Init(kSampleRate);
    g_varshape.SetFreq(kFreq);
    g_varshape.SetSync(false);
    g_varshape.SetPW(0.5f);
    g_varshape.SetWaveshape(0.5f);
}
float proc_varshape()
{
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) acc += g_varshape.Process();
    return acc;
}

} // namespace

const Workload kVoiceWorkloads[] = {
    { "voice", "morph_osc_bare",       setup_morph,    proc_morph    },
    { "voice", "modal_voice",          setup_modal,    proc_modal    },
    { "voice", "string_voice",         setup_string,   proc_string   },
    { "voice", "resonator",            setup_reso,     proc_reso     },
    { "voice", "formant_osc",          setup_formant,  proc_formant  },
    { "voice", "vosim_osc",            setup_vosim,    proc_vosim    },
    { "voice", "harmonic_osc",         setup_harm,     proc_harm     },
    { "voice", "grainlet_osc",         setup_grainlet, proc_grainlet },
    { "voice", "z_osc",                setup_zosc,     proc_zosc     },
    { "voice", "variable_shape_osc",   setup_varshape, proc_varshape },
};
const int kVoiceCount = sizeof(kVoiceWorkloads) / sizeof(kVoiceWorkloads[0]);

} // namespace bench
```

Check the MorphOsc API too — it is ours, but this is its first use outside `Voice`:

```bash
grep -n "void init\|float process\|void set_" engine/synth/morph_osc.h
```

- [ ] **Step 3: Declare the table and run it**

In `bench/workload.h`, add below the `kCoreWorkloads` externs:

```cpp
extern const Workload kVoiceWorkloads[];
extern const int      kVoiceCount;
```

In `bench/main.cpp`, add a second loop after the family-1 loop, before `report_end()`:

```cpp
    for (int i = 0; i < bench::kVoiceCount; ++i) {
        const bench::Workload& w = bench::kVoiceWorkloads[i];
        bench::report_row(w, bench::run_workload(w));
    }
```

In `bench/Makefile`, add `workloads_daisysp.cpp` to `CPP_SOURCES` after `workloads_system.cpp`.

- [ ] **Step 4: Build and run**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach/bench
make -j8 2>&1 | tail -12 && python run.py
```

Expected: ten `BENCH,voice,...` rows after the eleven `system` rows. What to check:
- `morph_osc_bare` is a small fraction of a percent — one oscillator kernel against a 960 000-cycle budget (measured 2026-07-18: 8142 cycles, 0.84 %).
- No row is `TIMEOUT`. Judge outliers against **`synth_1_voice`** (one real voice, ~59 500 cycles), not against the bare kernel. `modal_voice` and `resonator` land near 6× a real voice, and that is genuine, not a bug: `Resonator` computes `powf`/`cos` per sample across a 24-mode bank, and 24 is `kMaxNumModes` — the class ceiling, which DaisySP's own `ModalVoice` also uses. A candidate at 100× a real voice would be a setup bug (a filter self-oscillating into denormals, a frequency left at zero) — investigate before recording it.
- Every checksum is non-zero. A checksum of `00000000` on a candidate row means it produced silence for the whole window: its trigger or excitation never fired. Fix it; a silent voice is not a measurement.

- [ ] **Step 5: Commit**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
git add bench/
git commit -m "$(cat <<'EOF'
feat(bench): family 2 -- nine DaisySP candidates against our own voice

ModalVoice, StringVoice, Resonator and the six Plaits-derived oscillator kernels,
each as one sounding voice, plus a spotymod MorphOsc voice as the anchor row so
every candidate reads as a multiple of "our voice".

Turns the engine-expansion research's open question 1 from an argument about
analogies into a table.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 5: Family 3 — SRAM vs SDRAM, the M5 risk quantified

Three A/B pairs. The grain-read proxy is the one that matters: it puts a number on the sampler's SDRAM exposure before the sampler exists.

**Files:**
- Create: `bench/workloads_memory.cpp`
- Modify: `bench/workload.h` (third table extern), `bench/main.cpp` (run it), `bench/Makefile` (new source)
- Test: hardware run + a map-file check

**Interfaces:**
- Consumes: `bench::sram_arena()`, `bench::sdram_arena()`, `bench::reverb_sram()`, `bench::reverb_sdram()`, `bench::test_input()` from Task 3.
- Produces: `extern const Workload bench::kMemWorkloads[]; extern const int bench::kMemCount;`

- [ ] **Step 1: Write the memory table**

Create `bench/workloads_memory.cpp`:

```cpp
#include "workload.h"
#include "mem.h"
#include <cmath>

namespace bench {
namespace {

using namespace spky;

// --- grain-read proxy -------------------------------------------------------
// Eight scattered linear-interpolated stereo reads per sample. This is the
// access pattern a granular sampler has, without a sampler existing yet: the
// M5 texture deck's SDRAM exposure, measured in advance.
//
// The SRAM and SDRAM rows run the IDENTICAL pattern over identically-sized
// windows -- only the region changes, so the ratio is the region's factor and
// nothing else.
constexpr int    kGrains     = 8;
constexpr size_t kWindowFrames = kSramFloats / 2;   // stereo frames; same for both

float*   g_grain_buf = nullptr;
uint32_t g_grain_rng = 0;

void setup_grain(float* buf)
{
    g_grain_buf = buf;
    g_grain_rng = 99991u;
    for (size_t i = 0; i < kWindowFrames * 2; ++i)
        buf[i] = sinf(static_cast<float>(i) * 0.001f);
}
void setup_grain_sram()  { setup_grain(sram_arena()); }
void setup_grain_sdram() { setup_grain(sdram_arena()); }

float proc_grain()
{
    float acc = 0.f;
    for (size_t s = 0; s < kBlock; ++s) {
        for (int g = 0; g < kGrains; ++g) {
            // Scattered on purpose: a sequential walk would be prefetched and
            // would measure the prefetcher, not the memory.
            g_grain_rng = g_grain_rng * 1664525u + 1013904223u;
            const uint32_t idx = (g_grain_rng >> 8) % (kWindowFrames - 2);
            const float frac = static_cast<float>(g_grain_rng & 0xFFu) / 256.f;
            const float* p = g_grain_buf + idx * 2;
            acc += p[0] + (p[2] - p[0]) * frac;      // L, lerp
            acc += p[1] + (p[3] - p[1]) * frac;      // R, lerp
        }
    }
    return acc;
}

// --- Oliverb placement ------------------------------------------------------
// The SRAM side is family 1's oliverb_solo_sram row -- one 128 KB object, not
// two (plan deviation 3). This row is only the SDRAM twin; the M6 placement
// decision is the ratio between the two.
void setup_verb(AmbientReverb& v)
{
    v.init(kSampleRate);
    v.clear();
    v.set_size(0.9f);
    v.set_decay(0.95f);
    v.set_tone(0.8f);
    v.set_diffusion(0.9f);
    v.set_diffuser_mod_depth(1.f);
    v.set_mod_depth(1.f);
}
void setup_verb_sdram() { setup_verb(reverb_sdram()); }

float proc_verb_sdram()
{
    const float* in = test_input();
    AmbientReverb& v = reverb_sdram();
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) {
        float l, r;
        v.process(in[i], in[i] * 0.9f, l, r);
        acc += l + r;
    }
    return acc;
}

// --- echo-style streaming access -------------------------------------------
// FLUX's real buffer is 240 000 floats (937 KB per channel) and does not fit
// SRAM at any size, so this is a shortened window in BOTH regions: what is
// measured is the access-pattern factor, not the full delay length.
constexpr size_t kEchoFrames = kSramFloats;   // same window in both regions

float*   g_echo_buf = nullptr;
size_t   g_echo_w = 0;

void setup_echo(float* buf)
{
    g_echo_buf = buf;
    g_echo_w = 0;
    for (size_t i = 0; i < kEchoFrames; ++i) buf[i] = 0.f;
}
void setup_echo_sram()  { setup_echo(sram_arena()); }
void setup_echo_sdram() { setup_echo(sdram_arena()); }

float proc_echo()
{
    const float* in = test_input();
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) {
        // One read a long way behind the write head, one write at it: the
        // streaming pattern a delay line has.
        const size_t rd = (g_echo_w + kEchoFrames - (kEchoFrames * 3 / 4)) % kEchoFrames;
        const float out = g_echo_buf[rd];
        g_echo_buf[g_echo_w] = in[i] + out * 0.6f;
        g_echo_w = (g_echo_w + 1) % kEchoFrames;
        acc += out;
    }
    return acc;
}

} // namespace

const Workload kMemWorkloads[] = {
    { "mem", "grain_read_sram",  setup_grain_sram,  proc_grain      },
    { "mem", "grain_read_sdram", setup_grain_sdram, proc_grain      },
    { "mem", "oliverb_sdram",    setup_verb_sdram,  proc_verb_sdram },
    { "mem", "echo_walk_sram",   setup_echo_sram,   proc_echo       },
    { "mem", "echo_walk_sdram",  setup_echo_sdram,  proc_echo       },
};
const int kMemCount = sizeof(kMemWorkloads) / sizeof(kMemWorkloads[0]);

} // namespace bench
```

The grain and echo rows share the same arenas, which is safe because workloads run strictly sequentially and every `setup` refills its window from scratch.

- [ ] **Step 2: Declare the table and run it**

In `bench/workload.h`, add below the `kVoiceWorkloads` externs:

```cpp
extern const Workload kMemWorkloads[];
extern const int      kMemCount;
```

In `bench/main.cpp`, add a third loop after the family-2 loop, before `report_end()`:

```cpp
    for (int i = 0; i < bench::kMemCount; ++i) {
        const bench::Workload& w = bench::kMemWorkloads[i];
        bench::report_row(w, bench::run_workload(w));
    }
```

In `bench/Makefile`, add `workloads_memory.cpp` to `CPP_SOURCES` after `workloads_daisysp.cpp`.

- [ ] **Step 3: Verify the SRAM region actually holds both buffers**

The spec flags this as an assumption; here is where it gets checked. `alt_sram.lds` gives `.bss` a 256 KB `SRAM` region at `0x24040000`. Bench-owned demand there is 128 KB (`g_rev_sram`) + 64 KB (`g_sram`) = 192 KB, plus libDaisy's own statics.

```bash
cd /c/Users/bernd/Documents/AI/Spotykach/bench
make -j8 2>&1 | tail -12
arm-none-eabi-nm --size-sort -S build/bench.elf | tail -15
```

Expected: the build succeeds with no `region SRAM overflowed` error, and the size listing shows `g_rev_sram` at `0x20000` (128 KB) and `g_sram` at `0x10000` (64 KB).

**If `SRAM` overflows:** do not shrink `AmbientReverb` — its buffer size is a `third_party` constant this plan may not touch. Halve `kSramFloats` to `8 * 1024` (32 KB) in `bench/mem.h` instead and re-run. The grain proxy's SRAM window shrinks with it, and because `kWindowFrames` is derived from `kSramFloats` the SDRAM side shrinks identically — the pair stays same-size, so the ratio stays valid. Note the change in your report; the result file's verdict must say which window size was used.

- [ ] **Step 4: Run on hardware**

```bash
python run.py
```

Expected: five `BENCH,mem,...` rows. The findings to read out and put in your report:
- `grain_read_sdram / grain_read_sram` — **the M5 number.** Expect a factor well above 1. A factor of ~1.0 means the SDRAM reads are being served from D-cache: the window is too small or the scatter is too local. Report it rather than accepting it, because a factor of 1.0 is the one result that would be too good to be true.
- `oliverb_sdram` vs family 1's `oliverb_solo_sram` — the M6 placement decision.
- `echo_walk_sdram / echo_walk_sram` — the FLUX placement factor, on a shortened window.

- [ ] **Step 5: Commit**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
git add bench/
git commit -m "$(cat <<'EOF'
feat(bench): family 3 -- SRAM vs SDRAM, same pattern both sides

Grain-read proxy (8 scattered interpolated stereo reads per sample) in both
regions over identically-sized windows, the Oliverb SDRAM twin against family
1's SRAM solo, and a shortened echo-style streaming walk in both.

Puts a number on the M5 texture deck's SDRAM exposure before the sampler exists,
and on the M6 reverb placement decision.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 6: Anchor mode — the offline numbers, checked against a real callback

Offline DWT measurement is optimistic by construction: no interrupt overhead, no DMA contention on SDRAM, and no proof the audio actually sounds. Anchor mode runs three reference workloads inside a genuine `AudioCallback` with `CpuLoadMeter` and prints the offset.

**Files:**
- Create: `bench/anchor.cpp`
- Modify: `bench/report.h`, `bench/report.cpp` (the `ANCHOR` line), `bench/main.cpp` (call it), `bench/Makefile` (new source), `bench/run.py` (raise the default timeout)
- Test: hardware run — **with headphones or monitors on the Seed's outputs**

**Interfaces:**
- Consumes: the family-1 and family-3 setup/process functions, re-exposed for reuse.
- Produces:
  - `void bench::run_anchors(daisy::DaisySeed& hw)`
  - `void bench::report_anchor(const char* name, uint32_t avg_x100, uint32_t max_x100)`
  - In `bench/workload.h`: `const Workload* bench::find_workload(const char* name)` — looks a row up across all three tables by name.

- [ ] **Step 1: Add the lookup helper**

Append to `bench/workload.h`, inside `namespace bench`:

```cpp
// Anchor mode re-runs rows the offline tables already define, by name.
const Workload* find_workload(const char* name);
```

Append to `bench/runner.cpp`, inside `namespace bench`:

```cpp
const Workload* find_workload(const char* name)
{
    const Workload* tables[] = { kCoreWorkloads, kVoiceWorkloads, kMemWorkloads };
    const int       counts[] = { kCoreCount,     kVoiceCount,     kMemCount     };
    for (int t = 0; t < 3; ++t)
        for (int i = 0; i < counts[t]; ++i)
            if (std::strcmp(tables[t][i].name, name) == 0) return &tables[t][i];
    return nullptr;
}
```

- [ ] **Step 2: Add the anchor line to the report layer**

Append to `bench/report.h`, inside `namespace bench`:

```cpp
void report_anchor(const char* name, uint32_t avg_x100, uint32_t max_x100);
```

Append to `bench/report.cpp`, inside `namespace bench`:

```cpp
void report_anchor(const char* name, uint32_t avg_x100, uint32_t max_x100)
{
    logf("ANCHOR,%s,%lu.%02lu,%lu.%02lu\n",
         name,
         (unsigned long)(avg_x100 / 100), (unsigned long)(avg_x100 % 100),
         (unsigned long)(max_x100 / 100), (unsigned long)(max_x100 % 100));
}
```

- [ ] **Step 3: Write anchor mode**

Create `bench/anchor.cpp`:

```cpp
#include "workload.h"
#include "report.h"
#include <daisy_seed.h>

namespace bench {
namespace {

using namespace daisy;

// The three rows worth cross-checking. ORDER MATTERS: the two that fit inside
// the block budget run first and sound clean; instrument_worst runs LAST
// because it does not fit and will sound broken -- see the note below.
const char* kAnchorNames[] = {
    "oliverb_solo_sram",
    "grain_read_sdram",
    "instrument_worst",
};
constexpr int kAnchorCount = 3;
constexpr int kAnchorSeconds = 2;

CpuLoadMeter    g_meter;
const Workload* g_current = nullptr;

// THE CALLBACK LIMITS ITSELF. Do not go back to bounding the segment with a
// foreground hw.DelayMs().
//
// instrument_worst costs ~160 % of the block budget, so inside a real callback
// the ISR never finishes before the next block is due and the CPU is saturated
// in interrupt context. A foreground delay is then starved by the very
// workload it is meant to time out: a "4 second" segment ran for minutes,
// emitting continuous DMA garbage at the outputs. (Observed on hardware
// 2026-07-18. The noise IS the 160 % result, made audible.)
//
// Counting blocks in the callback and early-returning silence once the limit
// is hit frees the CPU immediately, so the foreground can proceed even after
// total starvation.
volatile uint32_t g_blocks = 0;
volatile bool     g_done   = false;
uint32_t          g_block_limit = 0;

void AnchorCallback(AudioHandle::InputBuffer,
                    AudioHandle::OutputBuffer out, size_t size)
{
    if (g_done) {                       // limit reached: cheap, silent, done
        for (size_t i = 0; i < size; ++i) { out[0][i] = 0.f; out[1][i] = 0.f; }
        return;
    }

    g_meter.OnBlockStart();

    const float v = g_current ? g_current->process() : 0.f;

    // Audible proof that the workload really computes: the block's checksum
    // value, heavily attenuated, as a click/tone on both outputs. Not music --
    // it is there so a dead workload can be HEARD as silence.
    const float mon = v * 0.0005f;
    for (size_t i = 0; i < size; ++i) {
        out[0][i] = mon;
        out[1][i] = mon;
    }

    g_meter.OnBlockEnd();

    if (++g_blocks >= g_block_limit) g_done = true;
}

} // namespace

void run_anchors(DaisySeed& hw)
{
    g_meter.Init(kSampleRate, kBlock);

    for (int a = 0; a < kAnchorCount; ++a) {
        const Workload* w = find_workload(kAnchorNames[a]);
        if (!w) continue;

        w->setup();
        g_current = w;
        g_meter.Reset();

        // Blocks, not milliseconds. An over-budget workload stretches wall
        // clock, so a fixed block count is the only bound it cannot starve.
        g_block_limit = (uint32_t)((kAnchorSeconds * kSampleRate) / kBlock);
        g_blocks = 0;
        g_done   = false;

        hw.StartAudio(AnchorCallback);
        while (!g_done) { }             // callback self-limits; see the note
        hw.StopAudio();

        // Print only AFTER the audio has stopped. This is not a nicety: a
        // semihosting write HALTS THE CORE while openocd services it, so a
        // print inside or alongside a running callback would not merely skew
        // the measurement, it would break the audio outright.
        report_anchor(kAnchorNames[a],
                      (uint32_t)(g_meter.GetAvgCpuLoad() * 10000.f),
                      (uint32_t)(g_meter.GetMaxCpuLoad() * 10000.f));
    }
    g_current = nullptr;
}

} // namespace bench
```

Two API names to confirm against the vendored libDaisy before building, since a wrong guess here fails at link time with a confusing message:

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
grep -n "Reset\|GetAvgCpuLoad\|GetMaxCpuLoad\|OnBlockStart" lib/libDaisy/src/hid/*.h lib/libDaisy/src/util/*.h | head
grep -n "StartAudio\|StopAudio" lib/libDaisy/src/daisy_seed.h lib/libDaisy/src/sys/system.h | head
```

If `CpuLoadMeter` has no `Reset()`, re-`Init()` it before each anchor instead — same effect.

- [ ] **Step 4: Call it from main**

In `bench/main.cpp`, add the declaration under the includes:

```cpp
namespace bench { void run_anchors(daisy::DaisySeed& hw); }
```

and insert the call between the family-3 loop and `report_end()`:

```cpp
    // Offline is optimistic: no ISR overhead, no DMA contention. Three rows
    // re-run inside a real callback calibrate the offset -- and audibly.
    bench::run_anchors(hw);
```

In `bench/Makefile`, add `anchor.cpp` to `CPP_SOURCES`.

In `bench/run.py`, raise the default timeout — three 4-second anchor segments sit on top of the offline sweep:

```python
    ap.add_argument("--timeout", type=float, default=600.0,
                    help="seconds to wait for BENCH_END")
```

- [ ] **Step 5: Run on hardware, with monitoring connected**

Connect headphones or monitors to the Seed's audio outputs **at low volume** before running — the monitor signal is attenuated but the reverb anchor blooms.

```bash
cd /c/Users/bernd/Documents/AI/Spotykach/bench
make -j8 2>&1 | tail -12 && python run.py
```

Expected: three `ANCHOR,...` lines after all the `BENCH,` rows, and audibly *something* on the outputs during each 2-second segment, in this order — a wash for the reverb, a noisy scatter for the grain proxy, and then **broken, glitching output for `instrument_worst`**.

That third segment sounding wrong is the *correct* result, not a failure: at ~160 % of block budget the callback cannot complete before the next block is due, so the DAC is fed underrun garbage. It is the offline number confirmed by ear. What matters is that it is **not silent** and that the run recovers and prints its `ANCHOR` line — the block-count limit is what guarantees the recovery.

Warn whoever is listening before this segment, and keep monitors low.

The calibration itself is the deliverable of this task. Record, per anchor: the offline percent, the anchor avg percent, and the ratio.
- An anchor **above** its offline number is expected and healthy — that gap is the ISR overhead and the DMA contention the offline runner cannot see.
- An anchor **below** its offline number by more than a few percent means the callback is not running the same work; check that `g_current->process()` is really being called.
- **Silence during a segment is a failure**, even if the numbers look plausible. It means the workload was optimized away in the callback context and the corresponding offline row is not trustworthy. Report it — do not adjust the numbers.

- [ ] **Step 6: Commit**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
git add bench/
git commit -m "$(cat <<'EOF'
feat(bench): anchor mode -- three rows re-run inside a real audio callback

CpuLoadMeter over instrument_worst, oliverb_solo_sram and grain_read_sdram,
four seconds each, audibly on the Seed's outputs at low level. Calibrates the
offline DWT numbers against ISR overhead and SDRAM DMA contention, and proves
the workloads actually compute instead of having been optimized away.

Results print after the audio stops -- a semihosting write halts the core while
openocd services it, so printing alongside a live callback would break the audio
outright, not merely skew the number.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 7: The host driver writes committed result files

Everything so far prints to a terminal. This task turns a run into two files under `docs/bench/` — the deliverable the spec's acceptance criteria are written against — and lands the first real result.

**Files:**
- Modify: `bench/run.py` (parse + write + determinism check)
- Create: `bench/README.md`, `docs/bench/<today>-<shorthash>.md`, `docs/bench/<today>-<shorthash>.csv`
- Test: two consecutive full runs on hardware

**Interfaces:**
- Consumes: the `BENCH_BEGIN` / `BENCH` / `ANCHOR` / `BENCH_END` line contract from Tasks 1, 2 and 6.
- Produces: `docs/bench/YYYY-MM-DD-<shorthash>.md` and `.csv`; `run.py --repeat N`, `--out-dir`.

- [ ] **Step 1: Add parsing and file writing to run.py**

Add these imports at the top of `bench/run.py`, next to the existing ones:

```python
import csv
import datetime
import io
```

Add the following functions above `main()`:

```python
BUDGET_CYCLES = 960000


def parse(lines):
    """Pull the marker-delimited payload out of a capture. Returns
    (header, rows, anchors) or None if the run never completed."""
    header, rows, anchors = None, [], []
    for line in lines:
        if line.startswith("BENCH_BEGIN,"):
            f = line.split(",")
            header = {"githash": f[1], "clock": f[2], "block": f[3], "cache": f[4]}
        elif line.startswith("BENCH,"):
            f = line.split(",")
            rows.append({
                "family": f[1], "name": f[2], "avg_cyc": f[3], "max_cyc": f[4],
                "pct_avg": f[5], "pct_max": f[6], "checksum": f[7],
            })
        elif line.startswith("ANCHOR,"):
            f = line.split(",")
            anchors.append({"name": f[1], "avg_pct": f[2], "max_pct": f[3]})
    if header is None or not rows:
        return None
    return header, rows, anchors


def by_name(rows):
    return {r["name"]: r for r in rows}


def ratio(rows, num, den):
    """Cycle ratio between two rows, as a printable string."""
    d = by_name(rows)
    if num not in d or den not in d:
        return "n/a (row missing)"
    try:
        a, b = float(d[num]["avg_cyc"]), float(d[den]["avg_cyc"])
    except ValueError:
        return "n/a (TIMEOUT)"
    if b <= 0:
        return "n/a (zero denominator)"
    return "%.2fx" % (a / b)


def verdict(rows, anchors):
    """The paragraph the spec's acceptance criterion 2 asks for: the three
    questions this bench exists to answer, answered in prose."""
    d = by_name(rows)
    a = {x["name"]: x for x in anchors}
    out = io.StringIO()

    worst = d.get("instrument_worst")
    worst_anchor = a.get("instrument_worst")
    out.write("## Verdict\n\n")

    if worst and worst["avg_cyc"] != "TIMEOUT":
        offline = worst["pct_max"]
        anchored = worst_anchor["max_pct"] if worst_anchor else "not anchored"
        out.write(
            "**2x4 budget — go/no-go.** The full instrument at its worst case "
            "(8 voices, COLOR 4-note on both parts, all FX on, high diffusion, "
            "echo at max) costs **%s %% of the block budget offline**, and "
            "**%s %% measured inside a real audio callback**. The anchored "
            "figure is the one that decides: under 100 %% the 2x4 architecture "
            "fits, over it the design has to shed voices or FX.\n\n"
            % (offline, anchored))
    else:
        out.write("**2x4 budget — NO RESULT.** `instrument_worst` did not "
                  "produce a number. The go/no-go question is unanswered.\n\n")

    # Ratios are against synth_1_voice -- ONE REAL spotymod voice (two MorphOsc
    # in unison + sub + SVF + envelope). NOT against morph_osc_bare, which is a
    # single oscillator kernel and ~7.3x cheaper; anchoring on that inflates
    # every ratio by that factor and misranks the table.
    out.write("**Cost per candidate, relative to one real spotymod voice.**\n\n")
    for r in rows:
        if r["family"] == "voice" and r["name"] != "morph_osc_bare":
            out.write("- `%s` — %s one real voice (%s a bare oscillator kernel)\n"
                      % (r["name"],
                         ratio(rows, r["name"], "synth_1_voice"),
                         ratio(rows, r["name"], "morph_osc_bare")))
    out.write("\n")

    out.write(
        "**SRAM vs SDRAM.** The grain-read proxy (8 scattered interpolated "
        "stereo reads per sample, identical window in both regions) costs "
        "**%s** in SDRAM against SRAM — this is the M5 texture deck's exposure, "
        "measured before the sampler exists. The Oliverb pair reads **%s**, "
        "and the shortened echo-style streaming walk **%s**.\n\n"
        % (ratio(rows, "grain_read_sdram", "grain_read_sram"),
           ratio(rows, "oliverb_sdram", "oliverb_solo_sram"),
           ratio(rows, "echo_walk_sdram", "echo_walk_sram")))
    return out.getvalue()


def write_results(out_dir, header, rows, anchors, drift):
    os.makedirs(out_dir, exist_ok=True)
    stamp = datetime.date.today().isoformat()
    base = os.path.join(out_dir, "%s-%s" % (stamp, header["githash"]))

    with open(base + ".csv", "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["family", "name", "avg_cyc", "max_cyc",
                    "pct_avg", "pct_max", "checksum"])
        for r in rows:
            w.writerow([r["family"], r["name"], r["avg_cyc"], r["max_cyc"],
                        r["pct_avg"], r["pct_max"], r["checksum"]])

    with open(base + ".md", "w", encoding="utf-8") as fh:
        fh.write("# Bench run %s — `%s`\n\n" % (stamp, header["githash"]))
        fh.write("Measured on a Daisy Seed (STM32H750). %s Hz core clock, "
                 "block size %s, %s, `-ffast-math -funroll-loops`. "
                 "Block budget %d cycles.\n\n"
                 % (header["clock"], header["block"], header["cache"],
                    BUDGET_CYCLES))
        if drift:
            fh.write("> **WARNING — checksum drift between runs.** %s\n>\n"
                     "> Determinism is a measured property of this engine, not "
                     "an assumption. These numbers are suspect until the drift "
                     "is explained.\n\n" % drift)
        fh.write(verdict(rows, anchors))
        fh.write("## Offline table\n\n")
        fh.write("| family | workload | avg cyc | max cyc | avg %% | max %% | checksum |\n")
        fh.write("|---|---|---:|---:|---:|---:|---|\n")
        for r in rows:
            fh.write("| %s | `%s` | %s | %s | %s | %s | `%s` |\n"
                     % (r["family"], r["name"], r["avg_cyc"], r["max_cyc"],
                        r["pct_avg"], r["pct_max"], r["checksum"]))
        if anchors:
            fh.write("\n## Anchor mode (real audio callback, CpuLoadMeter)\n\n")
            fh.write("| workload | avg %% | max %% |\n|---|---:|---:|\n")
            for x in anchors:
                fh.write("| `%s` | %s | %s |\n"
                         % (x["name"], x["avg_pct"], x["max_pct"]))
    return base
```

- [ ] **Step 2: Wire the repeat-and-compare flow into main()**

In `bench/run.py`, add the two new arguments next to the existing ones:

```python
    ap.add_argument("--repeat", type=int, default=2,
                    help="runs to compare for the determinism check")
    ap.add_argument("--out-dir", default=os.path.join(REPO, "docs", "bench"))
```

and replace everything in `main()` from `load(args.interface)` to the end of the function with:

```python
    captures = []
    for i in range(max(1, args.repeat)):
        print("# run %d/%d" % (i + 1, args.repeat), file=sys.stderr)
        lines = run_once(args.interface, args.timeout)
        if lines is None:
            print("ERROR: BENCH_END never arrived (timeout or openocd exited)",
                  file=sys.stderr)
            return 2
        parsed = parse(lines)
        if parsed is None:
            print("ERROR: capture completed but held no usable rows",
                  file=sys.stderr)
            return 2
        captures.append(parsed)

    header, rows, anchors = captures[0]

    # A repeat run must produce identical checksums. If it does not, the
    # engine is not deterministic under these conditions and the numbers say
    # less than they appear to -- so it goes in the file, loudly.
    drift = ""
    for j, (_, other, _) in enumerate(captures[1:], start=2):
        a, b = by_name(rows), by_name(other)
        bad = [n for n in a
               if n in b and a[n]["checksum"] != b[n]["checksum"]]
        if bad:
            drift += ("Run 1 vs run %d differ on: %s. " % (j, ", ".join(bad)))
    if drift:
        print("WARNING: %s" % drift, file=sys.stderr)

    base = write_results(args.out_dir, header, rows, anchors, drift)
    print("# wrote %s.md and %s.csv" % (base, base), file=sys.stderr)
    return 0
```

- [ ] **Step 3: Write the README**

Create `bench/README.md`. It must contain, in this order:
1. **What this is** — a never-shipped measurement app; the shipping firmware is the root `Makefile` and is untouched.
2. **The one command** — `python run.py`, with a note that it builds, loads via the probe, captures twice and writes `docs/bench/`.
3. **Hardware setup** — probe on SWD and nothing else; the Seed's micro-USB is not used. Monitors connected at low volume for anchor mode.
4. **The probe** — the exact `--interface` value that worked in Task 1 Step 7, named explicitly, with the other two candidates listed as fallbacks.
5. **Reading the table** — what `avg cyc` / `max cyc` / `pct` / `checksum` mean, and that `TIMEOUT` means a row exceeded 10× the block budget and was aborted rather than allowed to hang the run.
6. **Fallbacks** — (a) if the SRAM load ever stops working, `make program-dfu` from `bench/` with the bootloader button dance; note that semihosting still needs openocd attached afterwards, so `arm semihosting enable` must be issued against the already-running board. (b) If semihosting itself proves inadequate, the escape hatch is USB-CDC: swap `report.cpp`'s `sh_write0` for `daisy::Logger<daisy::LOGGER_INTERNAL>`, call `StartLog(true)` in `main`, connect the Seed's micro-USB as a second cable, and read the COM port with pyserial. Say plainly that this is untested.
7. **The one hard rule** — the bench binary requires an attached openocd. Run it without one and the first `bkpt 0xAB` halts the core forever, looking exactly like a hang.
8. **What anchor mode's audio actually proves, and what it does not.** The monitor output writes *one* value per block — `process()`'s return, the checksum accumulator — held across all 96 samples. That is a 500 Hz staircase of accumulator sums, not the workload's audio, so **every workload sounds like the same harsh buzz** and the three segments cannot be told apart by ear. It is a **non-silence detector**, nothing more: it distinguishes "this computed something" from "this was optimised away". The real anti-dead-code guarantee is the checksum, which is non-zero, reproducible across runs, and data-dependent by construction. Do not read the monitor as a listening test, and do not promise anyone they will hear a reverb.
9. **Anchor mode's third segment sounds broken on purpose.** `instrument_worst` runs at ~160 % of block budget, so the callback cannot complete before the next block is due and the DAC is fed underrun garbage. That is the offline number confirmed by ear. The block-count limit inside the callback is what makes it stop; an earlier foreground-delay version ran for minutes because the over-budget workload starved the thread meant to stop it.
8. **Adding a workload** — one row in the relevant `kXxxWorkloads[]` table, plus the note that basenames must stay unique because libDaisy's Makefile flattens paths with `notdir`.

- [ ] **Step 4: Do the real run**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach/bench
python run.py
```

Expected: two full sweeps, then `# wrote .../docs/bench/2026-07-…-<hash>.md and .csv`, exit code 0, and no drift warning.

```bash
cat ../docs/bench/*.md
```

Verify against the spec's acceptance criteria before committing:
- One command produced one `.md` + one `.csv` covering all three families.
- Two consecutive runs were checksum-identical (no warning block in the file).
- The verdict paragraph names an anchored worst-case percent, lists all nine candidates as multiples of **one real spotymod voice** (`synth_1_voice`), and gives the grain-proxy SRAM/SDRAM factor.

If the drift warning did fire, the file still gets written and committed — that is the design. Report the drifting row names; a non-deterministic row is a finding about the engine, not a reason to hide the run.

- [ ] **Step 5: Commit the tooling and the first result together**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
git add bench/ docs/bench/
git commit -m "$(cat <<'EOF'
feat(bench): run.py writes committed result files, and the first real run

One command builds, loads through the probe, captures twice, compares the
checksums and writes docs/bench/<date>-<hash>.{md,csv}: a verdict paragraph
answering the three questions this bench exists for, the full offline table and
the anchor-mode calibration. Checksum drift between the two runs lands in the
file as a warning rather than being swallowed -- determinism is measured here,
not assumed.

Committed with the first result from real hardware.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 8: The numbers flow back

A result file nobody reads changes nothing. The spec's third acceptance criterion is that these numbers reach the documents that were written on guesses.

**Files:**
- Modify: `docs/roadmap.md`
- Modify: `docs/superpowers/specs/2026-07-18-sampler-texture-deck-design.md`
- Modify (other repository): `C:\Users\bernd\Documents\AI\Synthux Design Residency\docs\superpowers\specs\2026-07-18-spotykach-engine-expansion-research.md`
- Test: the root-Makefile baseline from Task 1 Step 2

**Interfaces:**
- Consumes: `docs/bench/<date>-<hash>.md` from Task 7. Produces nothing for later tasks.

- [ ] **Step 1: Prove the shipping firmware was never disturbed**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
make -j8 2>&1 | tail -5
git diff --stat HEAD~7 -- main.cpp app.cpp app.h meter.h common.h alt_sram.lds Makefile CMakeLists.txt src/ third_party/ engine/
```

Expected: the size line matches what you recorded in Task 1 Step 2, and the `git diff --stat` output is **empty**. A non-empty diff means the bench modified the thing it was measuring — stop and report; do not paper over it in the roadmap.

- [ ] **Step 2: Record it in the roadmap**

In `docs/roadmap.md`, read 40 lines of surrounding context first and match the existing prose voice and heading depth — do not invent a new format.

Add a row to the "Status at a glance" table, after the `+ COLOR-MOTION` row, in the same style:

```markdown
| **Bench** | Bench firmware — DWT cycle measurement of the engine, nine DaisySP candidates and SRAM-vs-SDRAM buffer access on real hardware | ✅ **done** (`bench/`, results in `docs/bench/`) |
```

Then add a subsection under "## Done", after the COLOR-MOTION entry, written in the surrounding voice. It must state:
- what `bench/` is (a never-shipped standalone app; the shipping firmware untouched);
- that the numbers come from a real Daisy Seed at 480 MHz, 48 kHz, block 96, not from estimates;
- **the anchored worst-case percent of the block budget for the full instrument, and whether the 2×4 architecture fits** — the actual number from your result file;
- the SRAM/SDRAM factor of the grain-read proxy, and what it means for M5;
- a pointer to `docs/bench/` and to `bench/README.md`.

The file's `**Last updated:**` line near the top currently reads `2026-07-16`. Change it to today's date.

The file also carries a blockquote reading "nothing here has run on real hardware yet". That is no longer true and it is the single most misleading sentence in the document now. Rewrite it to say that the *engine and its milestones* are still verified only against the desktop renderer, but that the CPU budget has been measured on real hardware — see `docs/bench/`.

- [ ] **Step 3: Give the texture-deck spec its number**

Open `docs/superpowers/specs/2026-07-18-sampler-texture-deck-design.md` and find the passage that flags SDRAM access cost as an unquantified risk (search for `SDRAM`). Replace the caveat with the measured factor, citing the result file by name, in the spec's own voice. Keep it to two or three sentences: the grain-read proxy's factor, what window size it was measured over, and what that implies for the deck's grain count.

If the spec turns out to carry no such caveat, do not invent a section — add one sentence to its risk or assumptions section instead, and say in your report that the caveat was not where the bench spec expected it.

- [ ] **Step 4: Close the research document's open questions**

This file lives in the **residency repository**, not this one:

```
C:\Users\bernd\Documents\AI\Synthux Design Residency\docs\superpowers\specs\2026-07-18-spotykach-engine-expansion-research.md
```

It is written in German — match that. Its `## Offene Fragen` section has five numbered questions; **1** and **2** are now partly answered:

- **Question 1** ("Was kostet eine Plaits-Stimme tatsächlich auf dem H750 @480 MHz") — answer what was actually measured: the nine *DaisySP* candidates as multiples of our MorphOsc voice, with the numbers. Be precise that these are the DaisySP kernels, **not** the full Plaits engines — the Plaits-monorepo port is explicitly out of scope for the bench, so the question narrows rather than closes.
- **Question 2** (which engines need SDRAM, and the cost) — answer the *cost* half with the grain-proxy factor and the Oliverb pair. The *which-engines* and flash-footprint halves remain open; leave them open and say so.

Also revise the section `## Vorgeschlagene Reihenfolge`, whose opening line is **"Zuerst messen, nicht auswählen."** — the measuring has happened. Rewrite that paragraph to point at `spotymod`'s `docs/bench/<date>-<hash>.md` and to state what the measurement decided, keeping the numbered priority list below it intact.

Do not touch the `## Widerlegt` / `## Bestätigt` evidence sections — those are about published third-party claims, and our own measurements do not belong in them.

- [ ] **Step 5: Commit both repositories**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
git add docs/roadmap.md docs/superpowers/specs/2026-07-18-sampler-texture-deck-design.md
git commit -m "$(cat <<'EOF'
docs: the bench numbers reach the roadmap and the texture-deck spec

The 2x4 budget is no longer an estimate, and the M5 sampler's SDRAM exposure is
no longer a caveat -- both now cite docs/bench/. Drops the roadmap's "nothing
here has run on real hardware yet" blockquote, which stopped being true.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

```bash
cd "/c/Users/bernd/Documents/AI/Synthux Design Residency"
git add docs/superpowers/specs/2026-07-18-spotykach-engine-expansion-research.md
git commit -m "$(cat <<'EOF'
docs(research): offene Fragen 1+2 mit gemessenen Zahlen beantwortet

Die neun DaisySP-Kandidaten als Vielfache unserer MorphOsc-Stimme, dazu der
SRAM/SDRAM-Faktor des Grain-Read-Proxys -- gemessen auf echter Hardware, nicht
geschaetzt. Der volle Plaits-Baum bleibt offen, ebenso die Flash-Haelfte von
Frage 2.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

## Out of scope

- **No Plaits monorepo, no stmlib, no msfa/Dexed port** just to measure. Family 2 is DaisySP-only, because DaisySP is already a submodule and costs zero porting. If a port lands later it becomes a table row then.
- **No result-diff tooling.** Committed files diff in git.
- **No on-device UI** — no LED codes, no button-driven mode select. The probe is the entire interface.
- **No CI integration.** The bench needs physical hardware on a desk; `.github/workflows/build-plugin.yml` is not touched.
- **No USB-CDC transport implementation.** `--transport` exists with one accepted value, `semihost`. The CDC path is described in `bench/README.md` as an escape hatch and is deliberately left unbuilt and untested.
- **No change to `engine/`, `src/`, `third_party/`, or the root build.** If a measurement seems to require one, that is a finding to report, not a task to do.
- **No optimisation work.** This plan produces numbers. What to do about them is the next spec.

# bench

## What this is

A measurement app that has never shipped and never will. It boots the Daisy
Seed straight into a cycle-counting harness instead of the spotymod firmware,
runs a fixed table of workloads, and reports cycle counts and checksums back
over the debug probe. The shipping firmware is the repo-root `Makefile`,
`main.cpp`, `app.cpp`/`app.h`, `src/**`, `third_party/**` and `engine/**` —
none of that is touched by anything under `bench/`. This directory has its
own `Makefile`, its own `main.cpp`, and links against `alt_sram.lds` only to
reuse the same BOOT_SRAM placement the real firmware uses, so the timing
context (SRAM vs SDRAM latency) matches production.

## The one command

```
python run.py
```

From `bench/`. This builds the bench firmware, loads it into the Seed's SRAM
through the debug probe (no bootloader, no flash wear), captures its
semihosting output **twice** (`--repeat 2` is the default — see "Anchor
mode's audio" below for what that means out loud), compares the two runs'
checksums, and writes `../docs/bench/YYYY-MM-DD-<githash>.md` and `.csv`.
Exit code 0 means both runs completed and a file was written; a checksum
mismatch between the two runs does **not** stop the file from being
written — it lands in the `.md` as a warning block instead, because
determinism is a measured property of this engine, not an assumption.

Useful flags: `--repeat N` (default 2), `--out-dir DIR` (default
`../docs/bench`), `--build-only`, `--no-build`, `--timeout SECONDS` (default
600, per run), `--interface CFG` (see below).

## Hardware setup

Only the debug probe's SWD cable is connected — 4 wires (SWDIO, SWCLK, GND,
3V3 or just GND+SWD if the probe supplies its own power) from the ST-Link to
the Seed's SWD header. The Seed's own micro-USB port is **not** used for
anything in the normal flow; the bench never talks over it. If anchor mode
is going to run (it always does, as part of the family-1 sweep), have
monitors connected to the Seed's audio out at **low volume** first — see
below for what you'll actually hear.

## The probe

This desk's probe is an ST-Link V3, and openocd needs `stlink-dap.cfg` to
get a real DAP out of it — that's `run.py`'s default and the value
confirmed working in Task 1. Plain `stlink.cfg` auto-selects the older
`hla_swd` transport, under which `spotykach-sram.cfg`'s
`transport select dapdirect_swd` line is an error, not a fallback path.

If a different probe is on the desk, the other candidates to try via
`--interface` are `cmsis-dap.cfg` (CMSIS-DAP probes) and `stlink.cfg`
(older ST-Link V2/V2-1 hardware, accepting the `hla_swd` limitation). Both
are untested against this bench.

## Reading the table

- **avg cyc / max cyc** — DWT cycle counter reading for the workload's
  `process()`, averaged and maxed over the repeated calls in one bench pass.
- **avg % / max %** — the same, as a percentage of `BUDGET_CYCLES` (960 000
  cycles: 480 MHz core clock, 96-sample block, 48 kHz). Over 100% means the
  workload alone would blow the audio block budget.
- **checksum** — an 8-hex-digit accumulator folded from every sample the
  workload produced. It exists so the compiler cannot optimise the workload
  away as dead code, and it is what the `--repeat 2` determinism check
  compares between runs. A workload whose output legitimately depends on
  uninitialized memory or wall-clock timing will show up here as drift.
- **TIMEOUT** — the row's `avg_cyc` field reads the literal string `TIMEOUT`
  (and `pct_avg`/`pct_max` are empty) when the workload ran past 10× the
  block budget; the runner aborts it rather than letting one bad workload
  hang the whole sweep. A `TIMEOUT` row still carries a `max_cyc` and a
  `checksum` from the point it was cut off.

## Fallbacks

**(a) If the SRAM load ever stops working** (i.e. `spotykach-sram.cfg`'s
`load_image` / `reset halt` path stops taking): fall back to
`make program-dfu` from `bench/`, with the usual BOOT-button-then-RESET
dance to get the Seed into its DFU bootloader. Semihosting still needs
openocd attached afterwards for `run.py` to read anything — since the board
is already running the bench image at that point, bring openocd up against
it and issue `arm semihosting enable` directly (there is no `load_image` or
`reset halt` step in this path, the board is already executing).

**(b) If semihosting itself proves inadequate** (too slow, or the probe
setup won't cooperate), the escape hatch is USB-CDC: swap `report.cpp`'s
`sh_write0` calls for `daisy::Logger<daisy::LOGGER_INTERNAL>`, call
`StartLog(true)` early in `main()`, connect the Seed's micro-USB as a
*second* cable alongside the SWD probe, and read the resulting COM port
from the host with `pyserial` instead of parsing openocd's stdout. This
path is **untested** — it is written down as the next thing to try, not as
something that has been made to work.

## The one hard rule

The bench binary requires an attached, running openocd session. Its
`report.cpp` talks to the host by executing `bkpt 0xAB` (the semihosting
breakpoint) and blocking for a response; without a debugger serving that
breakpoint, the very first one halts the core forever. From the outside
that looks exactly like a hang — no crash, no error, just a Seed that never
gets anywhere near `BENCH_END`. If a run seems stuck, check that openocd is
actually attached before assuming the firmware is broken.

## What anchor mode's audio actually proves, and what it does not

Anchor mode re-runs three of the family-1 workloads inside a real audio
callback and drives `CpuLoadMeter` for the anchored percentages. The DAC
output during that segment is **not** the workload's audio — it is one
value per block (`process()`'s return, the same checksum accumulator used
everywhere else in this bench) held flat across all 96 samples of that
block. That produces a roughly 500 Hz staircase built out of accumulator
sums, not a rendering of the reverb tail or the synth voice underneath it.

Consequently **every workload sounds like the same harsh buzz**, and the
three anchored segments cannot be told apart by ear. This is a
**non-silence detector**, nothing more: it distinguishes "this callback
computed something" from "the optimiser deleted this workload as dead
code." The checksum is the actual anti-dead-code guarantee — it is
non-zero, it is reproducible across the two repeat runs, and it is
data-dependent by construction. Do not read the monitor output as a
listening test, and do not promise anyone they will hear a reverb, a
synth voice, or anything resembling spotymod's actual sound in this mode.

**Anchor mode's third segment sounds broken on purpose.** `instrument_worst`
runs at roughly 160% of the block budget offline, so inside the real
callback it cannot finish before the next block is due; the DAC ends up
fed underrun garbage for that segment. That is not a bug in the bench — it
is the offline number, confirmed by ear. The block-count limit built into
the anchor callback (it stops itself after a fixed number of blocks) is
what ends that segment; an earlier version of this harness used a
foreground delay loop to pace itself instead, and because the over-budget
workload starves the very thread that delay loop needed to run on, that
version did not stop on its own — it ran for minutes until killed by hand.

Since `--repeat 2` is the default, anchor mode runs **twice** per `run.py`
invocation — expect two bursts of this harsh buzz, not one, with the
broken-on-purpose segment inside each.

One more thing the bench does that is easy to miss: it stamps
`boot_info.version` into the STM32H750's battery-backed backup SRAM and
does not restore the previous value afterwards. That marks the board as
having a "v6.1 bootloader present" for any firmware load that checks that
flag later. This has been judged harmless — it doesn't change what the
bootloader does — but it was undocumented until now, so future debugging
of boot-related oddities on this specific board should know the bench is a
possible source.

## `instrument_worst` measures the reverb in SRAM

`Instrument::init` wires its reverb pointer through `fx_mem()`, and
`bench/mem.cpp`'s `fx_mem()` hands back `&g_rev_sram` unconditionally --
there is no SDRAM variant reachable from the instrument rows. So
`instrument_worst` (and `instrument_init`) pay the SRAM reverb's cost, not
the SDRAM one. The bench's own SRAM-vs-SDRAM measurement for the same
Oliverb (`oliverb_solo_sram` vs `oliverb_sdram`, family 3) puts that
difference at roughly 1.1x -- small next to the grain-read proxy's 5.3x,
but not zero. If a future production build moves the reverb buffer to
SDRAM, expect `instrument_worst`'s headline percentage to rise by roughly
two points, not to hold at the figure recorded here.

## Row order is state, not just presentation

The `abl` instrument rows share one `Instrument` (`g_abl_inst`) and each
`setup_worst()` re-inits it, which clears the FLUX tape (`DeLine::Init`
memsets), the reverb buffer (`FxEngine::Init` calls `Clear`) and the reverb
loop filters (reset in `Oliverb::Init`). Even so, **inserting
`inst_worst_nogrit` before `inst_worst_choked` changed the choked row's
checksum** (`995dbd34` → `cd90d415`) while its cost moved 0.001 %
(841 869 → 841 859 cycles). Some state still carries between these rows and
the three obvious candidates above are not it; the cause is unpinned.

Consequences: a checksum shift on a row you did not touch, after inserting a
row *before* it, is expected rather than alarming — but it means **abl
checksums are only comparable between runs whose table order matches**, and
a genuine determinism regression could hide behind the same signature. The
same trap is documented in `workloads_system.cpp` for `g_inst_ctr`. Append
new `abl` rows at the end of the table when you can.

## The sampler family, and the two things it changed

`workloads_sampler.cpp` prices the M5 texture deck. Two structural
consequences worth knowing before touching anything near it:

**1. The bench's 64 KB arena left the SRAM region.** `Part` embeds a
`SamplerEngine` (1 392 B), and the bench's globals hold four bare `Part`s and
two `Instrument`s, so the sampler merge pushed `.bss` 6 168 bytes past the
256 KB `SRAM` region and the bench stopped linking. `alt_sram.lds` gained a
`.sram_exec_bss` section and `bench/mem.h` a `BENCH_SRAM_EXEC_BSS` macro;
`g_sram` now lives there.

This is **not** a change of measurement conditions. `SRAM_EXEC` and `SRAM` are
the same physical AXI SRAM — one 512 KB block at `0x24000000`, split into a
code half and a data half by the `MEMORY` block, same bus, same latency, same
MPU cache attributes. `grain_read_sram` measures exactly what it measured
before. The section is additive and empty in the shipping firmware (nothing
under `src/**` emits into it), so no shipping symbol moved either.

What it *is*: a warning. `SRAM_EXEC` now sits at **93 %** and `SRAM` at 82 %.
The next thing that overflows will be code, not data, and there is no third
half to move it to.

**2. The sampler rows own the SDRAM arena.** They run **last** in `main.cpp`
because `SamplerEngine::load_sample` takes two `float*`, so the 8 MB
`sdram_arena()` doubles as the source material both channels are copied from
(L from the front, R from an offset view — the offset is what makes both
windows land inside it exactly). Family 3 and the taps rows are done with the
arena by then, and every sampler `setup()` refills what it is about to read.
Anything appended after the sampler family must not assume the arena still
holds what family 3 put there.

Also note `fx_mem()` now fills `sampler_buf[]` and `sampler_frames` for every
`Instrument` the bench builds, not just the sampler rows. This costs the synth
rows nothing in the measured window — `SampleBuffer::init` → `clear()` takes
the `_size == 0` fast path on a buffer nothing has written to, and an
unselected engine's `process()` is never called — so their numbers are
unchanged. Only `setup()` touches it.

**What the rows found.** `inst_sampler_worst` is the same box as
`instrument_worst` with both parts swapped to the sampler, and it is *cheaper
on the mean block and more expensive on the worst* — the peak clears 100 % of
the budget while the mean stays under it. That shape is the grain scheduler:
the steady cloud is affordable, a block that happens to catch several spawns
is not. Anyone reading the sampler rows for a "does it fit" answer has to read
the max column, not the avg column.

One thing deliberately **not** a row: `SamplerEngine::init()` on a buffer
holding content ends in `clear()`, which memsets 16 MB of SDRAM. That is real
and it is expensive — far past 10x the block budget, so as a table row it
would only ever print `TIMEOUT`. It belongs nowhere near an audio thread, and
this paragraph is the record of it.

## Adding a workload

Add one row to the relevant `kXxxWorkloads[]` table (`workloads_system.cpp`
for family 1, `workloads_daisysp.cpp` for family 2, `workloads_memory.cpp`
for family 3, `workloads_mod.cpp` for the modulation plane, `workloads_abl.cpp`
for the ablation rows, `workloads_taps.cpp` for the FLUX tap bank,
`workloads_sampler.cpp` for the texture deck) with a
family tag, a name, a setup function and a process function. A new *table*
additionally needs its `extern` in `workload.h`, an entry in `runner.cpp`'s
`find_workload` arrays (and its loop bound), a loop in `main.cpp`, and the
source in the `Makefile` — five places, none of them automatic, because table
order is execution order and must not depend on link order. Workload **basenames must stay unique across the whole bench**,
not just within one file's table — libDaisy's Makefile flattens every
source path with `notdir` when it builds the object list, so two files
named e.g. `voice.cpp` in different directories would collide at link time
even though their paths differ.

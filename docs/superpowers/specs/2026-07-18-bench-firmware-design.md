# Bench firmware — measuring instead of guessing

**Date:** 2026-07-18
**Status:** approved (brainstorm with Bastian, 2026-07-18)
**Context:** the engine-expansion research (residency repo,
`2026-07-18-spotykach-engine-expansion-research.md`) adversarially killed
every published CPU number for this platform — 10 of 25 claims refuted,
including all with hard figures. The 2×4 architecture, the reverb budget,
the engine-candidate shortlist and the M5 sampler's SDRAM exposure all
rest on unmeasured assumptions. This firmware replaces them with numbers
from the actual Daisy Seed (STM32H750, 480 MHz), measured on the actual
engine code.

## Decisions (from brainstorming, 2026-07-18)

- **Scope: three families** — own system component-wise, DaisySP
  expansion candidates per voice, SRAM-vs-SDRAM A/B. One flash answers
  the budget question AND the engine-expansion question.
- **Method: hybrid** — offline DWT-cycle enumeration for the full table,
  then an in-callback anchor mode (CpuLoadMeter) for a handful of
  reference workloads to calibrate the offline optimism and prove by ear
  that nothing was optimized away.
- **Automation: debug probe.** Bastian owns a Daisy-compatible debug
  probe (SWD). One command builds, flashes, captures and writes committed
  result files — no bootloader button dance, no manual copy-paste.
- **Architecture: standalone `bench/` app.** Own main + Makefile in the
  libDaisy pattern, links `engine/` + `third_party/` + full DaisySP. The
  bench binary is never shipped — LGPL linkage is fine here, full DaisySP
  classes may be measured. Original firmware (`src/`, root Makefile)
  stays untouched.

## Measurement conditions

Fixed for every workload, printed in the result header: 48 kHz, block
size 96 (the planned firmware config; block budget = 2 ms = 960 000
cycles at 480 MHz), D-cache and I-cache enabled, `-ffast-math
-funroll-loops` (the root Makefile's flags), compiled-in git short hash.
All workloads run with seeded, deterministic inputs (the engine is
deterministic by construction; input-consuming workloads get seeded
noise/test tones). Every workload accumulates a checksum of its output
and prints it — this defeats dead-code elimination AND doubles as a
cross-run determinism regression check.

## Workload matrix

### Family 1 — own system, decomposed

| Workload | Answers |
|----------|---------|
| empty callback (baseline) | fixed cost |
| SuperModulator ×2 + Center, mod plane only | lanes budget (spec estimate 4–6 %) |
| SynthEngine 1 / 2 / 4 voices, one part | does polyphony scale linearly? |
| SynthEngine 2×4 voices (both parts) | the 8-voice truth (estimate 15–18 %) |
| PartFx components: GRIT alone, FLUX echo alone (SDRAM buffer), COMP alone | FX budget (estimate 8–10 %) |
| Oliverb solo | the reverb question (estimate 15–25 %); redeems the firmware-shell spec's planned first METER measurement |
| full `Instrument::process`, worst case: 8 voices, COLOR 4-note both parts, DENSE 1, all FX on, high diffusion, echo max | **the one number: does the 2×4 design fit?** |
| full `Instrument::process`, init patch | the typical load |

### Family 2 — expansion candidates (DaisySP, per voice)

`ModalVoice`, `StringVoice`, `Resonator`, `FormantOscillator`,
`VosimOscillator`, `HarmonicOscillator`, `GrainletOscillator`,
`ZOscillator`, `VariableShapeOscillator` — each as a single sounding
voice; **plus one spotymod MorphOsc synth voice as the reference
anchor**, so every candidate reads as a multiple of "our voice".
Answers the research's open question 1 with measurements instead of
analogies.

### Family 3 — SRAM vs SDRAM A/B (the NIME question, made concrete)

| Workload | Answers |
|----------|---------|
| **grain-read proxy:** 8 scattered linear-interpolated stereo reads per sample from a large SDRAM buffer vs. the identical access pattern on a small SRAM buffer | **the M5 sampler risk, quantified before the sampler exists** |
| Oliverb with its 128 KB buffer in SRAM vs. SDRAM | the M6 reverb placement decision |
| echo-style delay access SRAM vs. SDRAM (shortened window — 937 KB does not fit SRAM; measured is the access-pattern factor, not full length) | FLUX placement / SDRAM penalty on streaming access |

SDRAM buffers use `DSY_SDRAM_BSS` (runtime-zeroed — no compile-time
init, as the research flagged); SRAM variants use plain static buffers
inside the linker's RAM regions.

## Bench core (`bench/`)

- **Workload registry:** `{name, setup(), process(block)}` — a new
  workload is one table row. Registry order is execution order.
- **Offline runner:** per workload: 100 warm-up blocks, then N ≈ 1000
  measured blocks via the **DWT cycle counter**; records avg/max cycles →
  percent of block budget. A workload exceeding 10× budget is aborted and
  listed as `TIMEOUT` instead of hanging the run.
- **Anchor mode:** after the offline table, the firmware registers a real
  audio callback and cycles a few seconds each through: full instrument
  (worst case), Oliverb solo, the SDRAM grain proxy — measured with
  `daisy::CpuLoadMeter` (the existing `meter.h` pattern), audibly on the
  Seed's outputs (attenuated). Calibrates offline→ISR offset (interrupt
  overhead, DMA concurrency on SDRAM) and proves the workloads compute.
- **Output:** machine-parsable lines between markers:
  `BENCH_BEGIN,<githash>,<clock>,<block>,<cache>` … one
  `BENCH,<family>,<name>,<avg_cyc>,<max_cyc>,<pct_avg>,<pct_max>,<checksum>`
  per workload … anchor lines `ANCHOR,<name>,<avg_pct>,<max_pct>` …
  `BENCH_END`. A human-readable table follows for terminal watchers.

## Host driver (`bench/run.py`)

One command: build → flash via the debug probe (openocd; preferred:
load the app directly into RAM and run — no bootloader dance, no flash
wear) → capture output (RTT via the probe preferred, USB-CDC serial as
fallback, `--transport rtt|serial`) → parse markers → write **two
committable files**:

- `docs/bench/YYYY-MM-DD-<shorthash>.md` — the human table plus a
  verdict paragraph (see acceptance),
- `docs/bench/YYYY-MM-DD-<shorthash>.csv` — raw rows.

Non-zero exit if `BENCH_END` never arrives (host-side timeout — the
probe connection is the watchdog). A repeat run must produce identical
checksums; the script warns loudly if not.

## Acceptance

1. **One command, one committed result file** covering all three
   families; two consecutive runs checksum-identical.
2. The result file explicitly answers, in a verdict paragraph:
   - worst-case percent of the full instrument → **go/no-go for the 2×4
     budget** (with the anchor-calibrated number, not just offline),
   - cost per candidate voice relative to the MorphOsc anchor,
   - the SRAM/SDRAM factor of the grain-read proxy.
3. The numbers flow back: engine-expansion research (open questions 1+2),
   the M5 texture-deck spec (its SDRAM caveat gets its number), the
   milestone notes.

## Error behavior

Hung workload → per-workload 10×-budget abort, listed as `TIMEOUT`, run
continues. Dead probe / no output → host timeout, non-zero exit, nothing
written. Checksum drift between runs → loud warning in the result file
(determinism is a measured property, not an assumption). If RTT proves
flaky on this probe, the serial fallback is the supported path — the
firmware always logs to both.

## Out of scope (YAGNI)

- No Plaits-monorepo or msfa/Dexed porting just to measure — family 2 is
  DaisySP-only (zero porting, already vendored as a lib target). If a
  later port lands, it becomes a registry row then.
- No result-diff tooling in `run.py` — committed files diff in git.
- No on-device UI (LED codes etc.); the probe/serial is the interface.
- No CI integration — the bench needs physical hardware on a desk.

## Assumptions to verify during implementation

- The bench app (engine + full DaisySP + Oliverb) fits the RAM-load
  workflow; if the binary outgrows the load regions, fall back to the
  BOOT_SRAM/bootloader flow for the bench too (`APP_TYPE = BOOT_SRAM`,
  as the root Makefile).
- `lib/libDaisy` / `lib/DaisySP` submodules are present but not
  initialized in the working copy — first build step is
  `git submodule update --init` + `make libs`.
- openocd ships with the installed DaisyToolchain and talks to Bastian's
  probe (interface config to be identified at first connect); RTT
  support depends on the openocd build — serial fallback covers a miss.
- DWT cycle counter access (`CoreDebug->DEMCR`, `DWT->CYCCNT`) is free
  of side effects alongside libDaisy's own timing; `CpuLoadMeter`
  agreement in anchor mode is the cross-check.
- The Oliverb-in-SRAM A/B assumes 128 KB of spare SRAM next to the bench
  code — check the map file; if it doesn't fit, measure a reduced-size
  pair instead (same-size buffers in both locations, factor still valid).

# Bench ablation family Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the bench's dark matter. The component rows sum to ~120 % of block budget while `instrument_worst` measures ~159 % — roughly 375k cycles (39 points) that no row attributes. Fourteen short rows split that gap into named owners (Part glue, composition/cache coupling, the driven limiter), price every per-sample libm suspect directly (`sinf`, `tanhf`, `powf` vs `fast_sin`), isolate FLUX's SDRAM share from its compute share, and measure two states the current worst case silently omits (engaged CHOKE, GRIT's Reduce mode). Nothing is optimised here; the deliverable is a budget that closes, so the next cut is chosen on numbers instead of hypotheses.

**Architecture:** A fifth workload family, `abl`, in one new file `bench/workloads_abl.cpp`, following the same conventions as `workloads_system.cpp`: static table, checksum guards folded into every return value, setup re-initialises everything it touches. Four micro rows price one primitive each. One row runs a full `Part` that never sounds a voice, so its cost minus already-measured components is the per-sample glue. Three rows re-run the worst-case instrument with exactly one thing removed or added (FLUX off, reverb asleep, CHOKE engaged) — the drops are the in-context costs, and their excess over the solo rows is the composition coupling. Two rows price the master limiter clean vs driven. Two rows run the same `EchoDelay` core over SRAM vs SDRAM at echo-realistic scatter. Two rows price GRIT's two modes standalone.

**Tech Stack:** C++17, arm-none-eabi-gcc + libDaisy `core/Makefile` for the bench firmware; openocd 0.12 with an ST-Link V3; Python 3 standard library for `bench/run.py`.

**Design rationale (spec-lite):** brainstorm session 2026-07-18 against `docs/bench/2026-07-18-7e99b74.md`. Known facts feeding this plan: components sum 1,150k vs 1,525k measured worst (gap ≈ 375k; at `instrument_init` the gap is ≈ 470k); `EchoDelay::Process` calls libm `std::tanh` per sample per channel (`engine/fx/flux.h`); `PartFx::process` calls libm `std::sin` per sample (`engine/fx/part_fx.cpp:45`); `Limiter` with drive > 0 defeats its bit-exact bypass and runs `std::tanh` ×2 per sample (`engine/fx/limiter.h`); the bench's worst case runs CHOKE = 0, so the `max_voice_env()` per-sample scan (`engine/instrument.cpp:82-84`) is unpriced; `fx_grit` measures Drive mode only; `Comp` is cleared of suspicion (`kDecimate = 16`, isolated 8.5k). Modulo in `DeLine` is by a compile-time constant (multiply-shift, not sdiv) — a pow2 buffer would save a little, not a lot; do not treat it as a headline suspect.

---

## Global Constraints

- Repository: `c:\Users\bernd\Documents\AI\Spotykach` (the `spotymod` fork). Branch: `main`. Work directly on `main`.
- **This plan touches NOTHING under `engine/`, `src/`, or `third_party/` — it is pure bench + docs.** If a row seems to need an engine accessor that does not exist, the row is redesigned, not the engine.
- **Ordering: this plan lands AFTER the mod-plane plan's Task 2** (`docs/superpowers/plans/2026-07-18-mod-plane-optimization.md`). Both plans edit `bench/workload.h`, `bench/main.cpp`, `bench/Makefile` and `bench/runner.cpp`; the analysis here also consumes the mod family's rows (`super_mod_5lanes`, `center_tick`) and the post-`fast_sin` baseline. Verify precedence in Task 1 before writing anything.
- Bench measurement conditions are fixed and must not be touched: 48 kHz, block size 96, block budget 960 000 cycles, D-cache and I-cache on, `-ffast-math -funroll-loops`.
- Bench file basenames must stay globally unique across `bench/` and the `engine/` sources — libDaisy's `core/Makefile` derives object names with `$(notdir ...)`. `workloads_abl.cpp` is clear.
- Every line the bench firmware emits carries its own trailing `\n` — `SYS_WRITE0` appends nothing.
- **Precision discipline:** in prose, quote percentages to whole numbers and ratios to two significant figures. Intra-run jitter is ~1700 cycles on a 1.5M-cycle workload; a cross-build layout shift once moved a 29k-cycle workload by ~7 %. The micro rows are 1k–20k cycles — quote them as ranges, not single-cycle truths.
- Commit messages follow the repo's style (`feat(bench):`, `docs:`). Every commit trailer:
  ```
  Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
  ```
- Do not add a `Claude`/`Anthropic` co-author trailer.

## Hardware procedure (applies to every task that runs the bench)

- Before every run, confirm no `openocd` process is alive. A leftover holds the ST-Link and the next attempt dies with `LIBUSB_ERROR_ACCESS`.
- Run in the **foreground** with a bounded timeout so the command returns: `cd /c/Users/bernd/Documents/AI/Spotykach && timeout 900 python bench/run.py 2>&1 | tail -90`. Do not background it, do not poll, do not park waiting.
- Do not leave a shell sitting inside `bench/` — Windows locks the directory.
- `--repeat 2` is the default; both passes must be checksum-identical.
- **A person nearby will hear two bursts of harsh noise** at the end of each pass. That is anchor mode and it is expected.
- The bench binary requires an attached openocd. Without one the first `bkpt 0xAB` halts the core, looking exactly like a hang.

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `bench/workloads_abl.cpp` | The fourteen `abl` family rows | Create |
| `bench/workload.h` | `kAblWorkloads` / `kAblCount` externs | Modify |
| `bench/main.cpp` | Run the fifth family | Modify |
| `bench/Makefile` | Compile the new source | Modify |
| `bench/runner.cpp` | `find_workload` covers the fifth table | Modify |
| `docs/bench/` | Regenerated result files | Modify (regenerate) |
| `docs/roadmap.md` | Record the closed budget | Modify |

---

### Task 1: Preflight

- [ ] **Step 1: Confirm the mod-plane plan has landed**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
git log --oneline -8
ls bench/workloads_mod.cpp
grep -n "fast_sin" engine/mod/waveforms.h
```

Expected: `workloads_mod.cpp` exists and `wave_sine` already uses `fast_sin`. If either is missing, **stop** — this plan's baseline and two of its subtraction terms (`super_mod_5lanes`, `center_tick`) do not exist yet, and the integration edits would collide with in-flight work.

- [ ] **Step 2: Confirm the engine signatures the rows call**

```bash
grep -n "set_inhibit\|max_voice_env\|void set_step\|active_voices" engine/parts/part.h
grep -n "set_choke\|set_step\|set_voice_decay\|trigger_manual\|active_voices" engine/instrument.h
grep -n "set_on\|set_mode\|set_intensity\|set_mix" engine/fx/grit.h
grep -n "Init\|SetLagTime\|SetDelayTime\|SetFeedback" engine/fx/flux.h
```

Expected (verified against source 2026-07-18): `Part::set_inhibit(bool)`, `Part::max_voice_env()`, `Instrument::set_choke(float)` (clamps −1..1), `Instrument::set_step(int, bool, int)`, `Grit::set_on(bool, bool)`, `Grit::set_mode(GritMode)`, `EchoDelay<N>::Init(float, float*)`. Adapt any call whose signature differs and say so in your report.

Also confirm the arena sizes in `bench/mem.h`: `kSramFloats = 16 * 1024` — the short-echo rows use the WHOLE SRAM arena for one channel, which is why they are mono and `kShortEcho` is 16384.

---

### Task 2: The `abl` family

- [ ] **Step 1: Create `bench/workloads_abl.cpp`**

```cpp
#include "workload.h"
#include "mem.h"
#include <cmath>
#include "instrument.h"
#include "parts/part.h"
#include "fx/part_fx.h"
#include "fx/flux.h"
#include "fx/grit.h"
#include "fx/limiter.h"
#include "util/fast_sin.h"

namespace bench {
namespace {

using namespace spky;

float g_out_l[kBlock], g_out_r[kBlock];

// --- micro rows: one primitive, 96 data-dependent calls ---------------------
//
// These four price every per-sample libm suspicion in one sweep: FLUX's tanh
// (x2 channels x2 parts), the driven limiter's tanh (x2), PartFx's rev-send
// sinf (x2 parts), and the engine's own fast_sin as the yardstick. Arguments
// come from the noise block so -ffast-math cannot fold the loop; results feed
// the checksum. 96 calls = the per-sample-per-block cost of ONE call site.
void setup_micro() { (void)test_input(); }

float proc_micro_sinf() {
    const float* in = test_input();
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) acc += std::sin(in[i] * 3.1f);
    return acc;
}
float proc_micro_tanhf() {
    const float* in = test_input();
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) acc += std::tanh(in[i] * 2.5f);
    return acc;
}
float proc_micro_powf() {
    const float* in = test_input();
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) acc += std::pow(2.f, in[i]);
    return acc;
}
float proc_micro_fast_sin() {
    const float* in = test_input();
    float acc = 0.f;
    // fast_sin(p) is sin(2*pi*p) on p in [0,1): map the bipolar noise inside.
    for (size_t i = 0; i < kBlock; ++i) acc += fast_sin(in[i] * 0.49f + 0.5f);
    return acc;
}

// --- part glue: a full Part that never sounds a voice -----------------------
//
// Everything Part::process does per sample EXCEPT audible voices: 11 lane
// target evaluations, the quantizer, ChordBuilder::set_color + apply, the
// virtual set_targets/set_chord/process calls, and the PartFx shell (blocks
// off). Boot order matters: init() arms the FLOW drone promise
// (_auto_pending), and set_inhibit(true) -> set_hold(true) clears it BEFORE
// the first process(), so no voice ever triggers. Lanes stay in FLOW at full
// cost — deliberately, since STEP lanes are fire-gated and cheaper.
//
//   glue = this row − super_mod_5lanes − engine intercept − fx_none
//
// where the engine intercept is the synth family's zero-voice fixed cost
// (extrapolated: synth_1 − (synth_2 − synth_1), ~16k on the 7e99b74 run).
// No echo buffers are passed, so FLUX can never engage and the row never
// touches SDRAM.
Part g_glue;

void setup_part_glue()
{
    g_glue.init(kSampleRate, 7u);
    g_glue.set_inhibit(true);     // clears the boot drone; lane fires suppressed
}
float proc_part_glue()
{
    float acc = 0.f, l, r, sl, sr;
    for (size_t i = 0; i < kBlock; ++i) {
        g_glue.process(l, r, sl, sr);
        acc += l + r + sl + sr;
    }
    // Guard: if any voice ever sounds, this moves the checksum. The row's
    // meaning depends on max_voice_env() == 0 for the whole window.
    acc += g_glue.max_voice_env();
    return acc;
}

// --- instrument ablations: worst case with exactly one change ---------------
//
// setup_worst() mirrors workloads_system.cpp's setup_inst_worst exactly (keep
// them in sync by eye when either changes). Each variant then removes or adds
// ONE thing; the delta against instrument_worst is that thing's in-context
// cost, and the excess of that delta over the solo rows is composition
// coupling (cache eviction between components) — the effect no solo row can
// see. Own instance + own retrigger counter: the system family's g_inst_ctr
// phase must not leak in here (same trap the system file documents).
Instrument g_abl_inst;
int        g_abl_ctr = 0;

void setup_worst(bool flux_on, float reverb_mix)
{
    g_abl_inst.init(kSampleRate, fx_mem());
    g_abl_inst.set_tempo_bpm(120.f);
    g_abl_ctr = 0;
    for (int p = 0; p < PART_COUNT; ++p) {
        g_abl_inst.set_color(p, 1.f);
        g_abl_inst.set_density(p, 1.f);
        g_abl_inst.set_depth(p, 1.f);
        g_abl_inst.set_rate(p, 0.8f);
        g_abl_inst.set_fx_on(p, FxBlock::Grit, true);
        g_abl_inst.set_fx_on(p, FxBlock::Flux, flux_on);
        g_abl_inst.set_grit_mix(p, 1.f);
        g_abl_inst.set_flux_mix(p, 1.f);
        g_abl_inst.set_comp(p, 1.f);
        g_abl_inst.set_voice_decay(p, 1.f);
        g_abl_inst.trigger_manual(p);
    }
    g_abl_inst.set_reverb_mix(reverb_mix);
    g_abl_inst.set_reverb_size(1.f);
    g_abl_inst.set_reverb_decay(0.95f);
    g_abl_inst.set_reverb_diffusion(0.9f);
    g_abl_inst.set_reverb_smear(1.f);
    g_abl_inst.set_reverb_mod(1.f);
    g_abl_inst.set_master_drive(1.f);
}

void setup_worst_noflux()   { setup_worst(false, 0.5f); }
void setup_worst_noreverb() { setup_worst(true,  0.f);  }

// CHOKE engaged — the state the system family's worst case runs without.
// choke = -1 makes part A the priority side; A goes to STEP so its gate is
// only high ~5 ms per fire and the stage-2 window falls through to the
// max_voice_env() per-sample scan (instrument.cpp) — the exact code path
// that is currently unpriced. B keeps its FLOW voices decaying (~16 s at
// decay 1), so the row still carries 8 active voices.
void setup_worst_choked()
{
    setup_worst(true, 0.5f);
    g_abl_inst.set_step(PART_A, true, 8);
    g_abl_inst.set_choke(-1.f);
    g_abl_inst.trigger_manual(PART_A);
}

float proc_abl_inst()
{
    const float* in = test_input();
    g_abl_inst.process(in, in, g_out_l, g_out_r, kBlock);
    if (++g_abl_ctr >= 250) {
        g_abl_ctr = 0;
        g_abl_inst.trigger_manual(PART_A);
        g_abl_inst.trigger_manual(PART_B);
    }
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) acc += g_out_l[i] + g_out_r[i];
    acc += static_cast<float>(g_abl_inst.active_voices(PART_A));
    acc += static_cast<float>(g_abl_inst.active_voices(PART_B));
    return acc;
}

// --- master limiter, clean vs driven ----------------------------------------
//
// At drive 0 with peaks under the -1 dBFS knee the limiter is a bit-exact
// early return. At drive 1 the pre-gain (x4) defeats that bypass and shape()
// runs libm tanh on BOTH channels EVERY sample. instrument_worst sets
// master_drive(1), so the delta between these rows is inside THE number, and
// no existing row prices it.
Limiter g_lim;

void setup_lim_clean()  { g_lim.init(); g_lim.set_drive(0.f); }
void setup_lim_driven() { g_lim.init(); g_lim.set_drive(1.f); }

float proc_lim()
{
    const float* in = test_input();
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) {
        float l = in[i] * 0.7f, r = in[i] * 0.63f;
        g_lim.process(l, r);
        acc += l + r;
    }
    return acc;
}

// --- EchoDelay core, SRAM vs SDRAM ------------------------------------------
//
// The same template FLUX uses (EchoDelay<N> is header-only), instantiated
// small enough to fit the 16k-float SRAM arena, mono, identical settings in
// both regions. 0.25 s at 48 kHz = a 12 000-sample read offset — far outside
// any cache, so the SDRAM row's scatter is honest. The pair splits FLUX's
// isolated 77k into memory tax (sdram − sram) and compute (bpf + tanh +
// per-sample SetDelay; cross-check the tanh share against micro_tanhf).
// One shipping FLUX = 2 channels; the full instrument runs 2 parts = x4.
constexpr size_t kShortEcho = 16 * 1024;   // == kSramFloats, whole arena, mono

EchoDelay<kShortEcho> g_echo_abl;

void setup_echo_region(float* buf)
{
    g_echo_abl.Init(kSampleRate, buf);
    g_echo_abl.SetLagTime(0.03f);          // same slew the shipping Flux sets
    g_echo_abl.SetDelayTime(0.25f, true);
    g_echo_abl.SetFeedback(0.7f);
}
void setup_echo_sram()  { setup_echo_region(sram_arena());  }
void setup_echo_sdram() { setup_echo_region(sdram_arena()); }

float proc_echo()
{
    const float* in = test_input();
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) acc += g_echo_abl.Process(in[i]);
    return acc;
}

// --- GRIT, both modes -------------------------------------------------------
//
// fx_grit measures Drive-in-shell only; Reduce (decimator + bitcrush +
// reducer) has never had a row. Standalone Grit, full mix, so the two modes
// compare directly without the PartFx shell in the way.
Grit g_grit;

void setup_grit_mode(GritMode m)
{
    g_grit.init(kSampleRate);
    g_grit.set_mode(m);
    g_grit.set_on(true, true);
    g_grit.set_intensity(0.8f);
    g_grit.set_mix(1.f);
}
void setup_grit_drive()  { setup_grit_mode(GritMode::Drive);  }
void setup_grit_reduce() { setup_grit_mode(GritMode::Reduce); }

float proc_grit()
{
    const float* in = test_input();
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) {
        float l = in[i], r = in[i] * 0.9f;
        g_grit.process(l, r);
        acc += l + r;
    }
    return acc;
}

} // namespace

const Workload kAblWorkloads[] = {
    { "abl", "micro_sinf",          setup_micro,          proc_micro_sinf     },
    { "abl", "micro_tanhf",         setup_micro,          proc_micro_tanhf    },
    { "abl", "micro_powf",          setup_micro,          proc_micro_powf     },
    { "abl", "micro_fast_sin",      setup_micro,          proc_micro_fast_sin },
    { "abl", "part_glue_flow",      setup_part_glue,      proc_part_glue      },
    { "abl", "inst_worst_noflux",   setup_worst_noflux,   proc_abl_inst       },
    { "abl", "inst_worst_noreverb", setup_worst_noreverb, proc_abl_inst       },
    { "abl", "inst_worst_choked",   setup_worst_choked,   proc_abl_inst       },
    { "abl", "limiter_clean",       setup_lim_clean,      proc_lim            },
    { "abl", "limiter_driven",      setup_lim_driven,     proc_lim            },
    { "abl", "echo_short_sram",     setup_echo_sram,      proc_echo           },
    { "abl", "echo_short_sdram",    setup_echo_sdram,     proc_echo           },
    { "abl", "grit_drive_solo",     setup_grit_drive,     proc_grit           },
    { "abl", "grit_reduce_solo",    setup_grit_reduce,    proc_grit           },
};
const int kAblCount = sizeof(kAblWorkloads) / sizeof(kAblWorkloads[0]);

} // namespace bench
```

Two deliberate design points, do not "fix" them:
- `part_glue_flow` keeps the lanes in FLOW (no `set_step`) because STEP lanes are fire-gated and cheaper — the subtraction against `super_mod_5lanes` (a FLOW row) only works if both sides run FLOW.
- The short-echo rows are mono and reuse one instance for both regions; each setup re-`Init`s (which memsets the buffer), so row order cannot leak state.

If `part_glue_flow`'s guard shows a voice sounding anyway (checksum contains a nonzero env), the FLOW-inhibit boot assumption is wrong — fall back to `set_step(true, 8)` + `set_inhibit(true)`, rename the row `part_glue_step`, and correct the analysis with the mod family's `lane_step_shape00` vs `lane_flow_shape00` delta (×5 lanes). Report which variant ran.

- [ ] **Step 2: Integrate the family**

- `bench/workload.h`: add below the mod family externs:
  ```cpp
  extern const Workload kAblWorkloads[];
  extern const int      kAblCount;
  ```
- `bench/main.cpp`: add a fifth loop after the mod-family loop, before `run_anchors`, same shape as the others.
- `bench/Makefile`: add `workloads_abl.cpp` to `CPP_SOURCES`.
- `bench/runner.cpp`: add `kAblWorkloads` / `kAblCount` to `find_workload`'s `tables[]` / `counts[]`, keeping the loop bound in step.

- [ ] **Step 3: Build and run on hardware**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
timeout 900 python bench/run.py 2>&1 | tail -90
```

Expected: fourteen `BENCH,abl,...` rows, both passes checksum-identical, no `TIMEOUT`. The system/voice/mem/mod rows must be unchanged from the current committed baseline within jitter — this family only ADDS rows; if an existing row moved by more than ~2 %, suspect a layout shift and say so.

- [ ] **Step 4: Commit**

```bash
git add bench/
git commit -m "$(cat <<'EOF'
feat(bench): abl family -- close the instrument's unaccounted 39%

Fourteen rows: four libm micros (sinf/tanhf/powf vs fast_sin), a Part that
never sounds a voice (the per-sample glue, by subtraction), three worst-case
ablations (FLUX off, reverb asleep, CHOKE engaged), the master limiter clean
vs driven, the EchoDelay core SRAM vs SDRAM, and GRIT's two modes. Component
rows sum to ~120% of budget while instrument_worst measures ~159%; these
rows name the owners of the difference.

Measures only; nothing is optimised here.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 3: Close the budget on paper

No code. This task turns the fourteen numbers into the attribution the whole plan exists for.

- [ ] **Step 1: The closure arithmetic**

Work in average cycles, from THIS run's table (not 7e99b74's — the mod-plane fix moved the baseline). Compute and report each line explicitly:

1. **Glue per part** = `part_glue_flow` − `super_mod_5lanes` − engine intercept − `fx_none`, where the intercept = `synth_1_voice` − (`synth_2_voices` − `synth_1_voice`).
2. **In-context FLUX** = `instrument_worst` − `inst_worst_noflux`. Coupling_flux = that minus 2 × (`fx_flux_sdram` − `fx_none`).
3. **In-context reverb** = `instrument_worst` − `inst_worst_noreverb`. Coupling_reverb = that minus `oliverb_solo_sram`.
4. **CHOKE tax** = `inst_worst_choked` − `instrument_worst` (may be negative if suppression outweighs the scan — that is itself a finding: CHOKE is not a worst-case axis).
5. **Driven-limiter tax** = `limiter_driven` − `limiter_clean`.
6. **FLUX decomposition**: memory tax per channel = `echo_short_sdram` − `echo_short_sram`; ×4 ≈ the instrument's echo memory bill. tanh share ≈ 2 × `micro_tanhf` per channel. Whatever remains of (`fx_flux_sdram` − `fx_none`) after memory + tanh is bpf + interpolation + per-sample SetDelay.
7. **Closure test**: `super_mod_5lanes`×2 + `center_tick` + 2×glue + 2×`synth_4_voices` + 2×(full PartFx) + in-context reverb + driven-limiter tax should land within ~5 % of `instrument_worst`, where full PartFx = `fx_none` + (`fx_grit`−`fx_none`) + (`fx_flux_sdram`−`fx_none`) + (`fx_comp`−`fx_none`). Report the residual as a percentage of budget. If it is still >10 points, the dark matter has another owner — name the next ablation you would run, do not hand-wave it.

- [ ] **Step 2: Rank the cut list**

From the numbers, write the ranked list of next optimisations with predicted savings, one line each — e.g. "Part glue to control rate: −X %", "fast tanh in EchoDelay: −Y %", "half-rate reverb: −Z %". Include the two hygiene one-liners already known from source (`PartFx` rev-send `std::sin` → `fast_sin`; the double pitch quantization in `Part::process`) with their now-measured price ceilings (≤ `micro_sinf` ×2 parts).

- [ ] **Step 3: Update the docs and commit**

- `docs/bench/`: exactly one result pair committed, superseded pair `git rm`'d in the same commit (established convention).
- `docs/roadmap.md`: in the existing bench section's voice, record: the gap is now attributed (name the owners and their share), whether the 2×4 go/no-go conclusion moved, and the ranked cut list. Update `**Last updated:**`.
- Append `## Outcome (YYYY-MM-DD)` to THIS plan file with the closure residual and the ranking, so the next spec starts from figures.

```bash
git add docs/bench/ docs/roadmap.md docs/superpowers/plans/2026-07-18-bench-ablation-family.md
git commit -m "$(cat <<'EOF'
docs: the instrument budget closes -- ablation figures and the cut list

Records where the unaccounted 39% actually lives (Part glue, composition
coupling, the driven limiter), FLUX's memory/compute split, the CHOKE and
GRIT-Reduce prices, and the ranked list of cuts the next spec should be
written against.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

## Out of scope

- **No optimisation.** Every cut this family motivates gets its own spec, written against these figures. That includes the two "free" one-liners — they change engine output and belong with a listening pass.
- No engine, shipping-firmware, or third-party edits of any kind.
- No anchor-mode additions — the offline rows suffice for attribution; anchor stays the 3-row go/no-go set.
- No new instrument states beyond the three ablations (no STEP-vs-FLOW matrix, no per-lane sweeps — the mod family owns lane granularity).
- The firmware-shell cost (ADC/UI/LED/meter of the eventual 2×4 shell) stays unpriced here; it needs a shell to measure. Note it in the roadmap as standing headroom (~10 %) the engine budget must leave.

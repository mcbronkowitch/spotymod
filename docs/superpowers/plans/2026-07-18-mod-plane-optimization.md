# Mod-plane optimization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Decompose the modulation plane's measured 33 % of block budget into per-lane, per-shape and per-component bench rows, then make the one cut the evidence already points at — `waveforms.h` still calling libm `std::sin` per sample per lane — and prove the result with before/after numbers.

**Architecture:** A fourth workload family, `mod`, is added to the existing bench harness: seven rows isolating one `ModLane` at each of `shape_value()`'s four segments, one lane in STEP, one whole `SuperModulator`, and the `Center` tick alone. The existing `mod_plane_2x_center` row stays as the roll-up so the parts can be checked against the whole. Then `wave_sine` is redirected from `std::sin` to the engine's existing `fast_sin`, which the audio path has used since M2, and the same rows measure the effect. The plan ends at a decision checkpoint: no further cut is planned here, because later cuts depend on numbers that do not exist yet.

**Tech Stack:** C++17, arm-none-eabi-gcc + libDaisy `core/Makefile` for the bench firmware; CMake + Ninja + clang (`env.sh`) and doctest for the desktop unit tests; openocd 0.12 with an ST-Link V3; Python 3 standard library for `bench/run.py`.

**Spec:** `docs/superpowers/specs/2026-07-18-mod-plane-optimization-design.md`

---

## Global Constraints

- Repository: `c:\Users\bernd\Documents\AI\Spotykach` (the `spotymod` fork). Branch: `main`. Work directly on `main`.
- **The mod plane's output is allowed to change.** That is the spec's central decision. `fast_sin`'s error is < 1.2e-3 and the audio path has accepted it since M2. Do not try to preserve bit-identity in the mod plane, and do not add a compatibility switch.
- **The shipping firmware app is untouchable:** no edit to the repo-root `main.cpp`, `app.cpp`, `app.h`, `meter.h`, `common.h`, `alt_sram.lds`, the root `Makefile`, or `src/**`. This plan **does** edit `engine/mod/waveforms.h` — that is the point of it — but nothing else in `engine/` and nothing in `third_party/`.
- **`renders/` is gitignored and has never been tracked** (`.gitignore` line 11). Re-cut renders are a local listening artefact, never a commit and never a `git diff` check. A previous plan in this repo made exactly that mistake; do not repeat it.
- Bench measurement conditions are fixed and must not be touched: 48 kHz, block size 96, block budget 960 000 cycles, D-cache and I-cache on, `-ffast-math -funroll-loops`.
- Bench file basenames must stay globally unique across `bench/` and the twelve `engine/` sources — libDaisy's `core/Makefile` derives object names with `$(notdir ...)`, so a collision silently clobbers. `workloads_mod.cpp` is clear.
- Every line the bench firmware emits carries its own trailing `\n` — `SYS_WRITE0` appends nothing.
- **Precision discipline:** in prose, quote percentages to whole numbers and ratios to two significant figures. Intra-run jitter is ~1700 cycles on a 1.5M-cycle workload. Data tables keep full precision.
- Commit messages follow the repo's style (`feat(bench):`, `perf(mod):`, `docs:`). Every commit trailer:
  ```
  Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
  ```
- Do not add a `Claude`/`Anthropic` co-author trailer.

## Hardware procedure (applies to every task that runs the bench)

- Before every run, confirm no `openocd` process is alive. A leftover holds the ST-Link and the next attempt dies with `LIBUSB_ERROR_ACCESS`.
- Run in the **foreground** with a bounded timeout so the command returns: `cd /c/Users/bernd/Documents/AI/Spotykach && timeout 900 python bench/run.py 2>&1 | tail -70`. Do not background it, do not poll, do not park waiting.
- Do not leave a shell sitting inside `bench/` — Windows locks the directory.
- `--repeat 2` is the default; both passes must be checksum-identical.
- **A person nearby will hear two bursts of harsh noise** at the end of each pass. That is anchor mode and it is expected. The third anchor segment sounds broken on purpose.
- The bench binary requires an attached openocd. Without one the first `bkpt 0xAB` halts the core, looking exactly like a hang.

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `bench/workloads_mod.cpp` | The seven `mod` family rows | Create |
| `bench/workload.h` | `kModWorkloads` / `kModCount` externs | Modify |
| `bench/main.cpp` | Run the fourth family | Modify |
| `bench/Makefile` | Compile the new source | Modify |
| `engine/mod/waveforms.h` | `wave_sine` uses `fast_sin` instead of `std::sin` | Modify |
| `docs/bench/` | Regenerated result files | Modify (regenerate) |
| `docs/roadmap.md` | Record the optimization and the new figure | Modify |

---

### Task 1: The `mod` decomposition family

Seven rows that turn one number into a breakdown. Nothing is optimised in this task — its deliverable is the baseline the next task is measured against.

**Files:**
- Create: `bench/workloads_mod.cpp`
- Modify: `bench/workload.h`, `bench/main.cpp`, `bench/Makefile`
- Test: the Daisy Seed, via `python bench/run.py`

**Interfaces:**
- Consumes: `bench::Workload`, `bench::run_workload`, `kBlock`, `kSampleRate`, `kBudgetCycles` from `bench/workload.h`; `spky::ModLane`, `spky::SuperModulator`, `spky::Center`, `spky::Part`.
- Produces: `extern const Workload bench::kModWorkloads[]; extern const int bench::kModCount;`

- [ ] **Step 1: Confirm the engine signatures you are about to call**

These are the calls this task depends on. Check them before writing, because a wrong guess fails with a confusing template error:

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
grep -n "void init\|float process\|void set_shape\|void set_step\|void set_rate_hz\|bool  fired" engine/mod/lane.h
grep -n "void init\|void process\|float lane_output\|void set_rate\|void set_shape\|void set_density" engine/mod/super_modulator.h
grep -n "void init\|void update\|float morph\|float weather" engine/center/center.h
```

Expected: `ModLane::init(float, uint32_t)`, `ModLane::process()` returning `float`, `set_shape(float)`, `set_step(bool, int)`, `set_rate_hz(float)`, `fired()`; `SuperModulator::init(float, uint32_t)`, `process()` taking no arguments, `lane_output(int)`; `Center::init(float, uint32_t)` and `Center::update(SuperModulator&, SuperModulator&, Part&, Part&)` — **four** arguments. Adapt any call whose signature differs and say so in your report.

- [ ] **Step 2: Write the mod family**

Create `bench/workloads_mod.cpp`:

```cpp
#include "workload.h"
#include "mod/lane.h"
#include "mod/lane_id.h"
#include "mod/super_modulator.h"
#include "center/center.h"
#include "parts/part.h"

namespace bench {
namespace {

using namespace spky;

// --- one lane, FLOW, at each of shape_value()'s four segments ---------------
//
// shape_value() switches on floor(shape * 4), and ONLY segment 0 evaluates
// wave_sine. So the spread across these four rows is itself the measurement:
// if shape0 costs materially more than shape3, the libm sine is the reason,
// and the size of that gap is what the next task removes.
ModLane g_lane;

void setup_lane_flow(float shape)
{
    g_lane.init(kSampleRate, 5u);
    g_lane.set_rate_hz(2.f);
    g_lane.set_shape(shape);
    g_lane.set_density(1.f);      // every boundary fires; no frozen-hold shortcut
    g_lane.set_smooth(0.5f);
    g_lane.set_step(false, 8);    // FLOW: _compute_raw runs every sample
}

void setup_lane_flow_s00() { setup_lane_flow(0.f);  }   // sine -> triangle
void setup_lane_flow_s03() { setup_lane_flow(0.3f); }   // triangle -> ramp
void setup_lane_flow_s07() { setup_lane_flow(0.7f); }   // ramp -> pulse
void setup_lane_flow_s10() { setup_lane_flow(1.f);  }   // pure S&H

// STEP: does _compute_raw really run only on a fire? If so this row should
// come in far below the FLOW rows at the same shape.
void setup_lane_step()
{
    g_lane.init(kSampleRate, 5u);
    g_lane.set_rate_hz(2.f);
    g_lane.set_shape(0.f);        // same segment as setup_lane_flow_s00
    g_lane.set_density(1.f);
    g_lane.set_smooth(0.5f);
    g_lane.set_step(true, 8);
}

float proc_lane()
{
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) {
        acc += g_lane.process();
        // Fold the fire flag in: a lane whose phase never advances would
        // otherwise be indistinguishable from a working one by checksum.
        if (g_lane.fired()) acc += 1.f;
    }
    return acc;
}

// --- one whole SuperModulator (LANE_COUNT lanes) ----------------------------
// Compared against 5x lane_flow_shape00 this shows whether the bank adds cost
// above its lanes, or is just their sum.
SuperModulator g_sm;

void setup_super_mod()
{
    g_sm.init(kSampleRate, 1u);
    g_sm.set_rate(0.5f);
    g_sm.set_density(1.f);
    g_sm.set_shape(0.f);          // same segment as the single-lane rows
    g_sm.set_smooth(0.5f);
}

float proc_super_mod()
{
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) {
        g_sm.process();
        acc += g_sm.lane_output(LANE_PITCH);
    }
    return acc;
}

// --- the Center tick alone --------------------------------------------------
// Center::update runs once per kCtrlInterval (96) samples, i.e. exactly once
// per block. The modulators and parts here are hooks it writes through; they
// are deliberately NOT advanced, so this row prices the tick and nothing else.
SuperModulator g_c_a, g_c_b;
Center         g_center;
Part           g_c_pa, g_c_pb;

void setup_center_tick()
{
    g_c_a.init(kSampleRate, 1u);
    g_c_b.init(kSampleRate, 2u);
    g_c_pa.init(kSampleRate, 1u);
    g_c_pb.init(kSampleRate, 2u);
    g_center.init(kSampleRate, 11u);
    g_center.set_morph(0.5f);
    g_center.set_couple(0.7f);    // grid-gravity zone: the servo maths runs
    g_center.set_drift(0.7f);     // weather OU process active
}

float proc_center_tick()
{
    g_center.update(g_c_a, g_c_b, g_c_pa, g_c_pb);
    return g_center.morph() + g_center.weather();
}

} // namespace

const Workload kModWorkloads[] = {
    { "mod", "lane_flow_shape00", setup_lane_flow_s00, proc_lane      },
    { "mod", "lane_flow_shape03", setup_lane_flow_s03, proc_lane      },
    { "mod", "lane_flow_shape07", setup_lane_flow_s07, proc_lane      },
    { "mod", "lane_flow_shape10", setup_lane_flow_s10, proc_lane      },
    { "mod", "lane_step_shape00", setup_lane_step,     proc_lane      },
    { "mod", "super_mod_5lanes",  setup_super_mod,     proc_super_mod },
    { "mod", "center_tick",       setup_center_tick,   proc_center_tick },
};
const int kModCount = sizeof(kModWorkloads) / sizeof(kModWorkloads[0]);

} // namespace bench
```

- [ ] **Step 3: Declare the table**

In `bench/workload.h`, add below the `kMemWorkloads` externs:

```cpp
extern const Workload kModWorkloads[];
extern const int      kModCount;
```

- [ ] **Step 4: Run the family**

In `bench/main.cpp`, add a fourth loop immediately after the `kMemCount` loop and before `run_anchors`:

```cpp
    for (int i = 0; i < bench::kModCount; ++i) {
        const bench::Workload& w = bench::kModWorkloads[i];
        bench::report_row(w, bench::run_workload(w));
    }
```

In `bench/Makefile`, add `workloads_mod.cpp` to `CPP_SOURCES` after `workloads_memory.cpp`.

Also extend `find_workload` in `bench/runner.cpp` so anchor mode can still look rows up across all four tables — find the `tables[]` / `counts[]` arrays and add `kModWorkloads` / `kModCount` to both, keeping the arrays' length in step with the loop bound.

- [ ] **Step 5: Build and run on hardware**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
timeout 900 python bench/run.py 2>&1 | tail -70
```

Expected: seven `BENCH,mod,...` rows, both passes checksum-identical, no `TIMEOUT`.

What to read out of the table, and report each explicitly:
- **`lane_flow_shape00` versus `lane_flow_shape03`.** The gap is the libm sine. If they are within noise of each other, the sine hypothesis is wrong and the next task's prediction fails before it starts — say so rather than proceeding quietly.
- **`lane_flow_shape10`** should be the cheapest — pure S&H, no waveform maths at all.
- **`lane_step_shape00` versus `lane_flow_shape00`.** A large gap confirms `_compute_raw` is fire-gated in STEP; a small one means it runs regardless, which is itself a finding.
- **`super_mod_5lanes` versus 5 × `lane_flow_shape00`.** Roughly equal means the bank is just its lanes; materially higher means there is overhead worth a look.
- **`center_tick`** against the roll-up. Center runs once per block, so even expensive transcendentals should amortise to a small share. If it is large, that reverses the plan's priorities.

Sanity-check the parts against the whole: `2 × super_mod_5lanes + center_tick` should land near `mod_plane_2x_center` (~315 000 cycles). A large mismatch means one of these rows is not measuring what its name says — the failure mode this project has hit twice — and must be resolved before Task 2.

- [ ] **Step 6: Commit**

```bash
git add bench/
git commit -m "$(cat <<'EOF'
feat(bench): mod family -- decompose the modulation plane

Seven rows turning the plane's single 33% figure into a breakdown: one lane
in FLOW at each of shape_value()'s four segments, one lane in STEP, a whole
SuperModulator, and the Center tick alone. Only segment 0 evaluates
wave_sine, so the spread across the shape rows prices the libm sine
directly.

Measures only; nothing is optimised here. This is the baseline the next
commit is judged against.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 2: `wave_sine` uses `fast_sin`

The cut. One line in the engine, then the same rows measure what it bought.

**Files:**
- Modify: `engine/mod/waveforms.h`
- Test: `tests/` via ctest (desktop), then the Daisy Seed via `bench/run.py`

**Interfaces:**
- Consumes: Task 1's `mod` rows as the before-baseline.
- Produces: nothing new. `wave_sine`'s signature is unchanged.

- [ ] **Step 1: Record the before-figures**

From Task 1's run, write down `lane_flow_shape00`, `lane_flow_shape03`, `super_mod_5lanes` and `mod_plane_2x_center` — average cycles and percent. These are the numbers Step 5 compares against. Put them in your report before making any change.

- [ ] **Step 2: Make the change**

In `engine/mod/waveforms.h`, add the include under the existing ones:

```cpp
#include "util/fast_sin.h"
```

and replace the `wave_sine` line:

```cpp
inline float wave_sine(float ph)     { return std::sin(ph * TWO_PI); }
```

with:

```cpp
// fast_sin(p) IS sin(2*pi*p), so this is a drop-in for std::sin(ph * TWO_PI).
// The audio path has used it since M2 (see util/fast_sin.h: ~10-15 cycles on
// the M7 against ~80-120 for libm sinf); the modulation path was simply never
// moved over. Error < 1.2e-3, which is inaudible on an LFO but can shift an
// individual S&H or gate decision by a sample, since those sit on thresholds.
inline float wave_sine(float ph)     { return fast_sin(ph); }
```

That is the entire code change in this task. Do not touch anything else in `engine/`.

- [ ] **Step 3: Run the desktop unit tests**

These are the structural net: lane behaviour, gate density, step-clock, variation. They do not assert exact bits, which is why they are the right gate for a change that deliberately alters output.

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
source env.sh
cmake -S . -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

Expected: PASS, all tests.

If a test fails, **read it before concluding anything.** A test asserting a specific gate pattern or fire count may be recording a threshold decision that `fast_sin`'s 1.2e-3 error legitimately shifted — the spec anticipates exactly this. A test asserting *structure* (a lane fires at all, density scales monotonically, STEP quantises to the grid) failing is a real regression. Report which kind you are looking at, with the test name and its assertion, and do not weaken a test to make it pass.

- [ ] **Step 4: Re-cut the reference renders for listening**

```bash
./build/render.exe host/render/scenarios/ambient_wash.json /tmp/wash.wav /tmp/wash.csv
./build/render.exe host/render/scenarios/dorian_vs_drift.json /tmp/dorian.wav /tmp/dorian.csv
```

These are **local listening artefacts for Bastian, not a gate and not a commit** — `/renders/` is gitignored (`.gitignore` line 11) and has never been tracked, so do not `git add` anything under it and do not use `git diff` on it to check for change. Note in your report that the renders are available at those paths.

- [ ] **Step 5: Commit the code change — before measuring, and this order matters**

```bash
git add engine/mod/waveforms.h
git commit -m "$(cat <<'EOF'
perf(mod): wave_sine uses fast_sin, as the audio path has since M2

The modulation path still called libm std::sin once per sample per lane
while the audio path moved to the engine's own fast_sin in M2 for exactly
this reason. Ten lanes across both parts made that the single largest item
in a modulation plane the bench measured at 33% of block budget against a
4-6% estimate.

Error < 1.2e-3: inaudible on an LFO, but S&H and gate decisions sit on
thresholds, so an individual trigger can shift by a sample. Reference
renders are re-cut; their byte-identity is no longer a gate, which the spec
accepts deliberately.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

**Why commit first.** `bench/run.py` names its result files after the hash the firmware reports, and guards that against `git rev-parse --short HEAD`. With the change uncommitted, the firmware *contains* it while HEAD still points at Task 1's commit — the two hashes match, so the guard stays silent, and the result file is filed under a commit that does not contain the change it measured. That is the same silent-mislabelling failure the bench plan already had to fix once. Commit, then measure.

- [ ] **Step 6: Re-measure on hardware**

```bash
timeout 900 python bench/run.py 2>&1 | tail -70
```

Expected: both passes checksum-identical. **Every checksum in every family will differ from Task 1's run** — the mod plane's output changed, which is the point, and it feeds the instrument rows too. That is expected; what must hold is agreement between the two passes of *this* run.

Now settle the spec's prediction, explicitly:

> Ten lanes × ~170 cycles for `sinf` ≈ 1700 cycles per sample ≈ 52 % of the plane's 3288 cycles per sample, so `mod_plane_2x_center` should fall from about 33 % to about 17 %.

Report the actual before/after for `mod_plane_2x_center`, `lane_flow_shape00` and `super_mod_5lanes`, and state plainly whether the prediction held, overshot or fell short. Two cross-checks that make the answer harder to fool yourself about:
- `lane_flow_shape00` should now cost about what `lane_flow_shape03` costs, since neither evaluates a libm sine any more. If a gap remains, something else lives in segment 0.
- `lane_flow_shape03`, `shape07` and `shape10` should be **unchanged** from Task 1 — they never called the sine. If they moved, the change reached further than intended, or the measurement is noisier than assumed.

Also report `instrument_worst`, which should drop by roughly the plane's saving.

The result files this run writes are committed in Task 3, together with the roadmap entry and the checkpoint verdict — they belong with the write-up rather than with the code change.

---

### Task 3: Record the result and stop at the checkpoint

The plan ends here by design. Later cuts depend on figures that did not exist when this plan was written, and a task saying "optimise whatever turns out to be expensive" would be a placeholder in disguise.

**Files:**
- Modify: `docs/bench/` (regenerated), `docs/roadmap.md`
- Test: none — this task writes up measurements already taken.

**Interfaces:**
- Consumes: Task 2's post-change run. Produces nothing for later tasks.

- [ ] **Step 1: Regenerate the committed result files**

Task 2's run already wrote them. Confirm exactly one result pair is committed and the superseded pair is deleted in the same commit:

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
ls docs/bench/
git status --short docs/bench/
```

Expected: one `.md` and one `.csv`, named after the current short hash. If the previous pair is still present, `git rm` it so exactly one result set remains — that is the convention the bench plan established.

- [ ] **Step 2: Update the roadmap**

Read 40 lines of surrounding context first and match the existing voice, heading depth and table style — do not invent a new format.

In `docs/roadmap.md`, find the bench section added by the previous plan and record, in the surrounding prose style:
- that the modulation plane was measured at 33 % against a 4–6 % estimate, and what it costs now;
- that the cause was `waveforms.h` calling libm `std::sin` per sample per lane while the audio path had used `fast_sin` since M2;
- the new `instrument_worst` figure and whether the 2×4 conclusion is unchanged;
- that the mod plane's output changed deliberately, so `renders/` byte-identity is no longer a regression gate for it.

Quote percentages to whole numbers and ratios to two significant figures. Also update the `**Last updated:**` line.

- [ ] **Step 3: Write the checkpoint verdict**

Append a short section to the spec at `docs/superpowers/specs/2026-07-18-mod-plane-optimization-design.md`, headed `## Outcome (YYYY-MM-DD)`, stating in a few sentences:
- the prediction as written, and whether it held;
- the decomposition table's ranking — what each part actually costs;
- what the next-largest item in the mod plane now is, with its figure;
- a recommendation on whether a follow-up spec is worth writing at all, or whether the plane is now cheap enough to leave alone.

This is the deliverable that decides whether there is more work, so make the recommendation explicit rather than leaving the reader to infer it.

- [ ] **Step 4: Commit**

```bash
git add docs/bench/ docs/roadmap.md docs/superpowers/specs/2026-07-18-mod-plane-optimization-design.md
git commit -m "$(cat <<'EOF'
docs: the mod-plane numbers, before and after

Records what the decomposition found, whether the spec's prediction held,
and what the plane costs now. Ends at the checkpoint the spec set: any
further cut gets its own spec, written against these figures rather than
against estimates.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

## Out of scope

- **No control-rate rework of the lanes.** Computing lane values every 8–16 samples and interpolating through the existing `_slew` could be worth a factor of 8–16, but STEP firing must stay sample-accurate and at SMOOTH 0 the slew is near-passthrough, where stepping would become audible. Revisit only if the decomposition justifies the risk.
- **No second cut.** Whatever Task 1 ranks after the sine gets its own spec and plan, written against real figures.
- No FLUX, reverb, or voice-count work — those are separate specs, and the mod plane alone cannot bring the instrument under 100 % (zeroing it entirely still leaves ~132 %).
- No change to lane semantics: rates, ratios, gate logic, phrase generation and groove all stay as they are.
- No new panel control, parameter id, or VCV panel change.
- No listening pass — that is Bastian's, after landing.

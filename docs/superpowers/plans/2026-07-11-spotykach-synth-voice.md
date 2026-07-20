# Spotykach Synth Voice (M2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the 4-voice polyphonic synth engine (`engine/synth/`) that replaces `TestToneEngine` as the boot-default `IPartEngine` — trigger-driven overlapping voices in STEP, a sustaining-last-voice drone in FLOW — fully testable on the desktop renderer.

**Architecture:** A new `engine/synth/` directory holds a polyblep morphing oscillator (`MorphOsc`), an exponential AD/ADS envelope (`Env`), a `Voice` (2× MorphOsc + sub sine + DaisySP `Svf` lowpass + envelope + equal-power pan with deterministic drift), and a `SynthEngine : IPartEngine` doing round-robin allocation, oldest-steal-with-retrigger-from-level, and the FLOW drone logic. `IPartEngine` gains default-no-op `set_cycle`/`set_flow`; `Part` gains `set_engine(EngineId)` with a click-free SoftSwitch crossfade and forwards cycle/flow; `Instrument` and the render host expose the new API 1:1. All audio-path sines go through a shared polynomial `fast_sin` (no libm `sinf`); drift LFOs and envelope coefficients update at control rate (per 96-sample block).

**Tech Stack:** C++17, CMake + Ninja + clang (via `env.sh`), doctest, DaisySP (vendored submodule at `lib/DaisySP` — `Svf` only for the voice filter), nlohmann/json (host only).

**Spec:** `docs/superpowers/specs/2026-07-11-spotykach-synth-voice-design.md` (residency repo); master design `2026-07-10-spotykach-modulation-first-synth-design.md`, section "Engine 2 — Synth voice".

**Milestone context:** This is the M2 plan (after M1, scales, and M1.6/FX; before M3 capture sequencer). All code lands in the firmware fork at `c:\Users\bernd\Documents\AI\Spotykach` (branch `main`); this plan file lives in the residency repo.

## M1.6 end-state assumption (read first)

M1.6 (FX) is **mid-implementation in the fork** (as of writing, the fork HEAD is at M1.6 Task 8 — engine FX done, host wiring Tasks 9–10 pending). **This plan assumes M1.6 is FINISHED per the M1.6 plan** (`2026-07-11-spotykach-fx.md`). Where current fork code and the M1.6 plan differ, target the M1.6 end state:

- `CMakeLists.txt` has the `daisysp_min` static lib (5 DaisySP sources — **without** `svf.cpp`, which Task 4 of THIS plan adds) linked into `spky_tests` and `render`.
- `engine/fx/fx_util.h` provides `spky::SoftSwitch` (reused here for click-free engine switching).
- `Part::process(outL, outR, sendL, sendR)` runs engine → PartFx (GRIT→FLUX→FX MIX) → reverb-send tap; the 2-arg `process(l, r)` wrapper discards sends. M2 does not change the FX chain.
- `host/render/scenario.cpp` already dispatches the 10 M1.6 FX actions; `host/render/main.cpp` already injects `FxMem` (static echo buffers + static `AmbientReverb`) and writes `a_fx0..a_fx4` / `b_fx0..b_fx4` CSV columns. Task 8 of this plan appends to that end state.
- `host/render/scenarios/dub_delay.json` and `ambient_wash.json` exist (Task 8 updates them along with the 5 M1 scenario files).

If a Task 8/9 file from M1.6 turns out not to exist yet when you get there, **stop and report** — M1.6 must be completed first (prerequisite per the M2 spec).

## Global Constraints

Every task's requirements implicitly include this section. Copied verbatim from the M1 plan, then amended for M1.6/M2.

- **Language / standard:** C++17 (`set(CMAKE_CXX_STANDARD 17)`).
- **Namespace:** all new engine and host code is in `namespace spky` (distinct from the existing firmware `namespace spotykach`, which the shell reconciles in M6).
- **No hardware types in `engine/`:** no `#include <daisy.h>`, `<daisysp.h>`, or anything under `src/`. The engine compiles standalone.
- **No heap / no allocation in the audio path:** engine members are fixed-size (`std::array`, plain fields). No `new`, `malloc`, `std::vector`, or `std::function` inside `engine/`. `std::vector`/file I/O are allowed only in `host/` and `tests/`.
- **Determinism:** all randomness uses the engine's `spky::Rng` (xorshift32). Never `std::rand`, `rand()`, `<random>` distributions, or time-based seeds anywhere in `engine/`.
- **Parameter smoothing:** knob→engine slews go through `spky::OnePole` (the portable copy of the firmware's one-pole smoother).
- **Sample rate / block:** engine is sample-rate agnostic via `init(sample_rate)`. Firmware target is 96 samples @ 48 kHz; the offline render host processes sample-by-sample (block size is a firmware CPU concern, not an offline one).
- **Do not modify** `src/`, `Makefile`, `main.cpp`, `app.cpp`, or `app.h`.

**M1.6 amendment** (supersedes the `<daisysp.h>` clause of the no-hardware-types line): individual DaisySP module headers (`#include "Filters/svf.h"` etc.) ARE allowed in `engine/` since M1.6 — DaisySP is portable C++ and a desktop build dependency (`daisysp_min`). libDaisy remains banned.

**M2 binding constraints** (from the M2 spec's CPU budget — load-bearing for the < 70 % worst case):

- **No libm `sinf` in the voice audio path.** `MorphOsc` and the sub oscillator use the shared polynomial `spky::fast_sin` (`engine/util/fast_sin.h`, Task 1) — one implementation for desktop and firmware so renders stay bit-identical across hosts. `std::sin`/`std::pow`/`std::exp` remain allowed at **control rate** (per-block coefficient math, trigger-time pitch mapping) and in tests/host.
- **Control-rate updates:** drift LFOs and envelope-coefficient recomputation run once per 96-sample block (`SynthEngine::kCtrlInterval = 96`), not per sample. The desktop per-sample loop honors this via a sample counter (every 96 calls).
- **Build/test command** (run from the fork root `c:\Users\bernd\Documents\AI\Spotykach`, every task):
  `source env.sh && cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure`
- All normalized parameters are `0..1` floats, clamped at the API boundary (existing convention).
- Existing tests must stay green after every task.
- Determinism invariant: identical scenario ⇒ bit-identical WAV.
- Commit messages follow repo style (`feat(engine):`, `feat(host):`, `test(engine):`, `docs:`) and end with:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`

## Locked design decisions (resolved from the spec, do not re-litigate)

1. **Pitch mapping helper is `110·8^p` via `std::pow` at trigger/control rate** (same numbers as `TestToneEngine`); the per-sample oscillator core never calls libm.
2. **Envelope shape:** attack = one-pole rise toward an overshoot target 1.2 (crosses 1.0 in exactly the programmed attack time, ln 6 law); decay = one-pole fall toward the sustain level, `decay_s` defined as the −60 dB time (ln 1000 law); a voice goes idle at level < 1e-4 (−80 dB), i.e. ~1.33 × `decay_s` after the peak. FLOW demotion = `set_sustain(0)` — the same decay coefficient now converges to zero ("released: decays to zero at the decay rate", per spec).
3. **Edit-knob → ratio maps (exponential):** ATTACK ratio = `0.002 · 250^n` (0.2 %…50 % of cycle), DECAY ratio = `0.1 · 80^n` (0.1×…8× cycle). Boot defaults are stored directly as ratios (0.02 and 1.5) — the spec fixes the *defaults*, not their knob positions.
4. **Shape phase alignment in `MorphOsc`:** sine (fast_sin), triangle peaking at phase 0.25, saw crossing zero rising at phase 0.5 (wrap discontinuity polyblepped), 50 % pulse (+1 first half, both edges polyblepped) — all aligned so the morph never jumps in level. Triangle is naive (derivative discontinuity only), per the spec's v1 scope.
5. **TIMBRE macro:** `morph = t`, `detune_cents = t² · DETUNE_MAX` split ± half across osc A/B — t = 0 is exact unison AND pure sine.
6. **MOTION drift:** per voice, one pan-drift LFO and one detune-drift LFO (`fast_sin` on slow phases), rates drawn once from `spky::Rng` in `Voice::init` (0.05–0.2 Hz), pan drift amplitude ±0.25 · width, detune drift ±3 ct · width. Deterministic per seed; `Part` decorrelates the two parts by seeding `SynthEngine` from its own `seed_base`.
7. **Engine switch = fade-out → swap → fade-in** through one `SoftSwitch` (4 ms Hann each way) applied as a master gain on the engine output inside `Part::process`. The swap (and re-forwarding of flow/cycle state to the incoming engine) happens at the idle point between the fades. At the fade's hold state the multiplier is exactly 1.0, so unswitched operation stays bit-identical (the M1.6 bypass acceptance test keeps passing).
8. **Voice gain staging:** each voice contributes `(0.5·(oscA+oscB) + sub) → Svf low → env → pan`, summed across 4 voices and scaled by `kVoiceGain = 0.22` × smoothed LEVEL. Worst case (4 voices, sub 1.0, res low) stays inside ±1.25 pre-part-mix — inside the existing ±1.5 test bound and the WAV clamp.
9. **`set_flow` re-entry semantics:** the auto-trigger fires only on a false→true transition with no sustaining voice, and is *deferred to the next `process()` call* (targets are guaranteed fresh there — `Part` calls `set_targets` every sample before `process`). Switching engines away and back does NOT retrigger: the synth keeps its sustaining voice frozen while inactive and resumes it, which satisfies the drone promise without a spurious note.
10. **Existing engine-level tests are engine-agnostic** (verified by reading `tests/test_part.cpp` / `tests/test_instrument.cpp`: they assert lane/target routing, quantization, FX-bypass bit-exactness and comparative audibility — none pins test-tone waveform specifics). They stay untouched; new tests pin the new boot default. The **scenario JSONs** were sound-designed for the test tone and DO get explicit `set_engine: test_tone` lines (Task 8), per the spec's "update rather than weaken the boot default" note.

## File Structure

All paths relative to the fork root `c:\Users\bernd\Documents\AI\Spotykach`.

| File | Action | Responsibility |
|------|--------|----------------|
| `engine/util/fast_sin.h` | create | shared polynomial sine (audio-path sine for desktop + firmware) |
| `engine/synth/morph_osc.h` | create | polyblep morphing oscillator (phasor, morph, detune in cents) |
| `engine/synth/env.h` | create | AD/ADS exponential envelope, retrigger-from-level |
| `engine/synth/voice.h/.cpp` | create | one voice: 2× MorphOsc + sub + Svf + env + pan/drift |
| `engine/synth/synth_engine.h/.cpp` | create | `IPartEngine` impl: allocation, drone logic, target mapping |
| `engine/parts/engine_iface.h` | modify | `EngineId` enum; `set_cycle`/`set_flow` default no-ops |
| `engine/parts/part.h/.cpp` | modify | `set_engine` + SoftSwitch crossfade, flow/cycle forwarding, `trigger_manual`, voice API forwards, boot default ENGINE_SYNTH |
| `engine/instrument.h` | modify | `set_engine`, `set_voice_*`, `trigger_manual`, `active_voices`, `voice_env`, `engine_id`; `set_step` routed via Part |
| `CMakeLists.txt` | modify | `svf.cpp` into `daisysp_min`; new engine/test sources into `spky_tests` + `render` |
| `host/render/scenario.cpp` | modify | 7 new scenario actions |
| `host/render/main.cpp` | modify | `voices` + `v0..v3` CSV columns per part |
| `host/render/scenarios/*.json` (7 existing) | modify | explicit `set_engine: test_tone` init lines |
| `host/render/scenarios/overlapping_voices.json` | create | M2 acceptance demo: STEP Dorian melody, prob ~0.6, long decay |
| `host/render/scenarios/flow_drone.json` | create | FLOW drone, breathing TIMBRE/FILTER, probability swells |
| `tests/test_fast_sin.cpp` | create | accuracy vs `std::sin`, bounded error |
| `tests/test_morph_osc.cpp` | create | frequency, anchor shapes, bounds, detune beat |
| `tests/test_env.cpp` | create | attack/decay timing, sustain hold, retrigger-from-level |
| `tests/test_voice.cpp` | create | latch, pan law, drift determinism, steal continuity |
| `tests/test_synth_engine.cpp` | create | allocation, tempo coupling, drone, pitch contract, MOTION, determinism |
| `tests/test_part.cpp` | modify | boot default, FLOW/STEP promises, manual trigger, engine switch, cycle forwarding |
| `tests/test_instrument.cpp` | modify | API reach-through, boot default |
| `tests/test_scenario.cpp` | modify | new actions reach the instrument |
| `docs/roadmap.md`, `README.md` | modify | M2 status |

**How the build grows** (M1 convention): engine `.cpp` files and test `.cpp` files are compiled directly into the `spky_tests` executable; each task appends its new sources to that target's source list. Engine `.cpp` files needed by the CLI are ALSO appended to the `render` executable's source list.

### Build environment

Same as M1/M1.6 (validated on this machine): no MSVC, LLVM/clang + Ninja via the gitignored `env.sh` at the fork root. Every build/test invocation in this plan means, from `c:\Users\bernd\Documents\AI\Spotykach`:

```bash
source env.sh
cmake -S . -B build          # after CMakeLists edits
cmake --build build
ctest --test-dir build --output-on-failure
```

With the Ninja single-config generator the `spky_tests` / `render` binaries are directly under `build/`.

---

## Task 1: `fast_sin` — shared polynomial sine

**Files:**
- Create: `engine/util/fast_sin.h`
- Test: `tests/test_fast_sin.cpp`
- Modify: `CMakeLists.txt` (add `tests/test_fast_sin.cpp` to `spky_tests`)

**Interfaces:**
- Consumes: nothing (header-only, no dependencies).
- Produces: `spky::fast_sin(float phase) → float` — evaluates `sin(2π·phase)` for ANY float phase (wraps internally), max absolute error < 2e-3 vs libm, exactly 0 at phase 0 and 0.5, exactly ±1 at 0.25/0.75. This is the ONLY sine allowed in the M2 audio path (Tasks 2, 4).

- [ ] **Step 1: Write the failing tests**

`tests/test_fast_sin.cpp`:
```cpp
#include <doctest/doctest.h>
#include <cmath>
#include "util/fast_sin.h"
#include "util/math.h"
using namespace spky;

TEST_CASE("fast_sin: exact at the quadrant anchors") {
    CHECK(fast_sin(0.f)   == doctest::Approx(0.f));
    CHECK(fast_sin(0.25f) == doctest::Approx(1.f));
    CHECK(fast_sin(0.5f)  == doctest::Approx(0.f));
    CHECK(fast_sin(0.75f) == doctest::Approx(-1.f));
    CHECK(fast_sin(1.f)   == doctest::Approx(0.f));      // wraps
    CHECK(fast_sin(1.25f) == doctest::Approx(1.f));      // wraps
    CHECK(fast_sin(-0.75f) == doctest::Approx(1.f));     // negative phase wraps
}

TEST_CASE("fast_sin: bounded error vs std::sin across many cycles") {
    float max_err = 0.f;
    for (int i = 0; i <= 100000; ++i) {
        float ph = -3.f + i * (9.f / 100000.f);          // [-3, 6): includes wraps
        float ref = static_cast<float>(std::sin(static_cast<double>(ph) * 6.283185307179586));
        float err = std::fabs(fast_sin(ph) - ref);
        if (err > max_err) max_err = err;
    }
    CHECK(max_err < 2e-3f);
}

TEST_CASE("fast_sin: output never leaves [-1, 1] (beyond float fuzz)") {
    for (int i = 0; i <= 20000; ++i) {
        float v = fast_sin(i / 20000.f);
        CHECK(v >= -1.0001f);
        CHECK(v <=  1.0001f);
    }
}
```

- [ ] **Step 2: Add to build and run — expect RED**

Add `tests/test_fast_sin.cpp` to the `spky_tests` source list in `CMakeLists.txt` (after `tests/test_part_fx.cpp`), then:
```bash
source env.sh && cmake -S . -B build && cmake --build build 2>&1 | tail -5
```
Expected: build FAILS — `util/fast_sin.h: No such file or directory`.

- [ ] **Step 3: Implement `fast_sin`**

`engine/util/fast_sin.h`:
```cpp
#pragma once
#include <cmath>

namespace spky {

// Polynomial sine approximation on NORMALIZED phase: returns sin(2*pi*phase).
// Parabola + odd cubic refinement (the classic devmaster/Capens fast sine);
// max abs error < 1.2e-3, exact at 0 / 0.25 / 0.5 / 0.75.
//
// This is the audio-path sine for the M2 synth voice (MorphOsc core, sub
// oscillator, pan-law + drift-LFO evaluation): ~10-15 cycles on the M7 vs
// ~80-120 for libm sinf, which is what keeps the 8-voice worst case inside
// the CPU budget (spec "CPU budget"). One shared implementation so desktop
// renders and firmware output stay bit-identical. std::floor is the only
// libm call (cheap, and typically a single instruction).
inline float fast_sin(float phase) {
    float p = phase - std::floor(phase);                 // wrap to [0, 1)
    float q = p < 0.5f ? p : p - 1.f;                    // [-0.5, 0.5)
    float aq = q < 0.f ? -q : q;
    float y = 8.f * q * (1.f - 2.f * aq);                // parabola through the anchors
    float ay = y < 0.f ? -y : y;
    return 0.225f * (y * ay - y) + y;                    // refine toward true sine
}

} // namespace spky
```

- [ ] **Step 4: Build and run — expect GREEN**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```
Expected: PASS (all existing tests + 3 new fast_sin cases).

- [ ] **Step 5: Commit**

```bash
git add engine/util/fast_sin.h tests/test_fast_sin.cpp CMakeLists.txt
git commit -m "feat(engine): shared polynomial fast_sin for the voice audio path

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 2: `MorphOsc` — polyblep morphing oscillator

**Files:**
- Create: `engine/synth/morph_osc.h`
- Test: `tests/test_morph_osc.cpp`
- Modify: `CMakeLists.txt` (add `tests/test_morph_osc.cpp` to `spky_tests`)

**Interfaces:**
- Consumes: `spky::fast_sin` (Task 1); `spky::clampf`, `lerpf` (`util/math.h`).
- Produces: `spky::MorphOsc` (header-only) with `init(float sample_rate)`, `set_freq(float hz)`, `set_detune_cents(float ct)` (multiplicative ratio `2^(ct/1200)`, control rate — uses `std::pow`), `set_morph(float m)` (0 = sine, 1/3 = triangle, 2/3 = saw, 1 = pulse; continuous blend), `reset_phase(float ph = 0.f)`, `process() → float` (advance one sample, bipolar output, polyblep-corrected saw/pulse). Task 4's `Voice` owns two of these.

- [ ] **Step 1: Write the failing tests**

`tests/test_morph_osc.cpp`:
```cpp
#include <doctest/doctest.h>
#include <cmath>
#include <vector>
#include "synth/morph_osc.h"
#include "util/fast_sin.h"
using namespace spky;

static int rising_crossings(const std::vector<float>& v) {
    int n = 0;
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i - 1] <= 0.f && v[i] > 0.f) ++n;
    return n;
}

static std::vector<float> run_osc(float freq, float morph, float detune_ct, int n) {
    MorphOsc o;
    o.init(48000.f);
    o.set_morph(morph);
    o.set_detune_cents(detune_ct);
    o.set_freq(freq);
    std::vector<float> out(n);
    for (auto& s : out) s = o.process();
    return out;
}

TEST_CASE("morph_osc: frequency accuracy via zero crossings at every anchor shape") {
    for (float m : { 0.f, 1.f / 3.f, 2.f / 3.f, 1.f }) {
        auto v = run_osc(220.f, m, 0.f, 48000 * 5);
        int n = rising_crossings(v);
        CHECK(n >= 220 * 5 - 3);
        CHECK(n <= 220 * 5 + 3);
    }
}

TEST_CASE("morph_osc: anchor shapes match the ideal waveforms away from blep edges") {
    // 100 Hz -> exactly 480 samples per cycle; sample i sits at phase (i+1)/480.
    const int cyc = 480;
    auto sine = run_osc(100.f, 0.f, 0.f, cyc);
    auto tri  = run_osc(100.f, 1.f / 3.f, 0.f, cyc);
    auto saw  = run_osc(100.f, 2.f / 3.f, 0.f, cyc);
    auto pul  = run_osc(100.f, 1.f, 0.f, cyc);
    for (int i = 0; i < cyc - 1; ++i) {
        float t = (i + 1) / static_cast<float>(cyc);
        CHECK(sine[i] == doctest::Approx(fast_sin(t)).epsilon(0.01));
        float tri_ref = t < 0.25f ? 4.f * t : (t < 0.75f ? 2.f - 4.f * t : 4.f * t - 4.f);
        CHECK(tri[i] == doctest::Approx(tri_ref).epsilon(0.02));
        bool near_wrap = t < 0.02f || t > 0.98f;              // polyblep regions
        if (!near_wrap)
            CHECK(saw[i] == doctest::Approx(2.f * t - 1.f).epsilon(0.02));
        bool near_step = near_wrap || std::fabs(t - 0.5f) < 0.02f;
        if (!near_step)
            CHECK(pul[i] == doctest::Approx(t < 0.5f ? 1.f : -1.f).epsilon(0.02));
    }
}

TEST_CASE("morph_osc: output bounded across the full morph sweep") {
    for (int mi = 0; mi <= 20; ++mi) {
        auto v = run_osc(880.f, mi / 20.f, 0.f, 9600);
        for (float s : v) {
            CHECK(s >= -1.05f);
            CHECK(s <=  1.05f);
        }
    }
}

TEST_CASE("morph_osc: detune in cents shifts frequency by the expected ratio") {
    auto base = run_osc(220.f, 0.f, 0.f, 48000 * 10);
    auto det  = run_osc(220.f, 0.f, 50.f, 48000 * 10);
    int n0 = rising_crossings(base);
    int n1 = rising_crossings(det);
    // 50 ct -> ratio 2^(50/1200) = 1.02930 -> 226.45 Hz -> ~64 extra cycles / 10 s
    // (this cycle-count difference IS the beat frequency between the two).
    CHECK(n1 - n0 >= 58);
    CHECK(n1 - n0 <= 70);
}
```

- [ ] **Step 2: Add to build and run — expect RED**

Add `tests/test_morph_osc.cpp` to `spky_tests` in `CMakeLists.txt`, then:
```bash
source env.sh && cmake -S . -B build && cmake --build build 2>&1 | tail -5
```
Expected: build FAILS — `synth/morph_osc.h: No such file or directory`.

- [ ] **Step 3: Implement `MorphOsc`**

`engine/synth/morph_osc.h`:
```cpp
#pragma once
#include <cmath>
#include "util/fast_sin.h"
#include "util/math.h"

namespace spky {

// Band-limited morphing oscillator: ONE phasor, continuous morph
// sine -> triangle -> saw -> pulse (anchors at morph 0, 1/3, 2/3, 1), with
// polyblep corrections on the saw and pulse discontinuities. Shapes are
// phase-aligned (sine and triangle peak at phase 0.25; saw crosses zero
// rising at 0.5; pulse is +1 on the first half) so the morph never jumps.
// The triangle is naive (derivative discontinuity only) - v1 scope; the
// spec flags "polyblep the tri->saw midpoint" as an assumption to verify
// by ear, not a requirement.
//
// Audio path uses fast_sin only (no libm sinf - CPU budget constraint).
// set_detune_cents uses std::pow: control-rate only, never per sample.
class MorphOsc {
public:
    void init(float sample_rate) {
        _sr = sample_rate;
        _phase = 0.f;
        _ratio = 1.f;
        set_freq(220.f);
    }

    void set_freq(float hz) {
        _freq = hz < 0.f ? 0.f : hz;
        _inc = _freq * _ratio / _sr;
    }

    void set_detune_cents(float ct) {                    // control rate
        _ratio = std::pow(2.f, ct * (1.f / 1200.f));
        _inc = _freq * _ratio / _sr;
    }

    void set_morph(float m) { _morph = clampf(m, 0.f, 1.f); }

    void reset_phase(float ph = 0.f) { _phase = clampf(ph, 0.f, 0.999999f); }

    float process() {
        _phase += _inc;
        if (_phase >= 1.f) _phase -= 1.f;
        const float t = _phase;
        const float dt = _inc > 1e-6f ? _inc : 1e-6f;
        const float seg = _morph * 3.f;
        if (seg <= 1.f) return lerpf(fast_sin(t), _tri(t), seg);
        if (seg <= 2.f) return lerpf(_tri(t), _saw(t, dt), seg - 1.f);
        return lerpf(_saw(t, dt), _pulse(t, dt), seg - 2.f);
    }

private:
    // triangle phase-aligned with the sine: peak +1 at t=0.25, -1 at t=0.75
    static float _tri(float t) {
        if (t < 0.25f) return 4.f * t;
        if (t < 0.75f) return 2.f - 4.f * t;
        return 4.f * t - 4.f;
    }

    // standard 2-sample polyblep residual around a phase-wrap discontinuity
    static float _polyblep(float t, float dt) {
        if (t < dt) {
            float x = t / dt;
            return x + x - x * x - 1.f;
        }
        if (t > 1.f - dt) {
            float x = (t - 1.f) / dt;
            return x * x + x + x + 1.f;
        }
        return 0.f;
    }

    // saw rising through zero at t=0.5; -2 step at the wrap, blep-corrected
    static float _saw(float t, float dt) { return 2.f * t - 1.f - _polyblep(t, dt); }

    // 50% pulse: +2 step at the wrap, -2 step at t=0.5, both blep-corrected
    static float _pulse(float t, float dt) {
        float t2 = t + 0.5f;
        if (t2 >= 1.f) t2 -= 1.f;
        float v = t < 0.5f ? 1.f : -1.f;
        return v + _polyblep(t, dt) - _polyblep(t2, dt);
    }

    float _sr = 48000.f;
    float _phase = 0.f;
    float _freq = 220.f;
    float _ratio = 1.f;
    float _inc = 0.f;
    float _morph = 0.f;
};

} // namespace spky
```

- [ ] **Step 4: Build and run — expect GREEN**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```
Expected: PASS (frequency ±3 cycles over 5 s at all four anchors, shape matches, bounds, detune beat 58–70 cycles).

- [ ] **Step 5: Commit**

```bash
git add engine/synth/morph_osc.h tests/test_morph_osc.cpp CMakeLists.txt
git commit -m "feat(engine): MorphOsc - single-phasor polyblep morph sine->tri->saw->pulse

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 3: `Env` — AD/ADS exponential envelope

**Files:**
- Create: `engine/synth/env.h`
- Test: `tests/test_env.cpp`
- Modify: `CMakeLists.txt` (add `tests/test_env.cpp` to `spky_tests`)

**Interfaces:**
- Consumes: nothing (header-only; `<cmath>` for `std::exp` at control rate).
- Produces: `spky::Env` with `init(float sample_rate)`, `set_times(float attack_s, float decay_s)` (control rate — recomputes the two one-pole coefficients; callers pass already-clamped seconds), `set_sustain(float s)` (0 = AD, 0.7 = FLOW sustaining hold; setting 0 while holding IS the demotion release), `trigger()` (restarts the attack FROM THE CURRENT LEVEL — click-free steal), `process() → float` (per-sample level 0..1), `active() → bool`, `value() → float`. Task 4's `Voice` owns one; Task 5's engine drives sustain/demotion.

- [ ] **Step 1: Write the failing tests**

`tests/test_env.cpp`:
```cpp
#include <doctest/doctest.h>
#include "synth/env.h"
using namespace spky;

TEST_CASE("env: idle until triggered; attack reaches 1.0 in the programmed time") {
    Env e;
    e.init(48000.f);
    e.set_times(0.1f, 0.5f);
    CHECK(!e.active());
    CHECK(e.process() == 0.f);
    e.trigger();
    int n = 0;
    while (e.value() < 1.f && n < 48000) { e.process(); ++n; }
    CHECK(n >= static_cast<int>(0.1f * 48000 * 0.85f));   // ~4800 samples +/- 15%
    CHECK(n <= static_cast<int>(0.1f * 48000 * 1.15f));
}

TEST_CASE("env: AD decays to exact zero and goes idle (-80 dB cutoff)") {
    Env e;
    e.init(48000.f);
    e.set_times(0.002f, 0.5f);          // decay_s = time to -60 dB
    e.trigger();
    int n = 0;
    while (e.active() && n < 48000 * 3) { e.process(); ++n; }
    CHECK(!e.active());
    CHECK(e.process() == 0.f);          // exact zero once idle
    // idle threshold is -80 dB -> ~1.33 x the -60 dB decay time
    CHECK(n > static_cast<int>(0.5f * 48000));
    CHECK(n < static_cast<int>(0.5f * 48000 * 1.7f));
}

TEST_CASE("env: sustain holds near 0.7 (ADS); set_sustain(0) releases to zero") {
    Env e;
    e.init(48000.f);
    e.set_times(0.002f, 0.2f);
    e.set_sustain(0.7f);
    e.trigger();
    for (int i = 0; i < 48000; ++i) e.process();          // well past attack + decay
    CHECK(e.active());
    CHECK(e.value() == doctest::Approx(0.7f).epsilon(0.02));
    e.set_sustain(0.f);                                    // FLOW demotion
    int n = 0;
    while (e.active() && n < 48000 * 2) { e.process(); ++n; }
    CHECK(!e.active());
}

TEST_CASE("env: retrigger restarts the attack from the current level (no jump)") {
    Env e;
    e.init(48000.f);
    e.set_times(0.05f, 0.3f);
    e.trigger();
    for (int i = 0; i < 12000; ++i) e.process();           // into the decay tail, still above the -80 dB idle cutoff (~21.6k samples)
    float before = e.value();
    CHECK(before > 0.f);
    CHECK(before < 1.f);
    e.trigger();
    float after = e.process();
    CHECK(after >= before);                                // rises, never drops
    CHECK(after - before < 0.01f);                         // and never jumps up
}
```

- [ ] **Step 2: Add to build and run — expect RED**

Add `tests/test_env.cpp` to `spky_tests` in `CMakeLists.txt`, then:
```bash
source env.sh && cmake -S . -B build && cmake --build build 2>&1 | tail -5
```
Expected: build FAILS — `synth/env.h: No such file or directory`.

- [ ] **Step 3: Implement `Env`**

`engine/synth/env.h`:
```cpp
#pragma once
#include <cmath>

namespace spky {

// AD / ADS envelope with exponential segments.
//
// - Attack: one-pole rise toward an overshoot target (1.2), so the level
//   crosses 1.0 in exactly the programmed attack time (ln 6 law), then
//   switches to decay. Because the rise starts from the CURRENT level,
//   trigger() doubles as the click-free retrigger-from-level used on steals.
// - Decay: one-pole fall toward the sustain level. decay_s is defined as the
//   time to fall 60 dB (ln 1000 law). Sustain > 0 = ADS hold (the FLOW
//   sustaining voice); sustain 0 = plain AD. Setting sustain to 0 while
//   holding IS the demotion release: the same coefficient now converges to
//   zero, i.e. "released: decays to zero at the decay rate" (spec).
// - Idle below 1e-4 (-80 dB): level snaps to exact 0 and process() is free.
//
// set_times uses std::exp - CONTROL RATE ONLY (SynthEngine calls it once per
// 96-sample block); process() is a multiply-add.
class Env {
public:
    void init(float sample_rate) {
        _sr = sample_rate;
        _level = 0.f;
        _sustain = 0.f;
        _stage = Stage::Idle;
        set_times(0.01f, 0.5f);
    }

    void set_times(float attack_s, float decay_s) {      // control rate
        if (attack_s < 1e-4f) attack_s = 1e-4f;
        if (decay_s  < 1e-3f) decay_s  = 1e-3f;
        _a_coef = 1.f - std::exp(-1.7918f / (attack_s * _sr));   // ln 6
        _d_coef = 1.f - std::exp(-6.9078f / (decay_s * _sr));    // ln 1000
    }

    void set_sustain(float s) { _sustain = s < 0.f ? 0.f : (s > 1.f ? 1.f : s); }

    void trigger() { _stage = Stage::Attack; }           // rises from current level

    float process() {
        switch (_stage) {
            case Stage::Idle:
                return 0.f;
            case Stage::Attack:
                _level += _a_coef * (kAttackTarget - _level);
                if (_level >= 1.f) { _level = 1.f; _stage = Stage::Decay; }
                break;
            case Stage::Decay:
                _level += _d_coef * (_sustain - _level);
                if (_sustain <= 0.f && _level < kSilence) {
                    _level = 0.f;
                    _stage = Stage::Idle;
                }
                break;
        }
        return _level;
    }

    bool  active() const { return _stage != Stage::Idle; }
    float value()  const { return _level; }

private:
    enum class Stage { Idle, Attack, Decay };

    static constexpr float kAttackTarget = 1.2f;   // overshoot: finite rise time
    static constexpr float kSilence = 1e-4f;       // -80 dB idle threshold

    float _sr = 48000.f;
    float _level = 0.f;
    float _sustain = 0.f;
    float _a_coef = 0.01f;
    float _d_coef = 0.001f;
    Stage _stage = Stage::Idle;
};

} // namespace spky
```

- [ ] **Step 4: Build and run — expect GREEN**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```
Expected: PASS (attack timing ±15 %, −80 dB idle inside 1.0–1.7× decay_s, sustain 0.7 hold, monotone retrigger).

- [ ] **Step 5: Commit**

```bash
git add engine/synth/env.h tests/test_env.cpp CMakeLists.txt
git commit -m "feat(engine): AD/ADS exponential envelope with retrigger-from-level

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 4: `Voice` — oscillators + filter + envelope + pan/drift

**Files:**
- Create: `engine/synth/voice.h`, `engine/synth/voice.cpp`
- Test: `tests/test_voice.cpp`
- Modify: `CMakeLists.txt` (add `lib/DaisySP/Source/Filters/svf.cpp` to `daisysp_min`; add `engine/synth/voice.cpp` to BOTH `spky_tests` and `render`; add `tests/test_voice.cpp` to `spky_tests`)

**Interfaces:**
- Consumes: `spky::MorphOsc` (Task 2), `spky::Env` (Task 3), `spky::fast_sin` (Task 1), `spky::Rng`, `spky::clampf`, `daisysp::Svf` (`Filters/svf.h` — the ONLY DaisySP module in the voice, per spec).
- Produces: `spky::Voice` with
  - `void init(float sample_rate, uint32_t seed)` — seeds the per-voice drift Rng (deterministic).
  - `void trigger(float freq_hz)` — latch pitch (applies to oscs immediately) + `Env::trigger()` (retrigger-from-level).
  - `void set_sustaining(bool on)` — sustain 0.7 (FLOW sustaining voice) vs 0 (AD); `set_sustaining(false)` on a holding voice is the demotion release.
  - `void set_pitch_hz(float freq_hz)` — FLOW sustaining voice tracks the target.
  - Control-rate feeds (Task 5 calls all of these once per 96-sample block): `set_env_times(float attack_s, float decay_s)`, `set_morph(float m)`, `set_detune_cents(float max_ct)` (split ± half across osc A/B), `set_sub_level(float n)`, `set_cutoff_hz(float hz)`, `set_resonance(float n)`, `set_pan(float pan)` (base pan −1..1), `set_drift_amount(float a)`, `void update_control(float dt_s)` (advances drift LFOs by `dt_s` seconds and refreshes osc frequencies + pan gains).
  - `void process(float& accL, float& accR)` — per-sample; ADDS into the accumulators (silent-idle voices add nothing and cost nothing).
  - `bool active() const`, `float env_value() const`.

- [ ] **Step 1: Write the failing tests**

`tests/test_voice.cpp`:
```cpp
#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include "synth/voice.h"
using namespace spky;

static void feed_clean(Voice& v) {          // pure centered sine, wide-open filter
    v.set_env_times(0.002f, 5.f);
    v.set_morph(0.f);
    v.set_detune_cents(0.f);
    v.set_sub_level(0.f);
    v.set_cutoff_hz(14000.f);
    v.set_resonance(0.1f);
    v.set_pan(0.f);
    v.set_drift_amount(0.f);
    v.update_control(0.002f);
}

static int rising_crossings(const std::vector<float>& v) {
    int n = 0;
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i - 1] <= 0.f && v[i] > 0.f) ++n;
    return n;
}

TEST_CASE("voice: an idle voice adds exactly nothing") {
    Voice v;
    v.init(48000.f, 42);
    feed_clean(v);
    float l = 0.f, r = 0.f;
    for (int i = 0; i < 1000; ++i) v.process(l, r);
    CHECK(l == 0.f);
    CHECK(r == 0.f);
}

TEST_CASE("voice: trigger latches the frequency (zero-crossing count)") {
    Voice v;
    v.init(48000.f, 42);
    feed_clean(v);
    v.set_sustaining(true);                 // hold at 0.7 for a steady tone
    v.trigger(311.13f);                     // = 110 * 8^0.5
    std::vector<float> out(48000);
    for (auto& s : out) {
        float l = 0.f, r = 0.f;
        v.process(l, r);
        s = l;
    }
    int n = rising_crossings(out);
    CHECK(n >= 308);
    CHECK(n <= 314);
}

TEST_CASE("voice: equal-power pan - hard left silences right, center is equal") {
    Voice v;
    v.init(48000.f, 42);
    feed_clean(v);
    v.set_sustaining(true);
    v.trigger(220.f);
    v.set_pan(-1.f);
    v.update_control(0.002f);
    float suml = 0.f, sumr = 0.f;
    for (int i = 0; i < 4800; ++i) {
        float l = 0.f, r = 0.f;
        v.process(l, r);
        suml += l * l;
        sumr += r * r;
    }
    CHECK(sumr < suml * 1e-4f);             // hard left: right ~ 0
    v.set_pan(0.f);
    v.update_control(0.002f);
    suml = sumr = 0.f;
    for (int i = 0; i < 4800; ++i) {
        float l = 0.f, r = 0.f;
        v.process(l, r);
        suml += l * l;
        sumr += r * r;
    }
    CHECK(suml == doctest::Approx(sumr).epsilon(0.01));   // center: equal power
}

TEST_CASE("voice: equal-power total stays constant across the pan range") {
    std::vector<float> powers;
    for (float pan : { -1.f, -0.5f, 0.f, 0.5f, 1.f }) {
        Voice v;
        v.init(48000.f, 42);
        feed_clean(v);
        v.set_sustaining(true);
        v.trigger(220.f);
        v.set_pan(pan);
        v.update_control(0.002f);
        for (int i = 0; i < 48000; ++i) {           // settle at sustain
            float l = 0.f, r = 0.f;
            v.process(l, r);
        }
        float p = 0.f;
        for (int i = 0; i < 4800; ++i) {
            float l = 0.f, r = 0.f;
            v.process(l, r);
            p += l * l + r * r;
        }
        powers.push_back(p);
    }
    float mn = *std::min_element(powers.begin(), powers.end());
    float mx = *std::max_element(powers.begin(), powers.end());
    CHECK(mx / mn < 1.03f);                          // ~constant loudness
}

TEST_CASE("voice: drift is deterministic per seed and differs across seeds") {
    auto run = [](uint32_t seed) {
        Voice v;
        v.init(48000.f, seed);
        feed_clean(v);
        v.set_drift_amount(1.f);
        v.set_sustaining(true);
        v.trigger(220.f);
        std::vector<float> out;
        out.reserve(48000);
        for (int i = 0; i < 48000; ++i) {
            if (i % 96 == 0) v.update_control(96.f / 48000.f);   // control rate
            float l = 0.f, r = 0.f;
            v.process(l, r);
            out.push_back(l);
        }
        return out;
    };
    auto a = run(7);
    auto b = run(7);
    auto c = run(8);
    CHECK(a == b);                                   // bit-identical per seed
    bool differs = false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != c[i]) differs = true;
    CHECK(differs);                                  // seeds decorrelate
}

TEST_CASE("voice: retrigger mid-decay has no output discontinuity (steal)") {
    Voice v;
    v.init(48000.f, 42);
    feed_clean(v);
    v.set_env_times(0.01f, 0.5f);
    v.trigger(220.f);
    float prev = 0.f, max_delta = 0.f;
    for (int i = 0; i < 24000; ++i) {
        if (i == 12000) v.trigger(440.f);            // hard steal to a new pitch
        float l = 0.f, r = 0.f;
        v.process(l, r);
        if (i > 0) max_delta = std::max(max_delta, std::fabs(l - prev));
        prev = l;
    }
    // A 440 Hz sine at full level moves ~0.058/sample; a from-zero restart
    // would jump by the pre-steal level (~0.5). Retrigger-from-level +
    // phase-continuous oscillators keep the delta at waveform scale.
    CHECK(max_delta < 0.12f);
}
```

- [ ] **Step 2: Add to build and run — expect RED**

In `CMakeLists.txt`:
1. Add `lib/DaisySP/Source/Filters/svf.cpp` to the `daisysp_min` source list (after the `reverbsc.cpp` line). The M1.6 module list did NOT include it — the voice filter is why it joins now. (`svf.cpp` includes `"svf.h"` from its own directory and `"dsp.h"` from `Utility`, already a PRIVATE include dir — no new include lines needed.)
2. Add `engine/synth/voice.cpp` to BOTH the `spky_tests` and `render` source lists.
3. Add `tests/test_voice.cpp` to `spky_tests`.

Run:
```bash
source env.sh && cmake -S . -B build && cmake --build build 2>&1 | tail -5
```
Expected: build FAILS — `synth/voice.h: No such file or directory`.

- [ ] **Step 3: Write the implementation**

`engine/synth/voice.h`:
```cpp
#pragma once
#include <cstdint>
#include "Filters/svf.h"
#include "synth/morph_osc.h"
#include "synth/env.h"

namespace spky {

// One synth voice (x4 per part):
//
//   MorphOsc A ─┐
//   MorphOsc B ─┼→ mix → Svf lowpass (FILTER) → Env (VCA) → equal-power pan
//   sub sine  ──┘
//
// plus a slow per-voice drift LFO pair (pan + micro-detune, ~0.05-0.2 Hz,
// rates drawn deterministically from spky::Rng at init). All parameters
// arrive from SynthEngine at CONTROL RATE (update_control, once per
// 96-sample block); process() is the pure per-sample audio path and uses
// fast_sin only. daisysp::Svf is the single DaisySP dependency (spec).
class Voice {
public:
    void init(float sample_rate, uint32_t seed);

    void trigger(float freq_hz);          // latch pitch + retrigger env from level
    void set_sustaining(bool on);         // FLOW: sustain 0.7; off = AD / demotion
    void set_pitch_hz(float freq_hz);     // FLOW sustaining voice tracks the target

    // control-rate parameter feeds
    void set_env_times(float attack_s, float decay_s);
    void set_morph(float m);
    void set_detune_cents(float max_ct);  // split +/- max_ct/2 across osc A/B
    void set_sub_level(float n);
    void set_cutoff_hz(float hz);
    void set_resonance(float n);
    void set_pan(float pan);              // base pan -1..1 (MOTION fan slot)
    void set_drift_amount(float a);       // 0..1 (proportional to MOTION width)
    void update_control(float dt_s);      // advance drift, refresh freqs + gains

    void process(float& accL, float& accR);   // adds into the accumulators

    bool  active() const { return _env.active(); }
    float env_value() const { return _env.value(); }

private:
    void _apply_freq();                   // osc A/B freq from pitch+detune+drift

    MorphOsc _osc_a;
    MorphOsc _osc_b;
    Env _env;
    daisysp::Svf _filt;

    float _sr = 48000.f;
    float _freq = 220.f;
    float _sub_phase = 0.f;
    float _sub_inc = 0.f;
    float _sub_level = 0.3f;
    float _detune_ct = 0.f;               // TIMBRE-spread detune (max, split half)
    float _pan_base = 0.f;
    float _drift_amt = 0.f;
    float _gain_l = 0.70710678f;
    float _gain_r = 0.70710678f;

    // slow deterministic drift (control-rate)
    float _drift_pan_phase = 0.f;
    float _drift_det_phase = 0.f;
    float _drift_pan_hz = 0.1f;
    float _drift_det_hz = 0.1f;
    float _drift_ct_cur = 0.f;            // current micro-detune drift (cents)
};

} // namespace spky
```

`engine/synth/voice.cpp`:
```cpp
#include "synth/voice.h"
#include <cmath>
#include "mod/rng.h"
#include "util/fast_sin.h"
#include "util/math.h"

using namespace spky;

namespace {
constexpr float kDriftDetuneCt = 3.f;     // micro-detune drift ceiling (spec: +/-3 ct)
constexpr float kDriftPanAmt   = 0.25f;   // pan drift ceiling around the fan slot
} // namespace

void Voice::init(float sample_rate, uint32_t seed) {
    _sr = sample_rate;
    _osc_a.init(sample_rate);
    _osc_b.init(sample_rate);
    _env.init(sample_rate);
    _filt.Init(sample_rate);
    _filt.SetFreq(2000.f);
    _filt.SetRes(0.15f);
    _filt.SetDrive(0.f);

    Rng rng;                               // used ONCE at init: deterministic
    rng.seed(seed);                        // per-voice drift character
    _drift_pan_hz    = 0.05f + 0.15f * rng.next_unipolar();   // 0.05..0.2 Hz
    _drift_det_hz    = 0.05f + 0.15f * rng.next_unipolar();
    _drift_pan_phase = rng.next_unipolar();
    _drift_det_phase = rng.next_unipolar();

    _sub_phase = 0.f;
    _drift_ct_cur = 0.f;
    set_pitch_hz(220.f);
    update_control(0.f);
}

void Voice::trigger(float freq_hz) {
    set_pitch_hz(freq_hz);                 // applies to the oscs immediately
    _env.trigger();                        // from current level: click-free steal
}

void Voice::set_sustaining(bool on) { _env.set_sustain(on ? 0.7f : 0.f); }

void Voice::set_pitch_hz(float freq_hz) {
    _freq = freq_hz < 0.f ? 0.f : freq_hz;
    _sub_inc = 0.5f * _freq / _sr;         // sub: one octave below
    _apply_freq();
}

void Voice::set_env_times(float attack_s, float decay_s) {
    _env.set_times(attack_s, decay_s);
}

void Voice::set_morph(float m) {
    _osc_a.set_morph(m);
    _osc_b.set_morph(m);
}

void Voice::set_detune_cents(float max_ct) { _detune_ct = max_ct; }
void Voice::set_sub_level(float n)   { _sub_level = clampf(n, 0.f, 1.f); }
void Voice::set_cutoff_hz(float hz)  { _filt.SetFreq(clampf(hz, 20.f, 0.3f * _sr)); }
void Voice::set_resonance(float n)   { _filt.SetRes(clampf(n, 0.f, 0.95f)); }
void Voice::set_pan(float pan)       { _pan_base = clampf(pan, -1.f, 1.f); }
void Voice::set_drift_amount(float a){ _drift_amt = clampf(a, 0.f, 1.f); }

void Voice::_apply_freq() {                // control-rate (std::pow inside)
    const float half = _detune_ct * 0.5f;
    _osc_a.set_detune_cents(half + _drift_ct_cur);
    _osc_b.set_detune_cents(-half - _drift_ct_cur);
    _osc_a.set_freq(_freq);
    _osc_b.set_freq(_freq);
}

void Voice::update_control(float dt_s) {
    _drift_pan_phase += _drift_pan_hz * dt_s;
    _drift_pan_phase -= std::floor(_drift_pan_phase);
    _drift_det_phase += _drift_det_hz * dt_s;
    _drift_det_phase -= std::floor(_drift_det_phase);

    const float drift_pan = fast_sin(_drift_pan_phase) * kDriftPanAmt * _drift_amt;
    _drift_ct_cur = fast_sin(_drift_det_phase) * kDriftDetuneCt * _drift_amt;
    _apply_freq();

    // equal-power pan: angle 0..0.25 turns; gl = cos, gr = sin (via fast_sin)
    const float pan = clampf(_pan_base + drift_pan, -1.f, 1.f);
    const float a = (pan + 1.f) * 0.125f;
    _gain_r = fast_sin(a);
    _gain_l = fast_sin(a + 0.25f);
}

void Voice::process(float& accL, float& accR) {
    if (!_env.active()) return;            // idle voices are free

    float s = 0.5f * (_osc_a.process() + _osc_b.process());
    _sub_phase += _sub_inc;
    if (_sub_phase >= 1.f) _sub_phase -= 1.f;
    s += _sub_level * fast_sin(_sub_phase);

    _filt.Process(s);
    s = _filt.Low() * _env.process();

    accL += s * _gain_l;
    accR += s * _gain_r;
}
```

- [ ] **Step 4: Build and run — expect GREEN**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```
Expected: PASS (all previous + 7 voice cases; the DaisySP `Svf` compiles into `daisysp_min` without warnings that stop the build).

- [ ] **Step 5: Commit**

```bash
git add engine/synth/voice.h engine/synth/voice.cpp tests/test_voice.cpp CMakeLists.txt
git commit -m "feat(engine): synth Voice - 2x MorphOsc + sub + Svf + env + pan/drift

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 5: `SynthEngine` — allocation, drone logic, target mapping (+ `IPartEngine` additions)

**Files:**
- Modify: `engine/parts/engine_iface.h` (add `EngineId`, `set_cycle`/`set_flow` default no-ops)
- Create: `engine/synth/synth_engine.h`, `engine/synth/synth_engine.cpp`
- Test: `tests/test_synth_engine.cpp`
- Modify: `CMakeLists.txt` (add `engine/synth/synth_engine.cpp` to BOTH `spky_tests` and `render`; add `tests/test_synth_engine.cpp` to `spky_tests`)

**Interfaces:**
- Consumes: `spky::Voice` (Task 4), `spky::IPartEngine`, `LaneId`, `spky::OnePole`, `clampf`.
- Produces (Tasks 6–8 depend on these exact names):
  - In `engine/parts/engine_iface.h`: `enum spky::EngineId { ENGINE_TEST_TONE = 0, ENGINE_SYNTH = 1 };` and two new virtuals on `IPartEngine` with default no-op bodies (so `TestToneEngine` and the M5 sampler ignore them): `virtual void set_cycle(float seconds) {}` and `virtual void set_flow(bool flow) {}`.
  - `spky::SynthEngine : IPartEngine` with `static constexpr int kVoices = 4`, `static constexpr int kCtrlInterval = 96`; `void set_seed(uint32_t seed)` (call BEFORE `init`; decorrelates the two parts' drift); overrides `init(float)`, `set_targets(const float*, float)`, `trigger(float pitch_norm)` (pitch latched: `freq = 110·8^p`), `process(float&, float&)`, `set_cycle(float seconds)`, `set_flow(bool flow)`; VOICE-edit-layer setters `set_attack(float n)` / `set_decay(float n)` / `set_resonance(float n)` / `set_sub(float n)` / `set_detune(float n)` (all normalized 0..1); introspection `int active_voices() const`, `float voice_env(int v) const`, `int sustain_voice() const` (−1 = none).
  - Target mapping (all continuous on all voices except PITCH, which is latched per voice): `LANE_SOURCE` = TIMBRE (morph = t, detune = t²·DETUNE_MAX split ±), `LANE_SIZE` = FILTER (cutoff `60·(14000/60)^n` Hz), `LANE_PITCH` = PITCH, `LANE_MOTION` = pan fan `[-1,+1,-0.5,+0.5]·width` + drift ∝ width, `LANE_LEVEL` = master gain through a `OnePole` (10 ms).

- [ ] **Step 1: Add `EngineId` + the two default no-op virtuals to `engine/parts/engine_iface.h`**

Replace the file's contents with:
```cpp
#pragma once
#include "mod/lane_id.h"

namespace spky {

// Selectable part engines. ENGINE_SYNTH is the boot default from M2 on;
// the test tone stays selectable (tests, A/B reference). The M5 sampler
// will extend this enum.
enum EngineId { ENGINE_TEST_TONE = 0, ENGINE_SYNTH = 1 };

// A part's sound engine. Consumes the 5 normalized target values; produces
// stereo audio. TestToneEngine (M1), SynthEngine (M2) and SamplerEngine
// (M5) implement the same interface behind the same Part.
class IPartEngine {
public:
    virtual ~IPartEngine() = default;
    virtual void init(float sample_rate) = 0;
    virtual void set_targets(const float* targets /*[LANE_COUNT]*/, float tune) = 0;
    virtual void trigger(float pitch_norm) = 0;
    virtual void process(float& outL, float& outR) = 0;

    // M2 additions - default no-ops so engines that don't care (test tone,
    // M5 sampler) ignore them. Part forwards both: cycle on change (not per
    // sample), flow on STEP/FLOW switches.
    virtual void set_cycle(float /*seconds*/) {}   // master-lane cycle length
    virtual void set_flow(bool /*flow*/) {}        // true = FLOW, false = STEP
};

} // namespace spky
```

- [ ] **Step 2: Write the failing tests**

`tests/test_synth_engine.cpp`:
```cpp
#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include "synth/synth_engine.h"
using namespace spky;

// Feed one set of targets: TIMBRE, FILTER, PITCH, MOTION, LEVEL by lane slot.
static void feed(SynthEngine& e, float pitch, float timbre = 0.f, float filter = 1.f,
                 float motion = 0.f, float level = 1.f) {
    float t[LANE_COUNT] = { timbre, filter, pitch, motion, level };
    e.set_targets(t, 0.5f);
}

// Fresh engine in "measurement" trim: pure sine, no sub, no detune, mono.
static void fresh(SynthEngine& e, uint32_t seed = 99) {
    e.set_seed(seed);
    e.init(48000.f);
    e.set_sub(0.f);
    e.set_detune(0.f);
    e.set_cycle(1.f);
    feed(e, 0.5f);
}

static std::vector<float> render_l(SynthEngine& e, int n) {
    std::vector<float> out(n);
    for (auto& s : out) {
        float l = 0.f, r = 0.f;
        e.process(l, r);
        s = l;
    }
    return out;
}

static int crossings(const std::vector<float>& v, size_t from = 0) {
    int n = 0;
    for (size_t i = from + 1; i < v.size(); ++i)
        if (v[i - 1] <= 0.f && v[i] > 0.f) ++n;
    return n;
}

TEST_CASE("synth: pitch contract - trigger(p) sounds at 110*8^p Hz, latched in STEP") {
    SynthEngine e;
    fresh(e);
    e.trigger(0.5f);                        // 311.13 Hz
    auto v1 = render_l(e, 48000);
    int n1 = crossings(v1, 4800);           // skip the attack; ~311 * 0.9 = 280
    CHECK(n1 >= 275);
    CHECK(n1 <= 285);

    SynthEngine e2;
    fresh(e2);
    e2.trigger(0.5f);
    feed(e2, 0.9f);                         // target moves AFTER the trigger...
    auto v2 = render_l(e2, 48000);
    CHECK(crossings(v2, 4800) == n1);       // ...a STEP voice must not follow
}

TEST_CASE("synth: round-robin fills 4 voices; 5th steals the oldest, click-free") {
    SynthEngine e;
    fresh(e);
    e.set_cycle(4.f);                       // long decay: all voices stay active
    for (int v = 0; v < 4; ++v) {
        e.trigger(0.2f + 0.15f * v);
        render_l(e, 960);                   // 20 ms apart
    }
    CHECK(e.active_voices() == 4);
    for (int v = 0; v < 4; ++v) CHECK(e.voice_env(v) > 0.f);

    float env0_at_steal = 0.f;              // voice 0 is the oldest
    float prev = 0.f, max_delta = 0.f;
    for (int i = 0; i < 9600; ++i) {
        if (i == 4800) {
            env0_at_steal = e.voice_env(0); // level the instant before the steal
            e.trigger(0.8f);                // 5th note: steal
        }
        float l = 0.f, r = 0.f;
        e.process(l, r);
        if (i > 0) max_delta = std::max(max_delta, std::fabs(l - prev));
        prev = l;
    }
    CHECK(e.active_voices() == 4);          // reuse, never a 5th voice
    CHECK(e.voice_env(0) > env0_at_steal);  // retriggered FROM its level, rising
    CHECK(max_delta < 0.2f);                // no click on the steal
}

TEST_CASE("synth: decay length tracks set_cycle (ratio 1.5x honored)") {
    auto silence_time = [](float cycle_s) {
        SynthEngine e;
        fresh(e);
        e.set_cycle(cycle_s);
        e.trigger(0.5f);
        int n = 0;
        while (n < 48000 * 30) {
            float l = 0.f, r = 0.f;
            e.process(l, r);
            ++n;
            if (e.active_voices() == 0) break;
        }
        return n;
    };
    // decay = 1.5 x cycle (to -60 dB); voice idles at -80 dB = ~1.33 x that
    int fast = silence_time(0.5f);          // decay 0.75 s -> idle ~1.0 s
    int slow = silence_time(2.0f);          // decay 3 s    -> idle ~4 s
    CHECK(fast > static_cast<int>(0.75f * 48000));
    CHECK(fast < static_cast<int>(1.4f * 48000));
    CHECK(slow > static_cast<int>(3.0f * 48000));
    CHECK(slow < static_cast<int>(5.6f * 48000));
    CHECK(slow > fast * 3);
}

TEST_CASE("synth: attack floor 2 ms and decay clamp 50 ms at extreme cycles") {
    SynthEngine e;
    fresh(e);
    e.set_cycle(0.02f);     // attack 2% -> 0.4 ms, floored to 2 ms;
                            // decay 1.5x -> 30 ms, clamped up to 50 ms
    e.trigger(0.5f);
    int to_peak = 0;
    while (e.voice_env(0) < 1.f && to_peak < 4800) {
        float l = 0.f, r = 0.f;
        e.process(l, r);
        ++to_peak;
    }
    CHECK(to_peak >= 80);                   // ~96 samples = 2 ms (ctrl-rate slack)
    CHECK(to_peak <= 200);
    int n = to_peak;
    while (e.active_voices() > 0 && n < 48000) {
        float l = 0.f, r = 0.f;
        e.process(l, r);
        ++n;
    }
    CHECK(n - to_peak > static_cast<int>(0.05f * 48000));         // >= 50 ms decay
    CHECK(n - to_peak < static_cast<int>(0.05f * 48000 * 2.0f));
    float l = 0.f, r = 0.f;                 // STEP silence is EXACT zero
    e.process(l, r);
    CHECK(l == 0.f);
    CHECK(r == 0.f);
}

TEST_CASE("synth: FLOW drone - auto-trigger, sustain 0.7, pitch tracks, demotion") {
    SynthEngine e;
    fresh(e);
    feed(e, 0.25f);                         // 185.0 Hz
    e.set_flow(true);                       // no sustaining voice -> auto-trigger
    auto v = render_l(e, 48000);
    CHECK(e.active_voices() >= 1);
    CHECK(e.sustain_voice() >= 0);
    int n = crossings(v, 4800);             // ~185 * 0.9 = 166
    CHECK(n >= 160);
    CHECK(n <= 172);

    render_l(e, 48000 * 3);                 // ride decay-to-sustain out
    CHECK(e.voice_env(e.sustain_voice()) == doctest::Approx(0.7f).epsilon(0.03));

    feed(e, 0.75f);                         // 523.3 Hz: the drone must follow
    render_l(e, 9600);                      // let the control rate catch up
    auto v2 = render_l(e, 48000);
    int n2 = crossings(v2);
    CHECK(n2 >= 515);
    CHECK(n2 <= 532);

    int old_voice = e.sustain_voice();      // a new fire demotes the old drone
    e.trigger(0.25f);
    CHECK(e.sustain_voice() != old_voice);
    render_l(e, 48000 * 8);                 // demoted voice decays to zero
    CHECK(e.voice_env(old_voice) == 0.f);
    CHECK(e.voice_env(e.sustain_voice()) == doctest::Approx(0.7f).epsilon(0.03));
}

TEST_CASE("synth: entering FLOW mid-run with no sustaining voice auto-triggers") {
    SynthEngine e;
    fresh(e);
    render_l(e, 4800);                      // STEP, no trigger: stays silent
    CHECK(e.active_voices() == 0);
    e.set_flow(true);                       // drone promise
    render_l(e, 4800);
    CHECK(e.active_voices() >= 1);
    CHECK(e.sustain_voice() >= 0);
}

TEST_CASE("synth: MOTION width 0 is dead mono; width 1 separates the channels") {
    SynthEngine e;
    fresh(e);
    e.set_flow(true);                       // steady drone to measure
    feed(e, 0.5f, 0.f, 1.f, 0.f);           // width 0
    float max_diff = 0.f;
    for (int i = 0; i < 48000; ++i) {
        float l = 0.f, r = 0.f;
        e.process(l, r);
        max_diff = std::max(max_diff, std::fabs(l - r));
    }
    CHECK(max_diff == 0.f);                 // identical gains -> bit-equal L/R

    SynthEngine e2;
    fresh(e2);
    e2.set_flow(true);
    feed(e2, 0.5f, 0.f, 1.f, 1.f);          // width 1: voice 0 fans hard left
    float suml = 0.f, sumr = 0.f;
    for (int i = 0; i < 48000; ++i) {
        float l = 0.f, r = 0.f;
        e2.process(l, r);
        suml += l * l;
        sumr += r * r;
    }
    CHECK(std::fabs(suml - sumr) / (suml + sumr + 1e-9f) > 0.2f);
}

TEST_CASE("synth: identical seed + call sequence is bit-identical") {
    auto run = [] {
        SynthEngine e;
        e.set_seed(1234);
        e.init(48000.f);
        e.set_cycle(0.8f);
        e.set_flow(true);
        std::vector<float> out;
        out.reserve(96000);
        for (int i = 0; i < 48000; ++i) {
            float t[LANE_COUNT] = { 0.4f, 0.7f, 0.5f, 0.8f, 0.9f };
            e.set_targets(t, 0.5f);
            if (i == 10000 || i == 20000) e.trigger(0.3f);
            float l = 0.f, r = 0.f;
            e.process(l, r);
            out.push_back(l);
            out.push_back(r);
        }
        return out;
    };
    CHECK(run() == run());
}
```

- [ ] **Step 3: Add to build and run — expect RED**

Add `engine/synth/synth_engine.cpp` to BOTH the `spky_tests` and `render` source lists and `tests/test_synth_engine.cpp` to `spky_tests` in `CMakeLists.txt`, then:
```bash
source env.sh && cmake -S . -B build && cmake --build build 2>&1 | tail -5
```
Expected: build FAILS — `synth/synth_engine.h: No such file or directory`.

- [ ] **Step 4: Write the implementation**

`engine/synth/synth_engine.h`:
```cpp
#pragma once
#include <array>
#include <cstdint>
#include "parts/engine_iface.h"
#include "synth/voice.h"
#include "util/onepole.h"

namespace spky {

// The M2 polyphonic part engine: 4 trigger-driven voices behind IPartEngine.
//
// - Allocation: round-robin over free voices; none free -> steal the OLDEST
//   (by trigger order); the steal retriggers the envelope from its current
//   level (click-free, Voice::trigger).
// - STEP (flow == false): plain AD - notes end, silence is legitimate.
// - FLOW (flow == true): the most recently triggered voice is the SUSTAINING
//   voice - it decays to 0.7 and holds, and its pitch continuously follows
//   the quantized PITCH target. A new fire demotes it (decays to zero) and
//   takes over. Entering FLOW with no sustaining voice auto-triggers one at
//   the current PITCH target (the drone promise) - deferred to the next
//   process() call so the targets are fresh.
// - Targets: TIMBRE (morph + t^2 * DETUNE_MAX detune), FILTER (60 Hz-14 kHz
//   exp), PITCH (latched at trigger, 110*8^p), MOTION (pan fan
//   [-1,+1,-0.5,+0.5] * width + drift ~ width), LEVEL (OnePole-smoothed
//   master gain). All but PITCH act on all voices continuously.
// - Control rate: drift LFOs + envelope coefficients + all voice parameter
//   pushes run once per kCtrlInterval samples (CPU-budget constraint).
class SynthEngine : public IPartEngine {
public:
    static constexpr int   kVoices       = 4;
    static constexpr int   kCtrlInterval = 96;
    static constexpr float kAttackFloorS = 0.002f;
    static constexpr float kDecayMinS    = 0.05f;
    static constexpr float kDecayMaxS    = 20.f;
    static constexpr float kDetuneCeilCt = 35.f;

    void set_seed(uint32_t seed) { _seed = seed; }   // call BEFORE init

    void init(float sample_rate) override;
    void set_targets(const float* t, float tune) override;
    void trigger(float pitch_norm) override;
    void process(float& outL, float& outR) override;
    void set_cycle(float seconds) override;
    void set_flow(bool flow) override;

    // VOICE edit layer (normalized knobs; boot defaults live as raw ratios)
    void set_attack(float n);      // ratio = 0.002 * 250^n  (0.2%..50% of cycle)
    void set_decay(float n);       // ratio = 0.1 * 80^n     (0.1x..8x cycle)
    void set_resonance(float n);
    void set_sub(float n);
    void set_detune(float n);      // DETUNE_MAX = n * 35 ct

    int   active_voices() const;
    float voice_env(int v) const;
    int   sustain_voice() const { return _sustain_voice; }

private:
    void _do_trigger(float pitch_norm);
    void _update_control();

    std::array<Voice, kVoices>    _voices;
    std::array<uint32_t, kVoices> _order {};   // trigger sequence per voice
    uint32_t _seq = 0;
    uint32_t _seed = 0xC0FFEEu;

    float _sr = 48000.f;
    float _targets[LANE_COUNT] = { 0.f, 0.5f, 0.5f, 0.f, 0.8f };
    bool  _flow = false;
    int   _sustain_voice = -1;     // -1 = none
    bool  _auto_pending = false;   // drone promise, fires in process()
    int   _next_rr = 0;
    int   _ctrl_ctr = 0;

    float _cycle_s = 1.f;
    float _attack_ratio = 0.02f;   // boot: 2 % of cycle (spec)
    float _decay_ratio  = 1.5f;    // boot: 1.5 x cycle (spec)
    float _resonance = 0.15f;      // boot (spec)
    float _sub_level = 0.3f;       // boot (spec)
    float _detune_max_ct = 18.f;   // boot DETUNE_MAX (spec)

    OnePole _level;                // smoothed master gain (LEVEL target)
};

} // namespace spky
```

`engine/synth/synth_engine.cpp`:
```cpp
#include "synth/synth_engine.h"
#include <cmath>
#include "util/math.h"

using namespace spky;

namespace {
// 4 voices at full level + sub must stay inside the part's +/-1.5 headroom.
constexpr float kVoiceGain = 0.22f;
constexpr float kPanFan[SynthEngine::kVoices] = { -1.f, 1.f, -0.5f, 0.5f };

// Pitch contract (identical numbers to TestToneEngine): 0..1 = 36 semitones.
// std::pow is fine here - trigger/control rate, never per sample.
inline float pitch_to_hz(float p) { return 110.f * std::pow(8.f, clampf(p, 0.f, 1.f)); }

// FILTER target: exponential 60 Hz .. 14 kHz (spec).
inline float filter_hz(float n) {
    return 60.f * std::pow(14000.f / 60.f, clampf(n, 0.f, 1.f));
}
} // namespace

void SynthEngine::init(float sample_rate) {
    _sr = sample_rate;
    for (int v = 0; v < kVoices; ++v) {
        _voices[v].init(sample_rate, _seed + 0x9e3779b9u * static_cast<uint32_t>(v + 1));
        _order[v] = 0;
    }
    _seq = 0;
    _sustain_voice = -1;
    _auto_pending = false;
    _next_rr = 0;
    _ctrl_ctr = 0;                 // first process() runs a control tick
    _level.init(sample_rate, 0.01f);
    _level.reset(_targets[LANE_LEVEL]);
    _update_control();
}

void SynthEngine::set_targets(const float* t, float /*tune*/) {
    // tune is already summed into the quantized PITCH target upstream (Part).
    for (int i = 0; i < LANE_COUNT; ++i) _targets[i] = t[i];
}

void SynthEngine::set_cycle(float seconds) {
    _cycle_s = clampf(seconds, 0.01f, 120.f);   // applied at the next ctrl tick
}

void SynthEngine::set_flow(bool flow) {
    if (flow == _flow) return;
    _flow = flow;
    if (!flow) {
        // STEP: no sustaining voice; a holding drone is released (decays out)
        if (_sustain_voice >= 0) _voices[_sustain_voice].set_sustaining(false);
        _sustain_voice = -1;
        _auto_pending = false;
    } else if (_sustain_voice < 0) {
        _auto_pending = true;      // drone promise; fires in process()
    }
}

void SynthEngine::trigger(float pitch_norm) { _do_trigger(pitch_norm); }

void SynthEngine::_do_trigger(float pitch_norm) {
    int pick = -1;
    for (int i = 0; i < kVoices; ++i) {              // round-robin over free voices
        int v = (_next_rr + i) % kVoices;
        if (!_voices[v].active()) { pick = v; break; }
    }
    if (pick < 0) {                                   // none free: steal the oldest
        uint32_t oldest = _order[0];
        pick = 0;
        for (int v = 1; v < kVoices; ++v)
            if (_order[v] < oldest) { oldest = _order[v]; pick = v; }
    }
    _next_rr = (pick + 1) % kVoices;
    _order[pick] = ++_seq;

    if (_flow) {
        if (_sustain_voice >= 0 && _sustain_voice != pick)
            _voices[_sustain_voice].set_sustaining(false);   // demote: decays out
        _sustain_voice = pick;
        _voices[pick].set_sustaining(true);
    }
    _voices[pick].trigger(pitch_to_hz(pitch_norm));   // pitch LATCHED here
}

void SynthEngine::_update_control() {
    const float attack_s = clampf(_attack_ratio * _cycle_s, kAttackFloorS, kDecayMaxS);
    const float decay_s  = clampf(_decay_ratio  * _cycle_s, kDecayMinS,  kDecayMaxS);

    const float timbre = _targets[LANE_SOURCE];       // pad 1 = TIMBRE
    const float det_ct = timbre * timbre * _detune_max_ct;   // t^2 law (spec)
    const float cutoff = filter_hz(_targets[LANE_SIZE]);     // pad 2 = FILTER
    const float width  = clampf(_targets[LANE_MOTION], 0.f, 1.f);
    const float dt_s   = kCtrlInterval / _sr;

    for (int v = 0; v < kVoices; ++v) {
        Voice& vc = _voices[v];
        vc.set_env_times(attack_s, decay_s);
        vc.set_morph(timbre);
        vc.set_detune_cents(det_ct);
        vc.set_sub_level(_sub_level);
        vc.set_cutoff_hz(cutoff);
        vc.set_resonance(_resonance);
        vc.set_pan(kPanFan[v] * width);
        vc.set_drift_amount(width);
        vc.update_control(dt_s);
    }

    // only the FLOW sustaining voice tracks the target after its trigger
    if (_flow && _sustain_voice >= 0)
        _voices[_sustain_voice].set_pitch_hz(pitch_to_hz(_targets[LANE_PITCH]));
}

void SynthEngine::process(float& outL, float& outR) {
    if (_auto_pending) {                     // targets are fresh by now (Part
        _auto_pending = false;               // calls set_targets every sample)
        _do_trigger(_targets[LANE_PITCH]);
    }
    if (--_ctrl_ctr <= 0) {
        _ctrl_ctr = kCtrlInterval;
        _update_control();
    }
    const float gain = _level.process(_targets[LANE_LEVEL]) * kVoiceGain;
    float l = 0.f, r = 0.f;
    for (auto& v : _voices) v.process(l, r);
    outL = l * gain;
    outR = r * gain;
}

void SynthEngine::set_attack(float n) {
    _attack_ratio = 0.002f * std::pow(250.f, clampf(n, 0.f, 1.f));
}

void SynthEngine::set_decay(float n) {
    _decay_ratio = 0.1f * std::pow(80.f, clampf(n, 0.f, 1.f));
}

void SynthEngine::set_resonance(float n) { _resonance = clampf(n, 0.f, 1.f); }
void SynthEngine::set_sub(float n)       { _sub_level = clampf(n, 0.f, 1.f); }

void SynthEngine::set_detune(float n) {
    _detune_max_ct = clampf(n, 0.f, 1.f) * kDetuneCeilCt;
}

int SynthEngine::active_voices() const {
    int n = 0;
    for (const auto& v : _voices)
        if (v.active()) ++n;
    return n;
}

float SynthEngine::voice_env(int v) const {
    if (v < 0 || v >= kVoices) return 0.f;
    return _voices[v].env_value();
}
```

- [ ] **Step 5: Build and run — expect GREEN**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```
Expected: PASS — including all pre-existing tests (`engine_iface.h` only gained an enum and two defaulted virtuals; nothing existing overrides or calls them yet).

- [ ] **Step 6: Commit**

```bash
git add engine/parts/engine_iface.h engine/synth/synth_engine.h engine/synth/synth_engine.cpp \
        tests/test_synth_engine.cpp CMakeLists.txt
git commit -m "feat(engine): SynthEngine - 4-voice allocation, FLOW drone, target mapping

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 6: `Part` — engine selection, click-free switch, flow/cycle forwarding, boot default SYNTH

**Files:**
- Modify: `engine/parts/part.h`, `engine/parts/part.cpp`
- Modify: `engine/instrument.h` (ONE line: route `set_step` through `Part` so the engine learns the STEP/FLOW state)
- Test: `tests/test_part.cpp` (append cases; add `#include <algorithm>` to its include block)

**Interfaces:**
- Consumes: `spky::SynthEngine` (Task 5), `EngineId`/`set_cycle`/`set_flow` (Task 5), `spky::SoftSwitch` (`fx/fx_util.h`, M1.6), existing `Part` internals.
- Produces (Tasks 7–8 depend on these exact names):
  - `void Part::set_engine(EngineId e)` — click-free: 4 ms SoftSwitch fade-out → swap → 4 ms fade-in; re-forwards flow + cycle to the incoming engine at the swap.
  - `EngineId Part::engine_id() const` — the ACTIVE engine (updates when the swap completes, ~192 samples after `set_engine`).
  - `void Part::set_step(bool on, int steps)` — forwards to `mod().set_step` AND `engine->set_flow(!on)`.
  - `void Part::trigger_manual()` — PLAY-tap path: raises the gate and calls `engine->trigger(target_value(LANE_PITCH))` (current quantized pitch).
  - Voice-parameter forwards (reach `_synth` directly, so edits stick even while the test tone is active): `set_voice_attack(float)`, `set_voice_decay(float)`, `set_voice_resonance(float)`, `set_voice_sub(float)`, `set_voice_detune(float)`.
  - Introspection: `int active_voices() const` (0 when the active engine is the test tone), `float voice_env(int v) const`.
  - Boot default: `ENGINE_SYNTH`, FLOW forwarded as `true` (lanes boot in FLOW) — a fresh `Part` hums (drone promise).
  - Cycle forwarding: `Part::process` compares `mod().master_hz()` to the last forwarded value and calls `engine->set_cycle(1/hz)` only on change.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_part.cpp` (and add `#include <algorithm>` next to the existing `#include <cmath>`):

```cpp
TEST_CASE("part: boots on the synth engine and hums in FLOW (drone promise)") {
    Part p;
    p.init(48000.f, 5);
    CHECK(p.engine_id() == ENGINE_SYNTH);
    float energy = 0.f, l, r;
    for (int i = 0; i < 48000; ++i) {
        p.process(l, r);
        energy += l * l;
    }
    CHECK(p.active_voices() >= 1);
    CHECK(energy > 1e-3f);
}

TEST_CASE("part: FLOW at probability 0 never goes silent; STEP decays to silence") {
    Part p;
    p.init(48000.f, 5);
    p.mod().set_probability(0.f);
    float l, r;
    for (int i = 0; i < 48000 * 4; ++i) p.process(l, r);
    float energy = 0.f;
    for (int i = 0; i < 48000; ++i) {
        p.process(l, r);
        energy += l * l;
    }
    CHECK(energy > 1e-4f);                  // the drone holds at probability 0

    Part q;
    q.init(48000.f, 5);
    q.mod().set_probability(0.f);
    q.set_step(true, 8);                    // STEP: the boot drone is released
    for (int i = 0; i < 48000 * 10; ++i) q.process(l, r);
    float tail = 0.f;
    for (int i = 0; i < 48000; ++i) {
        q.process(l, r);
        tail += l * l;
    }
    CHECK(tail == 0.f);                     // decays to EXACT silence and stays
}

TEST_CASE("part: manual trigger fires at the current pitch and raises the gate") {
    Part p;
    p.init(48000.f, 5);
    p.set_step(true, 8);
    p.mod().set_probability(0.f);
    float l, r;
    for (int i = 0; i < 48000 * 8; ++i) p.process(l, r);
    CHECK(p.active_voices() == 0);          // silent before the tap
    p.trigger_manual();
    CHECK(p.gate());
    p.process(l, r);
    CHECK(p.active_voices() == 1);
}

TEST_CASE("part: decay length follows the master cycle (set_cycle forwarding)") {
    auto tail_samples = [](float rate_norm) {
        Part p;
        p.init(48000.f, 5);
        p.set_step(true, 8);
        p.mod().set_probability(0.f);
        p.mod().set_sync_mode(SyncMode::Free);
        p.mod().set_rate(rate_norm);
        float l, r;
        // settle (no boot drone here: set_step ran before the first process()
        // call, which cancels the pending FLOW auto-trigger)
        for (int i = 0; i < 48000 * 3; ++i) p.process(l, r);
        p.trigger_manual();
        int n = 0;
        while (p.active_voices() > 0 && n < 48000 * 10) {
            p.process(l, r);
            ++n;
        }
        return n;
    };
    int slow = tail_samples(0.6f);   // ~1.61 Hz -> cycle 0.62 s -> decay ~0.93 s
    int fast = tail_samples(0.8f);   // ~6.9 Hz  -> cycle 0.14 s -> decay ~0.22 s
    CHECK(slow > fast * 2);          // longer cycle => audibly longer notes
}

TEST_CASE("part: engine switch test tone <-> synth is click-free") {
    Part p;
    p.init(48000.f, 5);
    float prev_l = 0.f, max_delta = 0.f;
    float l, r;
    for (int i = 0; i < 48000; ++i) {
        if (i == 12000) p.set_engine(ENGINE_TEST_TONE);
        if (i == 30000) p.set_engine(ENGINE_SYNTH);
        p.process(l, r);
        if (i > 0) max_delta = std::max(max_delta, std::fabs(l - prev_l));
        prev_l = l;
    }
    // a hard swap would step by the level difference (~0.3-0.5); the 4 ms
    // Hann fade keeps sample-to-sample deltas at waveform scale.
    CHECK(max_delta < 0.15f);
    CHECK(p.engine_id() == ENGINE_SYNTH);   // second switch completed
}

TEST_CASE("part: the test tone engine reports zero active voices") {
    Part p;
    p.init(48000.f, 5);
    p.set_engine(ENGINE_TEST_TONE);
    float l, r;
    for (int i = 0; i < 1000; ++i) p.process(l, r);   // ride out the 4 ms fades
    CHECK(p.engine_id() == ENGINE_TEST_TONE);
    CHECK(p.active_voices() == 0);
}
```

- [ ] **Step 2: Run to verify they fail**

```bash
source env.sh && cmake --build build 2>&1 | tail -5
```
Expected: FAIL — `engine_id` / `set_engine` / `active_voices` are not members of `spky::Part`.

- [ ] **Step 3: Implement — `engine/parts/part.h`**

Three edits:

1. Add two includes after `#include "parts/test_tone_engine.h"`:
```cpp
#include "synth/synth_engine.h"
#include "fx/fx_util.h"
```

2. In the public section, after the existing `set_fx_target_*` block, add:
```cpp
    // --- engine selection (M2). Boot default: ENGINE_SYNTH. ---
    // Click-free: 4 ms SoftSwitch fade-out -> swap -> 4 ms fade-in; the swap
    // and state re-forwarding happen inside process() at the idle point.
    void set_engine(EngineId e);
    EngineId engine_id() const { return _engine_id; }

    // STEP/FLOW reaches both the lanes and the engine (drone rule)
    void set_step(bool on, int steps);

    // PLAY tap (M6 wires the gesture; the engine sees an ordinary trigger)
    void trigger_manual();

    // VOICE edit layer - forwarded to the synth engine directly, so edits
    // stick even while the test tone is the active engine
    void set_voice_attack(float n)    { _synth.set_attack(n); }
    void set_voice_decay(float n)     { _synth.set_decay(n); }
    void set_voice_resonance(float n) { _synth.set_resonance(n); }
    void set_voice_sub(float n)       { _synth.set_sub(n); }
    void set_voice_detune(float n)    { _synth.set_detune(n); }

    int active_voices() const {
        return _engine_id == ENGINE_SYNTH ? _synth.active_voices() : 0;
    }
    float voice_env(int v) const {
        return _engine_id == ENGINE_SYNTH ? _synth.voice_env(v) : 0.f;
    }
```

3. In the private section, after the `IPartEngine* _engine = nullptr;` line, add:
```cpp
    SynthEngine    _synth;
    SoftSwitch     _engine_fade;
    EngineId       _engine_id = ENGINE_SYNTH;
    EngineId       _pending_engine = ENGINE_SYNTH;
    bool           _switching = false;
    bool           _step_on = false;
    float          _last_master_hz = -1.f;

    IPartEngine* _engine_for(EngineId e) {
        return e == ENGINE_SYNTH ? static_cast<IPartEngine*>(&_synth)
                                 : static_cast<IPartEngine*>(&_tone);
    }
```

- [ ] **Step 4: Implement — `engine/parts/part.cpp`**

Replace `Part::init` with:
```cpp
void Part::init(float sample_rate, uint32_t seed_base,
                float* echo_l, float* echo_r) {
    _sr = sample_rate;
    _mod.init(sample_rate, seed_base);
    _tone.init(sample_rate);
    _synth.set_seed(seed_base ^ 0x5eedC0DEu);   // per-part drift decorrelation
    _synth.init(sample_rate);
    _engine_id = ENGINE_SYNTH;                  // boot default (M2 spec)
    _pending_engine = _engine_id;
    _switching = false;
    _engine = _engine_for(_engine_id);
    _engine_fade.init(sample_rate);
    _engine_fade.set_on(true, true);            // boot: engine fully on
    _step_on = false;
    _engine->set_flow(true);                    // lanes boot in FLOW -> drone
    _last_master_hz = -1.f;                     // force a cycle forward on
                                                // the first process()
    _fx.init(sample_rate, echo_l, echo_r);
    _gate_len = static_cast<int>(sample_rate * 0.005f);
    _gate_ctr = 0;
    _quant.init(sample_rate);                   // boots Dorian / SCALE / root 0
    _pitch_q = _quant.process(pitch_pre_quant());
}
```

Add these three functions (after `fx_target_value`):
```cpp
void Part::set_engine(EngineId e) {
    if (_switching ? e == _pending_engine : e == _engine_id) return;
    _pending_engine = e;
    _switching = true;
    _engine_fade.set_on(false);   // fade out; process() swaps at the idle point
}

void Part::set_step(bool on, int steps) {
    _step_on = on;
    _mod.set_step(on, steps);
    _engine->set_flow(!on);
}

void Part::trigger_manual() {
    _gate_ctr = _gate_len;
    _engine->trigger(target_value(LANE_PITCH));   // current quantized pitch
}
```

Replace `Part::process` (4-arg) with:
```cpp
void Part::process(float& outL, float& outR, float& sendL, float& sendR) {
    _mod.process();

    // click-free engine switch: fade out (4 ms) -> swap -> fade in (4 ms).
    // At hold the multiplier is exactly 1.0, so unswitched runs stay
    // bit-identical (M1.6 bypass invariant).
    const float fade = _engine_fade.process();
    if (_switching && _engine_fade.is_idle()) {
        _engine_id = _pending_engine;
        _engine = _engine_for(_engine_id);
        _engine->set_flow(!_step_on);                          // re-sync state
        if (_last_master_hz > 0.f) _engine->set_cycle(1.f / _last_master_hz);
        _switching = false;
        _engine_fade.set_on(true);
    }

    // forward the master-lane cycle length on change, not per sample
    const float hz = _mod.master_hz();
    if (hz != _last_master_hz && hz > 0.f) {
        _last_master_hz = hz;
        _engine->set_cycle(1.f / hz);
    }

    if (_mod.lane_fired(LANE_PITCH)) _gate_ctr = _gate_len;
    if (_gate_ctr > 0) --_gate_ctr;

    float targets[LANE_COUNT];
    for (int i = 0; i < LANE_COUNT; ++i) targets[i] = target_raw(i);
    targets[LANE_PITCH] = _quant.process(pitch_pre_quant());
    _pitch_q = targets[LANE_PITCH];

    _engine->set_targets(targets, _tune);
    if (_mod.lane_fired(LANE_PITCH)) _engine->trigger(targets[LANE_PITCH]);
    _engine->process(outL, outR);
    outL *= fade;
    outR *= fade;

    float fxv[FXT_COUNT];
    for (int i = 0; i < FXT_COUNT; ++i) fxv[i] = fx_target_value(i);
    _fx.process(outL, outR, sendL, sendR, fxv);
}
```

- [ ] **Step 5: Route `Instrument::set_step` through `Part`**

In `engine/instrument.h`, change the one line
```cpp
    void set_step(int p, bool on, int steps) { _parts[p].mod().set_step(on, steps); }
```
to
```cpp
    void set_step(int p, bool on, int steps) { _parts[p].set_step(on, steps); }
```
(so the engine's flow state follows STEP/FLOW switches made through the public API — same behavior for the lanes as before).

- [ ] **Step 6: Build and run — expect GREEN, including every pre-existing test**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```
Expected: PASS. Notes on the pre-existing suite (verified engine-agnostic by inspection before this plan was written — see Locked decision 10):
- `test_part.cpp` / `test_instrument.cpp` assert lane/target routing, quantization grids, FX-target math and comparative audibility — none depends on the test-tone waveform. The boot-drone actually strengthens "boot reverb send is audible".
- The M1.6 bit-exact bypass test compares two identically configured instruments — both now boot the synth, so it still passes bit-exact (the fade multiplier is exactly 1.0 at hold).
- If `instrument: init and render a block without NaNs` ever exceeds ±1.5, lower `kVoiceGain` in `synth_engine.cpp` (headroom analysis says 0.22 keeps the worst case ≤ ~1.25; do not touch the test bound).

- [ ] **Step 7: Commit**

```bash
git add engine/parts/part.h engine/parts/part.cpp engine/instrument.h tests/test_part.cpp
git commit -m "feat(engine): Part engine selection - boot-default synth, click-free switch, flow/cycle forwarding

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 7: `Instrument` — public synth-voice API

**Files:**
- Modify: `engine/instrument.h` (inline forwards only — no `.cpp` change)
- Test: `tests/test_instrument.cpp` (append cases; add `#include <algorithm>` to its include block)

**Interfaces:**
- Consumes: the Task 6 `Part` API.
- Produces (Task 8's scenario dispatcher uses these exact signatures — they are the spec's "Instrument API" verbatim, plus `voice_env`/`engine_id` for the CSV and tests):
```cpp
void set_engine(int p, EngineId e);          // boot default: ENGINE_SYNTH
void set_voice_attack(int p, float n);       // ratio, exponential map
void set_voice_decay(int p, float n);        // ratio, exponential map
void set_voice_resonance(int p, float n);
void set_voice_sub(int p, float n);
void set_voice_detune(int p, float n);       // DETUNE_MAX, 0..~35 ct
void trigger_manual(int p);                  // PLAY tap path
int  active_voices(int p) const;             // for CSV / LEDs
float voice_env(int p, int v) const;         // per-voice envelope level
EngineId engine_id(int p) const;
```

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_instrument.cpp` (and add `#include <algorithm>` next to the existing `#include <cmath>`):

```cpp
TEST_CASE("instrument: boots both parts on the synth engine with an audible drone") {
    Instrument inst;
    inst.init(48000.f);
    CHECK(inst.engine_id(PART_A) == ENGINE_SYNTH);
    CHECK(inst.engine_id(PART_B) == ENGINE_SYNTH);
    float l, r, energy = 0.f;
    for (int i = 0; i < 48000; ++i) {
        inst.process(nullptr, nullptr, &l, &r, 1);
        energy += l * l;
    }
    CHECK(inst.active_voices(PART_A) >= 1);
    CHECK(inst.active_voices(PART_B) >= 1);
    CHECK(energy > 1e-3f);
}

TEST_CASE("instrument: voice setters and manual trigger reach the part") {
    Instrument inst;
    inst.init(48000.f);
    inst.set_voice_decay(PART_A, 0.f);      // shortest decay ratio (0.1x cycle)
    inst.set_step(PART_A, true, 8);
    inst.set_probability(PART_A, 0.f);
    float l, r;
    for (int i = 0; i < 48000 * 3; ++i) inst.process(nullptr, nullptr, &l, &r, 1);
    CHECK(inst.active_voices(PART_A) == 0);
    inst.trigger_manual(PART_A);
    inst.process(nullptr, nullptr, &l, &r, 1);
    CHECK(inst.active_voices(PART_A) == 1);
    float peak = 0.f;                        // some voice's envelope is running
    for (int v = 0; v < 4; ++v) peak = std::max(peak, inst.voice_env(PART_A, v));
    CHECK(peak > 0.f);
}

TEST_CASE("instrument: set_engine switches to the test tone and back") {
    Instrument inst;
    inst.init(48000.f);
    inst.set_engine(PART_A, ENGINE_TEST_TONE);
    float l, r;
    for (int i = 0; i < 1000; ++i) inst.process(nullptr, nullptr, &l, &r, 1);
    CHECK(inst.engine_id(PART_A) == ENGINE_TEST_TONE);
    CHECK(inst.active_voices(PART_A) == 0);
    inst.set_engine(PART_A, ENGINE_SYNTH);
    for (int i = 0; i < 48000; ++i) inst.process(nullptr, nullptr, &l, &r, 1);
    CHECK(inst.engine_id(PART_A) == ENGINE_SYNTH);
    CHECK(inst.active_voices(PART_A) >= 1);   // the drone resumes
}
```

- [ ] **Step 2: Run to verify they fail**

```bash
source env.sh && cmake --build build 2>&1 | tail -5
```
Expected: FAIL — `engine_id` / `set_engine` / `active_voices` are not members of `spky::Instrument`.

- [ ] **Step 3: Implement**

In `engine/instrument.h`, after the `set_reverb_*` / `fx_target_value` block, add:

```cpp
    // --- M2 synth voice API (spec "Instrument API") ---
    void set_engine(int p, EngineId e)       { _parts[p].set_engine(e); }
    void set_voice_attack(int p, float n)    { _parts[p].set_voice_attack(n); }
    void set_voice_decay(int p, float n)     { _parts[p].set_voice_decay(n); }
    void set_voice_resonance(int p, float n) { _parts[p].set_voice_resonance(n); }
    void set_voice_sub(int p, float n)       { _parts[p].set_voice_sub(n); }
    void set_voice_detune(int p, float n)    { _parts[p].set_voice_detune(n); }
    void trigger_manual(int p)               { _parts[p].trigger_manual(); }
    int  active_voices(int p) const          { return _parts[p].active_voices(); }
    float voice_env(int p, int v) const      { return _parts[p].voice_env(v); }
    EngineId engine_id(int p) const          { return _parts[p].engine_id(); }
```

- [ ] **Step 4: Build and run — expect GREEN**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add engine/instrument.h tests/test_instrument.cpp
git commit -m "feat(engine): Instrument synth-voice API - engine select, voice params, manual trigger

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 8: Render host — scenario actions, CSV voice columns, explicit test-tone pins

**Files:**
- Modify: `host/render/scenario.cpp` (7 new actions)
- Modify: `host/render/main.cpp` (CSV columns)
- Modify: `host/render/scenarios/demo_step_melody.json`, `dorian_melody.json`, `dorian_vs_drift.json`, `melody_then_drift.json`, `pentatonic_melody.json`, `dub_delay.json`, `ambient_wash.json` (explicit `ENGINE_TEST_TONE`)
- Test: `tests/test_scenario.cpp` (append a case)

**Interfaces:**
- Consumes: the Task 7 Instrument API.
- Produces: scenario actions 1:1 with the API — `set_engine` (svalue `"synth"` / `"test_tone"`), `set_voice_attack`, `set_voice_decay`, `set_voice_resonance`, `set_voice_sub`, `set_voice_detune` (part + value), `trigger_manual` (part only). `mods.csv` gains per part: `voices` (active count 0–4, integer) and `v0..v3` (per-voice envelope levels) — the overlapping-voices plot. Existing scenarios keep their M1/M1.6 sound by selecting the test tone explicitly.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_scenario.cpp`:

```cpp
TEST_CASE("scenario: M2 synth actions reach the instrument") {
    Instrument inst;
    inst.init(48000.f);
    inst.set_voice_decay(0, 0.f);            // shortest decay: fast test
    inst.set_step(0, true, 8);
    inst.set_probability(0, 0.f);
    float l = 0.f, r = 0.f;
    for (int i = 0; i < 48000 * 3; ++i) inst.process(nullptr, nullptr, &l, &r, 1);
    CHECK(inst.active_voices(0) == 0);       // boot drone released and gone

    Event trig;                              // trigger_manual is observable
    trig.action = "trigger_manual";
    trig.part = 0;
    apply_event(inst, trig);
    inst.process(nullptr, nullptr, &l, &r, 1);
    CHECK(inst.active_voices(0) == 1);

    Event eng;                               // set_engine is observable
    eng.action = "set_engine";
    eng.part = 0;
    eng.svalue = "test_tone";
    apply_event(inst, eng);
    for (int i = 0; i < 1000; ++i) inst.process(nullptr, nullptr, &l, &r, 1);
    CHECK(inst.engine_id(0) == ENGINE_TEST_TONE);
    CHECK(inst.active_voices(0) == 0);

    // the five voice-parameter actions dispatch without crashing (their
    // audible effect is pinned by the engine/env unit tests)
    const char* voice_actions[] = { "set_voice_attack", "set_voice_decay",
                                    "set_voice_resonance", "set_voice_sub",
                                    "set_voice_detune" };
    for (const char* a : voice_actions) {
        Event e;
        e.action = a;
        e.part = 0;
        e.value = 0.5f;
        apply_event(inst, e);
    }
    inst.process(nullptr, nullptr, &l, &r, 1);
    CHECK(l == l);                           // not NaN
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
source env.sh && cmake --build build && ctest --test-dir build --output-on-failure 2>&1 | tail -8
```
Expected: FAIL — `apply_event` ignores the unknown `trigger_manual` action (forward-compatible by design), so `active_voices(0) == 1` fails.

- [ ] **Step 3: Implement the scenario actions**

`host/render/scenario.cpp` — add one parse helper next to `parse_grit_mode` (M1.6):

```cpp
static EngineId parse_engine(const std::string& s) {
    return s == "test_tone" ? ENGINE_TEST_TONE : ENGINE_SYNTH;
}
```

In `apply_event`, after the M1.6 `set_reverb_shimmer` line and before the closing "unknown actions" comment, add:

```cpp
    else if (a == "set_engine")          inst.set_engine(e.part, parse_engine(e.svalue));
    else if (a == "set_voice_attack")    inst.set_voice_attack(e.part, e.value);
    else if (a == "set_voice_decay")     inst.set_voice_decay(e.part, e.value);
    else if (a == "set_voice_resonance") inst.set_voice_resonance(e.part, e.value);
    else if (a == "set_voice_sub")       inst.set_voice_sub(e.part, e.value);
    else if (a == "set_voice_detune")    inst.set_voice_detune(e.part, e.value);
    else if (a == "trigger_manual")      inst.trigger_manual(e.part);
```

- [ ] **Step 4: Add the CSV voice columns**

`host/render/main.cpp` — two changes to the M1.6 end-state file:

1. Replace the CSV header `fprintf` with:
```cpp
    if (csv) {
        std::fprintf(csv, "t,"
            "a_src,a_size,a_pitch,a_motion,a_level,a_pcv,a_gate,"
            "a_fx0,a_fx1,a_fx2,a_fx3,a_fx4,a_voices,a_v0,a_v1,a_v2,a_v3,"
            "b_src,b_size,b_pitch,b_motion,b_level,b_pcv,b_gate,"
            "b_fx0,b_fx1,b_fx2,b_fx3,b_fx4,b_voices,b_v0,b_v1,b_v2,b_v3\n");
    }
```

2. In the CSV row loop (inside the existing `for (int p = 0; ...)`), directly after the M1.6 `fx_target_value` loop, add:
```cpp
                std::fprintf(csv, ",%d", inst.active_voices(p));
                for (int v = 0; v < 4; ++v)
                    std::fprintf(csv, ",%.4f", inst.voice_env(p, v));
```

- [ ] **Step 5: Pin the existing scenarios to the test tone**

The 7 existing scenario JSONs were sound-designed against the M1 test tone; the boot default is now the synth. Insert these two lines as the FIRST entries of the `"init"` array in EACH of `host/render/scenarios/demo_step_melody.json`, `dorian_melody.json`, `dorian_vs_drift.json`, `melody_then_drift.json`, `pentatonic_melody.json`, `dub_delay.json`, `ambient_wash.json`:

```json
    {"action":"set_engine","part":0,"value":"test_tone"},
    {"action":"set_engine","part":1,"value":"test_tone"},
```

(Same two lines, verbatim, in all seven files. Unknown-file note: `dub_delay.json` / `ambient_wash.json` come from M1.6 Task 10 — see the M1.6 end-state assumption at the top of this plan.)

- [ ] **Step 6: Build, run tests, spot-check a render**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```
Expected: PASS.

```bash
./build/render.exe host/render/scenarios/dorian_melody.json /tmp/m2t.wav /tmp/m2t.csv
head -2 /tmp/m2t.csv
```
Expected: header shows 35 columns (`t` + 17 per part); `a_voices` and `b_voices` are `0` from `t ≈ 0.005` on (test tone selected explicitly). The first 3–4 CSV rows MAY show `1`: the boot drone auto-triggers on the very first sample, ~192 samples before the 4 ms `set_engine` crossfade completes — that aborted voice is inaudible (it dies inside the fade) and expected.

- [ ] **Step 7: Commit**

```bash
git add host/render/scenario.cpp host/render/main.cpp tests/test_scenario.cpp \
        host/render/scenarios/demo_step_melody.json host/render/scenarios/dorian_melody.json \
        host/render/scenarios/dorian_vs_drift.json host/render/scenarios/melody_then_drift.json \
        host/render/scenarios/pentatonic_melody.json host/render/scenarios/dub_delay.json \
        host/render/scenarios/ambient_wash.json
git commit -m "feat(host): synth scenario actions, voices/v0..v3 csv columns, explicit test-tone pins

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 9: Demo scenarios, acceptance renders, docs

This task is the M2 spec's acceptance-criteria gate.

**Files:**
- Create: `host/render/scenarios/overlapping_voices.json`, `host/render/scenarios/flow_drone.json`
- Modify: `docs/roadmap.md`, `README.md`

**Interfaces:**
- Consumes: everything above. No new symbols.

- [ ] **Step 1: Create `host/render/scenarios/overlapping_voices.json`**

The master spec's M2 acceptance demo: STEP Dorian melody (Dorian is the boot-default scale), probability ~0.6, long decays overlapping into pads. Part A uses the boot-default synth engine; part B is muted for clarity.

```json
{
  "sample_rate": 48000,
  "bpm": 84,
  "duration_s": 48,
  "init": [
    {"_comment":"PART A - synth (boot default). STEP Dorian melody at probability 0.6; DECAY ratio pushed long so notes overlap into pads (the spec's 'evolving, quantized melodic texture with overlapping voices')."},
    {"action":"set_sync_mode","part":0,"value":"sync"},
    {"action":"set_rate","part":0,"value":0.5},
    {"action":"set_step","part":0,"flag":true,"ivalue":8},
    {"action":"set_shape","part":0,"value":0.55},
    {"action":"set_range","part":0,"value":0.7},
    {"action":"set_smooth","part":0,"value":0.1},
    {"action":"set_probability","part":0,"value":0.6},
    {"action":"set_depth","part":0,"value":1.0},
    {"action":"set_target_active","part":0,"slot":2,"flag":true},
    {"action":"set_target_base","part":0,"slot":2,"value":0.45},
    {"action":"set_target_active","part":0,"slot":4,"flag":true},
    {"action":"set_target_base","part":0,"slot":4,"value":0.75},

    {"_comment":"TIMBRE breathes gently on its own lane; FILTER sits mid-open."},
    {"action":"set_target_active","part":0,"slot":0,"flag":true},
    {"action":"set_target_base","part":0,"slot":0,"value":0.35},
    {"action":"set_target_depth","part":0,"slot":0,"value":0.4},
    {"action":"set_target_base","part":0,"slot":1,"value":0.55},
    {"action":"set_target_active","part":0,"slot":3,"flag":true},
    {"action":"set_target_base","part":0,"slot":3,"value":0.5},

    {"_comment":"Long decay: ratio knob 0.75 -> ~2.7x cycle (~3.8 s at this rate). A touch of shared room."},
    {"action":"set_voice_decay","part":0,"value":0.75},
    {"action":"set_fx_target_base","part":0,"slot":3,"value":0.3},

    {"_comment":"PART B muted for clarity."},
    {"action":"set_target_active","part":1,"slot":4,"flag":false},
    {"action":"set_target_base","part":1,"slot":4,"value":0.0}
  ],
  "events": [
    {"t":24.0,"action":"set_probability","part":0,"value":0.85},
    {"_comment":"24s: denser firing - deeper voice overlap."},
    {"t":36.0,"action":"set_evolve","part":0,"value":0.3},
    {"_comment":"36s: the melody starts wandering within Dorian."}
  ]
}
```

- [ ] **Step 2: Create `host/render/scenarios/flow_drone.json`**

FLOW drone with breathing TIMBRE/FILTER; a mid-section at probability 0 proves the drone promise inside the render itself; probability then reopens for swells.

```json
{
  "sample_rate": 48000,
  "bpm": 60,
  "duration_s": 60,
  "init": [
    {"_comment":"PART A - FLOW (no set_step): the part hums from t=0 (drone promise: FLOW auto-triggers a sustaining voice at boot). Slow free rate ~0.26 Hz -> a new sustaining voice every ~3.9 s layers over the decaying previous one."},
    {"action":"set_sync_mode","part":0,"value":"free"},
    {"action":"set_rate","part":0,"value":0.35},
    {"action":"set_probability","part":0,"value":1.0},
    {"action":"set_smooth","part":0,"value":0.5},
    {"action":"set_range","part":0,"value":0.6},
    {"action":"set_depth","part":0,"value":1.0},

    {"_comment":"Breathing TIMBRE + FILTER + MOTION; PITCH target modulated too - the sustaining voice glides after it through the quantizer."},
    {"action":"set_target_active","part":0,"slot":0,"flag":true},
    {"action":"set_target_base","part":0,"slot":0,"value":0.4},
    {"action":"set_target_depth","part":0,"slot":0,"value":0.6},
    {"action":"set_target_active","part":0,"slot":1,"flag":true},
    {"action":"set_target_base","part":0,"slot":1,"value":0.5},
    {"action":"set_target_depth","part":0,"slot":1,"value":0.5},
    {"action":"set_target_active","part":0,"slot":2,"flag":true},
    {"action":"set_target_base","part":0,"slot":2,"value":0.35},
    {"action":"set_target_depth","part":0,"slot":2,"value":0.5},
    {"action":"set_target_active","part":0,"slot":3,"flag":true},
    {"action":"set_target_base","part":0,"slot":3,"value":0.6},
    {"action":"set_target_active","part":0,"slot":4,"flag":true},
    {"action":"set_target_base","part":0,"slot":4,"value":0.7},

    {"action":"set_voice_decay","part":0,"value":0.7},
    {"action":"set_fx_target_base","part":0,"slot":3,"value":0.35},
    {"action":"set_reverb_size","value":0.85},

    {"_comment":"PART B muted."},
    {"action":"set_target_active","part":1,"slot":4,"flag":false},
    {"action":"set_target_base","part":1,"slot":4,"value":0.0}
  ],
  "events": [
    {"t":24.0,"action":"set_probability","part":0,"value":0.0},
    {"_comment":"24s: probability 0 - lanes freeze, NO new fires; the sustaining voice must keep humming (acceptance: FLOW at prob 0 is never silent)."},
    {"t":36.0,"action":"set_probability","part":0,"value":0.8},
    {"_comment":"36s: probability reopens - fires swell fresh voices over the drone."},
    {"t":52.0,"action":"set_probability","part":0,"value":1.0}
  ]
}
```

- [ ] **Step 3: Render both demos and verify the CSV/WAV acceptance criteria**

```bash
./build/render.exe host/render/scenarios/overlapping_voices.json renders/overlapping_voices.wav renders/overlapping_voices.csv
./build/render.exe host/render/scenarios/flow_drone.json renders/flow_drone.wav renders/flow_drone.csv
python - <<'EOF'
import csv, wave, struct

# --- overlapping_voices: >= 2 simultaneously active voices, envelopes overlap ---
rows = list(csv.DictReader(open('renders/overlapping_voices.csv')))
voices = [int(r['a_voices']) for r in rows]
assert max(voices) >= 2, f"expected overlapping voices, max={max(voices)}"
overlap = sum(1 for v in voices if v >= 2) / len(voices)
assert overlap > 0.2, f"voices overlap only {overlap:.0%} of the time"
assert any(sum(1 for k in ('a_v0','a_v1','a_v2','a_v3') if float(r[k]) > 0.05) >= 2
           for r in rows), "v0..v3 envelopes never overlap visibly"

# --- flow_drone: continuous non-silent output from t=0 (drone promise),
#     including the probability-0 stretch at 24-36 s ---
w = wave.open('renders/flow_drone.wav', 'rb')
sr, n = w.getframerate(), w.getnframes()
data = w.readframes(n)
bytes_per_s = sr * 2 * 2                    # 16-bit stereo
for s in range(n // sr):
    chunk = data[s * bytes_per_s:(s + 1) * bytes_per_s]
    vals = struct.unpack('<%dh' % (len(chunk) // 2), chunk)
    rms = (sum(v * v for v in vals) / len(vals)) ** 0.5
    assert rms > 30, f"drone went silent in second {s} (rms={rms:.1f})"
print("m2 acceptance ok")
EOF
```
Expected: `m2 acceptance ok`.

- [ ] **Step 4: Verify bit-determinism and the CPU early indicator**

```bash
./build/render.exe host/render/scenarios/overlapping_voices.json /tmp/m2a.wav /tmp/m2a.csv
./build/render.exe host/render/scenarios/overlapping_voices.json /tmp/m2b.wav /tmp/m2b.csv
cmp /tmp/m2a.wav /tmp/m2b.wav && echo "deterministic"
time ./build/render.exe host/render/scenarios/dorian_melody.json /tmp/base.wav /tmp/base.csv
time ./build/render.exe host/render/scenarios/flow_drone.json /tmp/synth.wav /tmp/synth.csv
```
Expected: `deterministic`; the synth render's real time stays the same order of magnitude as the test-tone baseline (desktop render time is the CPU early indicator per the spec — record both numbers in the commit message body; the hard `METER` check is M6).

- [ ] **Step 5: Listen**

Open `renders/overlapping_voices.wav` and `renders/flow_drone.wav`. By-ear acceptance (spec): the melody is an evolving quantized texture whose notes bloom into each other; the drone breathes (TIMBRE/FILTER), holds through the probability-0 stretch, and swells layer over it after 36 s. The spec's "assumptions to verify" while listening: pulse-end polyblep clean at high notes; hard steals click-free in practice; sustain 0.7 sits right against decaying voices. If a level or character is off, adjust base/depth/ratio values in the JSONs first; engine-constant changes (sustain level, kVoiceGain, drift ranges) are spec "assumptions to verify" — flag them to the user rather than silently retuning.

- [ ] **Step 6: Update docs**

`docs/roadmap.md`:
- Status table: change the M2 row to
  `| **M2** | Polyphonic synth voice (replaces the M1 test tone) | ✅ **done** (engine + host; UI wiring deferred to M6) |`
- Delete the `### M2 — Polyphonic synth voice ⬜` block from the **Planned** section.
- Add to the **Done** section (after the M1.6 entry), following its style:

```markdown
### M2 — Polyphonic synth voice ✅

4-voice trigger-driven synth engine (`engine/synth/`) is the boot-default
part engine; `TestToneEngine` stays selectable (`set_engine` — tests, A/B
reference).

- **Voice** — 2× polyblep `MorphOsc` (single phasor, continuous
  sine→tri→saw→pulse, detune in cents) + sub sine → DaisySP `Svf` lowpass →
  exponential AD/ADS envelope (retrigger-from-level) → equal-power pan with
  slow deterministic per-voice drift. Audio-path sine is the shared
  polynomial `fast_sin` (`engine/util/fast_sin.h`) — no libm `sinf` in the
  voice path; drift + envelope coefficients update at control rate
  (96-sample blocks). CPU-budget constraints from the spec.
- **Engine** — round-robin allocation, oldest-steal with retrigger-from-
  level; STEP = plain AD notes; FLOW = sustaining-last-voice drone (sustain
  0.7, pitch continuously follows the quantized PITCH target; entering FLOW
  with no sustaining voice auto-triggers — the drone promise). Targets:
  TIMBRE (morph + t²·DETUNE_MAX detune), FILTER (60 Hz–14 kHz exp), PITCH
  (latched at trigger, 110·8^p), MOTION (pan fan ±1/±0.5 × width + drift),
  LEVEL (smoothed master gain).
- **Tempo-coupled envelopes** — attack/decay are ratios of the master
  modulation cycle (defaults 2 % / 1.5×, attack floor 2 ms, decay clamp
  50 ms–20 s), edited via `set_voice_attack/decay/resonance/sub/detune`
  (VOICE layer; hardware gestures in M6).
- **Part / Instrument** — `set_engine(EngineId)` with a click-free
  SoftSwitch crossfade; `set_cycle`/`set_flow` forwarding (default no-ops on
  `IPartEngine`); `trigger_manual` (PLAY tap); `active_voices` / `voice_env`
  introspection.
- **Host** — 7 new scenario actions; `voices` + `v0..v3` CSV columns; demo
  scenarios `overlapping_voices.json` (the master spec's M2 acceptance demo)
  and `flow_drone.json`. Existing scenarios pinned to `ENGINE_TEST_TONE`.
- **UI (M6)** — VOICE edit layer gestures (PLAY-pad hold), PLAY-tap manual
  trigger wiring, engine-switch gesture.
```

`README.md` — in the milestone status table, change the M2 row's status cell from `planned` to `**done** (engine + host)` (keep the row's scope text as is).

- [ ] **Step 7: Full build + tests one last time, then commit**

```bash
source env.sh && cmake --build build && ctest --test-dir build --output-on-failure
```
Expected: PASS.

```bash
git add host/render/scenarios/overlapping_voices.json host/render/scenarios/flow_drone.json \
        renders/overlapping_voices.wav renders/overlapping_voices.csv \
        renders/flow_drone.wav renders/flow_drone.csv docs/roadmap.md README.md
git commit -m "feat(host): overlapping_voices + flow_drone demos; docs: M2 done

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

(Commit the renders only if the repo already tracks `renders/` — it does: `git ls-files renders/` shows the M1/M1.6 WAVs. Follow that precedent.)

---

## Acceptance criteria traceability (spec → plan)

| Spec criterion | Where verified |
|---|---|
| `engine/synth/` compiles on desktop, no libDaisy, DaisySP (`Svf`) only | Tasks 4–5 builds; include audit in self-review |
| Fast polynomial sine, no libm `sinf` in the voice audio path, shared desktop/firmware | Task 1 (`fast_sin` + error-bound test); Tasks 2, 4 use it exclusively |
| Control-rate drift/env-coefficient updates (per 96 samples) | Task 5 (`kCtrlInterval`, `_update_control`); Task 4 (`update_control(dt_s)`) |
| MorphOsc: freq accuracy, morph anchors, bounds, detune beat | Task 2 tests |
| Envelope: tempo-coupled ratios, attack floor 2 ms, decay clamp, STEP decays to silence | Task 3 tests + Task 5 "decay tracks set_cycle" / "attack floor" tests |
| Allocation: 4 fill, 5th steals oldest, retrigger-from-level, no click | Task 5 allocation test + Task 4 steal-continuity test |
| Drone: auto-trigger on entering FLOW, sustain 0.7, pitch tracks target, demotion | Task 5 drone tests |
| Pitch contract `110·8^p`, STEP latch | Task 5 pitch-contract test |
| MOTION: width 0 mono, width 1 separation, equal-power, deterministic drift | Task 5 MOTION test + Task 4 pan/drift tests |
| Engine switch click-free via SoftSwitch; boot default ENGINE_SYNTH | Task 6 tests |
| `set_cycle`/`set_flow` default no-ops; Part forwards cycle-on-change + flow | Task 5 (interface) + Task 6 (forwarding tests) |
| Instrument API verbatim (set_engine, set_voice_*, trigger_manual, active_voices) | Task 7 |
| Boot defaults: synth engine, attack 2 %, decay 1.5×, res 0.15, sub 0.3, detune 18 ct, sustain 0.7 | Task 5 member initializers + Task 6/7 boot tests |
| Host actions 1:1; `voices` + `v0..v3` CSV columns | Task 8 |
| Existing tests/scenarios select ENGINE_TEST_TONE explicitly where they assumed it | Task 8 Step 5 (scenarios); Locked decision 10 (tests verified engine-agnostic) |
| `overlapping_voices.json`: ≥ 2 simultaneous voices, v0..v3 overlap, evolving quantized texture | Task 9 Steps 3 + 5 |
| `flow_drone.json`: never silent from t = 0, breathing, swells over the drone | Task 9 Steps 3 + 5 |
| Probability 0: FLOW never silent / STEP decays to silence and stays | Task 6 test + Task 9 render (prob-0 stretch) |
| Bit-determinism invariant | Task 5 determinism test + Task 9 Step 4 (`cmp` on WAVs) |
| All new tests pass; existing stay green | every task's ctest step |

## Self-Review (completed against the spec)

**1. Spec coverage.** Every M2 spec section maps to a task: Goal/voice architecture (T2–T5), pitch contract (T5), targets table incl. TIMBRE t² law and MOTION fan/drift (T5, T4), triggering/allocation/drone incl. the drone promise and steal-from-level (T5), tempo-coupled envelopes + VOICE edit layer engine API (T3, T5; hardware gestures explicitly M6), engine architecture table incl. `IPartEngine` additions and `Part` forwarding (T5, T6), Instrument API (T7), boot defaults (T5–T7), desktop render host incl. CSV columns and both demos (T8, T9), testing list (T2–T6), CPU-budget binding constraints (T1 + Global Constraints + control-rate design), assumptions-to-verify (T9 Step 5 listening notes), acceptance criteria (traceability table above). PLAY-tap = `trigger_manual` engine path is T6/T7/T8; its pad gesture is M6 scope per the spec.

**2. Placeholder scan.** No TBD/TODO/"similar to Task N"/"add error handling" anywhere; every code step contains complete, compilable code; every test step has real assertions; every run step has the exact command and expected outcome.

**3. Type consistency.** Threaded and checked: `fast_sin(float)→float` (T1) used by `MorphOsc`/`Voice` (T2/T4); `MorphOsc::set_freq/set_detune_cents/set_morph/process` (T2) consumed by `Voice` (T4); `Env::set_times/set_sustain/trigger/process/active/value` (T3) consumed by `Voice` (T4) and driven via `Voice::set_env_times/set_sustaining` from `SynthEngine` (T5); `Voice` control-rate feeds match `_update_control`'s calls one-for-one; `EngineId`/`set_cycle(float)`/`set_flow(bool)` declared in T5's `engine_iface.h` and consumed by `Part` (T6); `SynthEngine::set_attack/set_decay/set_resonance/set_sub/set_detune/active_voices/voice_env/sustain_voice/set_seed` (T5) consumed by `Part`'s forwards (T6); `Part::set_engine/engine_id/set_step/trigger_manual/set_voice_*/active_voices/voice_env` (T6) consumed by `Instrument` (T7); `Instrument::set_engine/set_voice_*/trigger_manual/active_voices/voice_env/engine_id` (T7) consumed by `scenario.cpp`/`main.cpp` (T8). Lane-slot constants match the scenario JSONs (slot 0 = TIMBRE, 1 = FILTER, 2 = PITCH, 3 = MOTION, 4 = LEVEL; fx slot 3 = REV_SEND).

**4. Numeric robustness.** All frequency tests count zero crossings over multi-second windows with ±3 tolerance (M1 lesson: float phasors never close cycles exactly). Envelope timing asserts land inside the analytically derived windows (ln 6 attack law; −80 dB idle = 1.33 × the −60 dB decay time — bounds set to 1.0–1.7×). Width-0 mono is asserted bit-exact (identical gain path), not approximate. Determinism asserts are bit-exact vector comparisons or `cmp` on WAVs. Control-rate quantization (96 samples ≈ 2 ms) is inside every timing tolerance used.

**5. Constraint audit.** `engine/synth/` includes only engine headers + `Filters/svf.h`; no heap anywhere in `engine/` (fixed `std::array`s, plain fields); the only `Rng` use is seeded in `Voice::init`; libm in the audio path: none (`fast_sin` everywhere; `std::pow`/`std::exp` confined to `set_detune_cents`/`_apply_freq`/`set_times`/`pitch_to_hz`/`filter_hz` — all control/trigger rate); LEVEL is OnePole-smoothed; `src/`, `Makefile`, `main.cpp`, `app.cpp`, `app.h` untouched.

---

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-07-11-spotykach-synth-voice.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — dispatch a fresh subagent per task, review between tasks, fast iteration.

**2. Inline Execution** — execute tasks in one session using superpowers:executing-plans, batch execution with checkpoints.

**Prerequisite either way: finish M1.6 first** (see "M1.6 end-state assumption").

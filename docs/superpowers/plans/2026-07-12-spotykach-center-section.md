# Spotykach Center Section (M4) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the center section — MORPH (equal-power A↔B blend), COUPLE (Kuramoto PLL), DRIFT (one OU weather system), SPOT (per-lane stumble) and SETTLE (panic) — as one control-rate `Center` module driving the two existing SuperModulator banks through narrow hooks.

**Architecture:** A new `engine/center/Center` runs once per 96-sample control tick (M2 convention). It reads both banks' PITCH-lane phases, base rates and sync modes, then writes back through four narrow hooks — `SuperModulator::set_rate_scale` (COUPLE × DRIFT rate), `SuperModulator::set_shape_offset` (DRIFT shape), `Part::set_detune_cents` (DRIFT tune), and per-lane `ModLane::kick` (SPOT) — and computes the equal-power morph gains applied in the `Instrument` mix. Cross-part logic lives in exactly one place, testable in isolation with real `SuperModulator`/`Part` objects.

**Tech Stack:** C++17, header-mostly engine in `namespace spky`, doctest unit tests, desktop render host (JSON scenarios → WAV + `mods.csv`). Build: clang + Ninja.

## Global Constraints

_These apply to every task. Exact values copied from the spec._

- **Repo root (the fork):** `c:\Users\bernd\Documents\AI\Spotykach`. All paths below are relative to it. The plan document itself lives in the residency repo; the code lives in the fork.
- **Namespace:** everything in `namespace spky`.
- **No heap, no allocation in the audio path, no libDaisy include anywhere under `engine/`.** `src/` (firmware shell) stays untouched.
- **Control rate = one 96-sample block** (`kCtrlInterval = 96`), matching the M2 `SynthEngine` convention. `Center` computes only at control rate; the only audio-path addition is the morph-gain multiply.
- **Bit-determinism invariant:** identical scenario → bit-identical WAV. All randomness is seeded `spky::Rng` (xorshift32). Never use time/global RNG.
- **Zero-effect invariant:** with `couple = 0` and `drift = 0`, every pre-existing `mods.csv` column and all lane behavior must be **bit-identical** to pre-M4. The single deliberate exception: the mix now applies equal-power morph gains (at the boot default `morph = 0.5` that is −3 dB/part vs. the old unity-sum placeholder), a level change only.
- **Build:** `source env.sh && cmake --build build` (from repo root; `env.sh` puts clang/ninja on `PATH` and sets `CC`/`CXX`/`CMAKE_GENERATOR`). Editing `CMakeLists.txt` triggers an automatic re-configure on the next `cmake --build`.
- **Run all tests:** `./build/spky_tests`. **Run one:** `./build/spky_tests --test-case="<name substring>"`.
- **Render a scenario:** `./build/render host/render/scenarios/<x>.json renders/<x>.wav renders/<x>.csv`.
- **Commit trailer:** end every commit message with
  `Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>`
  (per project convention — **not** the default Claude/Anthropic trailer).
- **TDD:** write the failing test first, watch it fail, implement minimally, watch it pass, commit.

## File Structure

**New files**
- `engine/center/center.h` — the `Center` class declaration (morph/couple/drift state, OU weather walk + two seeded `Rng`, the control-rate `update()`, gestures `spot()`/`settle()`, getters).
- `engine/center/center.cpp` — `Center` implementation (weather step, DRIFT taps, Kuramoto PLL, morph gains).
- `tests/test_center.cpp` — `Center` unit tests (morph law, drift bounds/determinism, couple lock/anchor, spot/settle).
- `host/render/scenarios/couple_lock.json` — COUPLE demo.
- `host/render/scenarios/weather_spot.json` — DRIFT + SPOT + SETTLE demo.

**Modified files**
- `engine/mod/lane.h` / `engine/mod/lane.cpp` — narrow hooks: `set_shape_offset`, `kick`, `settle` + wiring into `_compute_raw`/`process`.
- `engine/mod/super_modulator.h` / `.cpp` — `set_rate_scale`, `set_shape_offset`, `spot`, `settle`, `base_hz`, `sync_mode`; `_update_rate` split into base-rate + `_apply_rate`.
- `engine/parts/part.h` / `.cpp` — `set_detune_cents` acting on the post-quantizer engine pitch (leaving `pitch_cv()` clean).
- `engine/instrument.h` / `.cpp` — owns one `Center`; API `set_morph/set_couple/set_drift/spot/settle` + getters; control-rate `update()` and morph mix in `process()`; boot defaults.
- `host/render/main.cpp` — five new `mods.csv` global columns.
- `host/render/scenario.h` / `.cpp` — five new scenario actions.
- `CMakeLists.txt` — add `center.cpp` (both targets) and `test_center.cpp` (tests).
- `engine/fx/reverb.h`, `engine/instrument.cpp` comments, `docs/roadmap.md` — supersede the M1.6 pre-morph-send wording; mark M4.

---

### Task 1: ModLane center hooks (shape offset, kick, settle)

Add three narrow hooks to the modulation lane: a bank-wide DRIFT **shape offset**, a SPOT **kick** (permanent phase jump + decaying shape offset, no-op while replaying), and a **settle** glide. These are the lowest-level pieces; everything else drives them.

**Files:**
- Modify: `engine/mod/lane.h`
- Modify: `engine/mod/lane.cpp`
- Test: `tests/test_lane.cpp` (append cases)

**Interfaces:**
- Consumes: existing `ModLane` (`_phase`, `_shape`, `_ev_*`, `_replaying()`, `CaptureLoop`).
- Produces (later tasks rely on these exact signatures):
  - `void ModLane::set_shape_offset(float o)` — bank-wide DRIFT shape tap, added to the effective shape, clamped.
  - `void ModLane::kick(float dphase, float dshape)` — permanent phase jump + decaying shape offset (τ ≈ 1.5 s); **no-op while replaying**.
  - `void ModLane::settle()` — glide EVOLVE walk states and any open kick shape offset to 0 over ~1 s.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_lane.cpp` (it already `#include`s doctest, `<cmath>` and `"mod/lane.h"`; add `#include "mod/capture.h"` near the top if not present):

```cpp
TEST_CASE("lane kick: phase jump is permanent, no decay") {
    ModLane lane; lane.init(48000.f, 4u);
    lane.set_rate_hz(0.f);                 // freeze phase advance to isolate the kick
    CHECK(lane.phase() == doctest::Approx(0.f));
    lane.kick(0.25f, 0.f);
    CHECK(lane.phase() == doctest::Approx(0.25f));
    for (int i = 0; i < 1000; ++i) lane.process();
    CHECK(lane.phase() == doctest::Approx(0.25f));   // permanent
}

TEST_CASE("lane kick: no-op while replaying (captured loop is immune)") {
    CaptureLoop loop; loop.reset();
    ModLane lane; lane.init(48000.f, 4u);
    lane.set_capture_loop(&loop);
    lane.set_rate_hz(2.f);
    for (int i = 0; i < 48000; ++i) lane.process();  // record a full cycle
    loop.capture_now();                              // freeze -> valid
    lane.set_replay(true);
    lane.process();                                  // enter replay
    float p = lane.phase();
    lane.kick(0.4f, 0.4f);                           // must be ignored
    CHECK(lane.phase() == doctest::Approx(p));
}

TEST_CASE("lane shape_offset: shifts the effective shape; offset 0 is bit-identical") {
    ModLane a; a.init(48000.f, 8u); a.set_rate_hz(1.f); a.set_shape(0.3f);
    ModLane b; b.init(48000.f, 8u); b.set_rate_hz(1.f); b.set_shape(0.3f);
    b.set_shape_offset(0.4f);
    bool differ = false;
    for (int i = 0; i < 48000; ++i)
        if (std::fabs(a.process() - b.process()) > 1e-4f) differ = true;
    CHECK(differ);

    ModLane c; c.init(48000.f, 8u); c.set_rate_hz(1.f); c.set_shape(0.3f);
    ModLane d; d.init(48000.f, 8u); d.set_rate_hz(1.f); d.set_shape(0.3f);
    d.set_shape_offset(0.f);
    bool same = true;
    for (int i = 0; i < 48000; ++i) if (c.process() != d.process()) same = false;
    CHECK(same);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `source env.sh && cmake --build build 2>&1 | tail -20`
Expected: FAIL to compile — `'set_shape_offset'`, `'kick'` are not members of `ModLane`.

- [ ] **Step 3: Add the declarations to `engine/mod/lane.h`**

In the `public:` section, right after `bool replaying() const { return _replaying(); }` (the M3 capture block), add:

```cpp
    // --- M4 center hooks ---
    void set_shape_offset(float o) { _shape_offset = o; }  // DRIFT bank-wide shape tap
    void kick(float dphase, float dshape);                 // SPOT: phase jump + decaying shape
    void settle();                                         // panic: glide EVOLVE + kick to 0
```

In the `private:` data section, right after the EVOLVE walk members (`float _ev_rate = 0.f;`), add:

```cpp
    // M4 center hooks
    float _shape_offset = 0.f;   // DRIFT shape tap (set per control tick)
    float _kick_shape   = 0.f;   // SPOT shape offset, decays with _kick_coef
    float _kick_coef    = 1.f;   // per-sample decay for _kick_shape (tau ~ 1.5 s)
    int   _settle_ctr   = 0;     // >0: gliding EVOLVE walks + kick to 0
    float _settle_coef  = 1.f;   // per-sample settle glide (tau ~ 0.3 s)
```

- [ ] **Step 4: Implement in `engine/mod/lane.cpp`**

In `ModLane::init`, after `_ev_rate = 0.f;`, add:

```cpp
    _shape_offset = 0.f;
    _kick_shape   = 0.f;
    _kick_coef    = std::exp(-1.f / (1.5f * _sr));   // SPOT shape decay tau = 1.5 s
    _settle_coef  = std::exp(-1.f / (0.3f * _sr));   // SETTLE glide tau = 0.3 s
    _settle_ctr   = 0;
```

In `ModLane::_compute_raw`, change the shape line from:

```cpp
    float sh = clampf(_shape + _ev_shape, 0.f, 1.f);
```
to:
```cpp
    float sh = clampf(_shape + _ev_shape + _shape_offset + _kick_shape, 0.f, 1.f);
```

Add the two new methods (place them near `reset`):

```cpp
void ModLane::kick(float dphase, float dshape) {
    if (_replaying()) return;              // captured loop never mutates (M3/M4 guard)
    _phase += dphase;
    _phase -= std::floor(_phase);          // permanent wrap into [0,1)
    _kick_shape += dshape;                 // decays back to 0 over ~1.5 s
}

void ModLane::settle() {
    _settle_ctr = static_cast<int>(_sr * 1.0f);   // glide EVOLVE + kick over ~1 s
}
```

In `ModLane::process`, immediately after `_fired = false;` (the first line), add:

```cpp
    _kick_shape *= _kick_coef;                 // SPOT shape offset fades toward 0
    if (_settle_ctr > 0) {                     // SETTLE: glide EVOLVE walks + kick to 0
        --_settle_ctr;
        _ev_phase   *= _settle_coef;
        _ev_shape   *= _settle_coef;
        _ev_rate    *= _settle_coef;
        _kick_shape *= _settle_coef;
    }
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `source env.sh && cmake --build build && ./build/spky_tests --test-case="lane kick*,lane shape_offset*"`
Expected: PASS. Then run the full suite to confirm the zero-effect wiring did not perturb existing lanes:
Run: `./build/spky_tests`
Expected: `Status: SUCCESS!`, all 145 pre-existing cases still green.

- [ ] **Step 6: Commit**

```bash
git add engine/mod/lane.h engine/mod/lane.cpp tests/test_lane.cpp
git commit -m "$(cat <<'EOF'
feat(m4): ModLane center hooks — shape offset, SPOT kick, settle glide

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 2: SuperModulator center hooks (rate scale, shape offset, spot, settle)

Give the bank the hooks `Center` writes to: a `rate_scale` multiplier applied after knob/sync (carrying COUPLE **and** DRIFT rate), a bank-wide shape offset, per-lane SPOT, a settle forward, plus the getters `base_hz()` (pre-scale rate) and `sync_mode()`.

**Files:**
- Modify: `engine/mod/super_modulator.h`
- Modify: `engine/mod/super_modulator.cpp`
- Test: `tests/test_super_modulator.cpp` (append cases)

**Interfaces:**
- Consumes: `ModLane::set_shape_offset`, `ModLane::kick`, `ModLane::settle` (Task 1); `Rng` (`engine/mod/rng.h`, visible via `lane.h`).
- Produces (Task 6 / Task 8 rely on these):
  - `void set_rate_scale(float s)` — multiplies the master rate after knob/sync; recomputes lane rates.
  - `void set_shape_offset(float o)` — broadcast DRIFT shape tap to all lanes.
  - `void spot(Rng& rng)` — draw a per-lane phase kick (±½ cycle) and shape kick (±0.35) and apply via `ModLane::kick` (no-op on the replaying lane).
  - `void settle()` — forward to all lanes.
  - `float base_hz() const` — master rate **before** `rate_scale` (the geometric-mean input for COUPLE).
  - `SyncMode sync_mode() const`.
  - `float master_hz() const` — now the **effective** (post-scale) rate.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_super_modulator.cpp`:

```cpp
TEST_CASE("super: rate_scale multiplies the master rate; base_hz stays put") {
    SuperModulator m; m.init(48000.f, 42u);
    m.set_sync_mode(SyncMode::Free); m.set_rate(0.5f);
    float base = m.base_hz();
    m.set_rate_scale(2.f);
    CHECK(m.base_hz()   == doctest::Approx(base));
    CHECK(m.master_hz() == doctest::Approx(base * 2.f));
}

TEST_CASE("super: rate_scale 1 is a bit-identical no-op") {
    SuperModulator a; a.init(48000.f, 42u); a.set_rate(0.5f);
    SuperModulator b; b.init(48000.f, 42u); b.set_rate(0.5f);
    b.set_rate_scale(1.f);
    bool same = true;
    for (int i = 0; i < 48000; ++i) {
        a.process(); b.process();
        for (int s = 0; s < LANE_COUNT; ++s)
            if (a.lane_output(s) != b.lane_output(s)) same = false;
    }
    CHECK(same);
}

TEST_CASE("super: sync_mode getter reflects the set mode") {
    SuperModulator m; m.init(48000.f, 42u);
    m.set_sync_mode(SyncMode::Sync);
    CHECK(m.sync_mode() == SyncMode::Sync);
}

TEST_CASE("super: spot kicks the live lanes deterministically") {
    SuperModulator a; a.init(48000.f, 1u);
    float before[LANE_COUNT];
    for (int i = 0; i < LANE_COUNT; ++i) before[i] = a.lane_phase(i);
    Rng rng; rng.seed(77u);
    a.spot(rng);
    int moved = 0;
    for (int i = 0; i < LANE_COUNT; ++i)
        if (std::fabs(a.lane_phase(i) - before[i]) > 1e-6f) ++moved;
    CHECK(moved >= 3);

    // determinism: same seed -> same kicks
    SuperModulator x; x.init(48000.f, 1u);
    SuperModulator y; y.init(48000.f, 1u);
    Rng rx; rx.seed(5u); Rng ry; ry.seed(5u);
    x.spot(rx); y.spot(ry);
    CHECK(x.lane_phase(LANE_SOURCE) == y.lane_phase(LANE_SOURCE));
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `source env.sh && cmake --build build 2>&1 | tail -20`
Expected: FAIL to compile — `set_rate_scale`, `base_hz`, `sync_mode`, `spot` not members.

- [ ] **Step 3: Edit `engine/mod/super_modulator.h`**

Add to `public:` after `float master_hz() const { return _master_hz; }`:

```cpp
    // --- M4 center hooks ---
    void set_rate_scale(float s)  { _rate_scale = s; _apply_rate(); }  // COUPLE * DRIFT rate
    void set_shape_offset(float o){ for (auto& l : _lanes) l.set_shape_offset(o); }
    void spot(Rng& rng);          // per-lane SPOT kicks (skips the replaying PITCH lane)
    void settle()                { for (auto& l : _lanes) l.settle(); }
    float    base_hz()   const { return _base_hz; }   // rate before COUPLE/DRIFT scale
    SyncMode sync_mode() const { return _mode; }
```

In `private:`, add a method declaration next to `void _update_rate();`:

```cpp
    void _apply_rate();
```

In the private data, add next to `float _master_hz = 1.f;`:

```cpp
    float _base_hz    = 1.f;   // rate from knob/sync, before rate_scale
    float _rate_scale = 1.f;   // COUPLE * DRIFT rate multiplier
```

- [ ] **Step 4: Edit `engine/mod/super_modulator.cpp`**

Replace `_update_rate()` with a base-rate computation that defers lane wiring to `_apply_rate()`:

```cpp
void SuperModulator::_update_rate() {
    switch (_mode) {
        case SyncMode::Free:        _base_hz = free_hz(_rate_norm); break;
        case SyncMode::Sync:        _base_hz = sync_hz(_rate_norm, _bpm, false); break;
        case SyncMode::SyncTriplet: _base_hz = sync_hz(_rate_norm, _bpm, true); break;
    }
    _apply_rate();
}

void SuperModulator::_apply_rate() {
    _master_hz = _base_hz * _rate_scale;
    for (int i = 0; i < LANE_COUNT; ++i)
        _lanes[i].set_rate_hz(_master_hz * kLaneRatio[i]);
}
```

In `SuperModulator::init`, before the final `_update_rate();`, add:

```cpp
    _rate_scale = 1.f;
```

Add the `spot` implementation at the end of the file:

```cpp
void SuperModulator::spot(Rng& rng) {
    // Draw a kick for every lane so the RNG stream is independent of replay
    // state; ModLane::kick() no-ops on the replaying PITCH lane.
    for (int i = 0; i < LANE_COUNT; ++i) {
        float dphase = rng.next_bipolar() * 0.5f;    // uniform +/- 1/2 cycle
        float dshape = rng.next_bipolar() * 0.35f;   // uniform +/- 0.35
        _lanes[i].kick(dphase, dshape);
    }
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `source env.sh && cmake --build build && ./build/spky_tests --test-case="super:*"`
Expected: PASS (new cases + the existing `super:` cases still green — rate ratios, SYNC rate, triplet).
Run: `./build/spky_tests`
Expected: `Status: SUCCESS!`.

- [ ] **Step 6: Commit**

```bash
git add engine/mod/super_modulator.h engine/mod/super_modulator.cpp tests/test_super_modulator.cpp
git commit -m "$(cat <<'EOF'
feat(m4): SuperModulator center hooks — rate_scale, shape offset, spot, settle

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 3: Part detune (engine pitch only, CV stays clean)

DRIFT's tune tap detunes the pitch handed to the **engine** (after the quantizer) while `pitch_cv()` — the rack CV out — stays exactly on the scale grid.

**Files:**
- Modify: `engine/parts/part.h`
- Modify: `engine/parts/part.cpp`
- Test: `tests/test_part.cpp` (append a case)

**Interfaces:**
- Produces (Task 6 relies on this): `void Part::set_detune_cents(float c)` — bipolar cents added to the engine pitch only. Normalized pitch spans 36 semitones over 0..1, so 1 cent = `1/3600`.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_part.cpp`:

```cpp
TEST_CASE("part detune: engine pitch shifts but pitch_cv stays quantized") {
    Part p; p.init(48000.f, 1u);
    p.set_depth(0.f);                          // isolate the base value
    p.set_target_base(LANE_PITCH, 0.5f);
    p.set_tune(0.5f);
    float l, r;
    for (int i = 0; i < 4000; ++i) p.process(l, r);   // ride out the quantizer slew
    float cv0 = p.pitch_cv();
    p.set_detune_cents(50.f);                  // +50 cents on the engine pitch
    for (int i = 0; i < 4000; ++i) p.process(l, r);
    CHECK(p.pitch_cv() == doctest::Approx(cv0));       // rack CV out unchanged
}
```
(The audible engine detune — internal — is verified by ear in the `weather_spot` render's tune beating.)

- [ ] **Step 2: Run the test to verify it fails**

Run: `source env.sh && cmake --build build 2>&1 | tail -20`
Expected: FAIL to compile — `set_detune_cents` is not a member of `Part`.

- [ ] **Step 3: Edit `engine/parts/part.h`**

In `public:`, right after `void set_tune(float t) { _tune = clampf(t, 0.f, 1.f); }`, add:

```cpp
    void set_detune_cents(float c) { _detune_cents = c; }   // DRIFT tune tap; engine pitch only
```

In `private:`, next to `float _tune = 0.5f;`, add:

```cpp
    float _detune_cents = 0.f;   // DRIFT detune, applied post-quantizer to the engine only
```

- [ ] **Step 4: Edit `engine/parts/part.cpp`**

In `Part::process`, the pitch is currently:

```cpp
    targets[LANE_PITCH] = _quant.process(pitch_pre_quant());
    _pitch_q = targets[LANE_PITCH];
```
Change to:
```cpp
    targets[LANE_PITCH] = _quant.process(pitch_pre_quant());
    _pitch_q = targets[LANE_PITCH];                              // clean, drives pitch_cv()
    targets[LANE_PITCH] = clampf(_pitch_q + _detune_cents * (1.f / 3600.f), 0.f, 1.f);
```
`_pitch_q` (hence `pitch_cv()`) stays the clean quantized value; only the copy handed to `_engine->set_targets`/`trigger` is detuned.

- [ ] **Step 5: Run the test to verify it passes**

Run: `source env.sh && cmake --build build && ./build/spky_tests --test-case="part detune*"`
Expected: PASS.
Run: `./build/spky_tests`
Expected: `Status: SUCCESS!` (detune 0 default → `clampf(_pitch_q + 0)` is bit-identical, existing part/instrument tests stay green).

- [ ] **Step 6: Commit**

```bash
git add engine/parts/part.h engine/parts/part.cpp tests/test_part.cpp
git commit -m "$(cat <<'EOF'
feat(m4): Part detune_cents — engine pitch floats, pitch_cv stays quantized

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 4: Center skeleton + MORPH (equal-power gains)

Create the `Center` class with its control-rate `update()` computing only the **equal-power morph gains** for now (COUPLE/DRIFT/SPOT arrive in Tasks 5–7). Wire `center.cpp` into the build and start `tests/test_center.cpp`. `update()`'s full signature is fixed here so later tasks only fill in the body.

**Files:**
- Create: `engine/center/center.h`
- Create: `engine/center/center.cpp`
- Create: `tests/test_center.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `SuperModulator` (Task 2 hooks), `Part` (Task 3 hook), `Rng`, `OnePole`, `clampf`/`TWO_PI` (`util/math.h`).
- Produces (Tasks 5–8 rely on these exact signatures):
  - `void Center::init(float sample_rate, uint32_t seed)`
  - `void Center::set_morph(float m)` / `set_couple(float c)` / `set_drift(float d)`
  - `void Center::update(SuperModulator& a, SuperModulator& b, Part& pa, Part& pb)`
  - `void Center::spot(SuperModulator& a, SuperModulator& b)` (stub in Task 4, filled Task 7)
  - `void Center::settle(SuperModulator& a, SuperModulator& b)` (stub in Task 4, filled Task 7)
  - getters `gain_a() gain_b() morph() couple() drift() weather() phase_err()` (all `float`, `const`)
  - `static constexpr int Center::kCtrlInterval = 96`

- [ ] **Step 1: Write the failing tests**

Create `tests/test_center.cpp`:

```cpp
#include <doctest/doctest.h>
#include <cmath>
#include "center/center.h"
#include "mod/super_modulator.h"
#include "parts/part.h"
using namespace spky;

// Small fixture: two banks + two parts + a Center, all deterministically seeded.
namespace {
struct Rig {
    Center c; SuperModulator a, b; Part pa, pb;
    void init(uint32_t cseed = 123u) {
        c.init(48000.f, cseed);
        a.init(48000.f, 1u); b.init(48000.f, 2u);
        pa.init(48000.f, 1u); pb.init(48000.f, 2u);
    }
    void ticks(int n) { for (int k = 0; k < n; ++k) c.update(a, b, pa, pb); }
};
} // namespace

TEST_CASE("center morph: equal-power law holds across the sweep") {
    Rig r; r.init();
    for (float m = 0.f; m <= 1.001f; m += 0.05f) {
        r.c.set_morph(m);
        r.ticks(600);                                  // settle the smoother
        float ga = r.c.gain_a(), gb = r.c.gain_b();
        CHECK(ga * ga + gb * gb == doctest::Approx(1.f).epsilon(0.005));
    }
}

TEST_CASE("center morph: 0 is full A, 1 is full B") {
    Rig r; r.init();
    r.c.set_morph(0.f); r.ticks(2000);
    CHECK(r.c.gain_a() == doctest::Approx(1.f).epsilon(0.005));
    CHECK(r.c.gain_b() == doctest::Approx(0.f).epsilon(0.005));
    r.c.set_morph(1.f); r.ticks(2000);
    CHECK(r.c.gain_a() == doctest::Approx(0.f).epsilon(0.005));
    CHECK(r.c.gain_b() == doctest::Approx(1.f).epsilon(0.005));
}

TEST_CASE("center morph: smoothing — no click-sized step per control tick after a jump") {
    Rig r; r.init();
    r.c.set_morph(0.f); r.ticks(2000);
    r.c.set_morph(1.f);                                // hard jump
    float prev = r.c.gain_a();
    for (int k = 0; k < 2000; ++k) {
        r.ticks(1);
        float g = r.c.gain_a();
        CHECK(std::fabs(g - prev) < 0.05f);
        prev = g;
    }
}
```

- [ ] **Step 2: Create `engine/center/center.h`**

```cpp
#pragma once
#include <cstdint>
#include "parts/part.h"
#include "mod/super_modulator.h"
#include "mod/rng.h"
#include "util/onepole.h"
#include "util/math.h"

namespace spky {

// The center section: one control-rate brain over the two banks. Owns MORPH
// (equal-power A/B gains), COUPLE (Kuramoto PLL), DRIFT (one OU weather walk
// tapped to six destinations), SPOT (per-lane stumble) and SETTLE (panic).
// update() reads both banks' phases/rates and writes back through narrow hooks;
// the audio path only multiplies the morph gains.
class Center {
public:
    static constexpr int kCtrlInterval = 96;   // control tick, matches M2

    void init(float sample_rate, uint32_t seed);

    // performable amounts
    void set_morph(float m)  { _morph_target  = clampf(m, 0.f, 1.f); }
    void set_couple(float c) { _couple        = clampf(c, 0.f, 1.f); }
    void set_drift(float d)  { _drift_target  = clampf(d, 0.f, 1.f); }

    // one control tick: read both banks, write hooks, advance weather + morph
    void update(SuperModulator& a, SuperModulator& b, Part& pa, Part& pb);

    // gestures
    void spot(SuperModulator& a, SuperModulator& b);
    void settle(SuperModulator& a, SuperModulator& b);

    // getters (CSV + later LEDs)
    float gain_a()    const { return _g_a; }
    float gain_b()    const { return _g_b; }
    float morph()     const { return _morph; }
    float couple()    const { return _couple; }
    float drift()     const { return _drift; }
    float weather()   const { return _weather; }
    float phase_err() const { return _phase_err; }

private:
    void _step_weather();

    float _sr = 48000.f;
    float _cr = 500.f;               // control rate = sr / kCtrlInterval

    // MORPH
    OnePole _morph_smooth;
    float   _morph_target = 0.5f;
    float   _morph = 0.5f;
    float   _g_a = 0.70710678f;
    float   _g_b = 0.70710678f;

    // COUPLE
    float _couple = 0.f;
    float _phase_err = 0.f;

    // DRIFT
    OnePole _drift_smooth;
    float   _drift_target = 0.f;
    float   _drift = 0.f;
    float   _ou = 0.f;               // Ornstein-Uhlenbeck state (unbounded)
    float   _weather = 0.f;          // tanh(_ou), softly bounded to (-1,1)
    Rng     _weather_rng;

    // SPOT / SETTLE
    Rng   _spot_rng;
    int   _settle_ctr = 0;           // >0: weather gliding to 0
    float _settle_coef = 1.f;
};

} // namespace spky
```

- [ ] **Step 3: Create `engine/center/center.cpp` (morph body; weather/couple stubbed to zero-effect)**

```cpp
#include "center/center.h"
#include <cmath>

using namespace spky;

namespace {
constexpr float kQuarter = TWO_PI * 0.25f;   // pi/2, for the equal-power law
}

void Center::init(float sample_rate, uint32_t seed) {
    _sr = sample_rate;
    _cr = sample_rate / static_cast<float>(kCtrlInterval);

    _morph_target = 0.5f; _morph = 0.5f;
    _morph_smooth.init(_cr, 0.02f);
    _morph_smooth.reset(0.5f);
    _g_a = std::cos(_morph * kQuarter);
    _g_b = std::sin(_morph * kQuarter);

    _couple = 0.f; _phase_err = 0.f;

    _drift_target = 0.f; _drift = 0.f;
    _drift_smooth.init(_cr, 0.3f);
    _drift_smooth.reset(0.f);
    _ou = 0.f; _weather = 0.f;
    _weather_rng.seed(seed);

    _spot_rng.seed(seed ^ 0x51207AB7u);
    _settle_ctr = 0;
    _settle_coef = std::exp(-1.f / (0.3f * _cr));
}

void Center::update(SuperModulator& a, SuperModulator& b, Part& pa, Part& pb) {
    // --- MORPH (equal-power, smoothed at control rate) ---
    _morph = _morph_smooth.process(_morph_target);
    _g_a = std::cos(_morph * kQuarter);
    _g_b = std::sin(_morph * kQuarter);

    // COUPLE and DRIFT arrive in Tasks 5-6; keep the hooks at their zero-effect
    // values so pre-M4 behavior is bit-identical.
    a.set_rate_scale(1.f);   b.set_rate_scale(1.f);
    a.set_shape_offset(0.f); b.set_shape_offset(0.f);
    pa.set_detune_cents(0.f); pb.set_detune_cents(0.f);
    (void)pa; (void)pb;
}

void Center::_step_weather() { /* filled in Task 5 */ }

void Center::spot(SuperModulator& a, SuperModulator& b) {
    a.spot(_spot_rng); b.spot(_spot_rng);
}

void Center::settle(SuperModulator& a, SuperModulator& b) {
    _drift_target = 0.f;
    _settle_ctr = static_cast<int>(_cr * 1.5f);
    a.settle(); b.settle();
}
```

- [ ] **Step 4: Wire the build in `CMakeLists.txt`**

In the `add_executable(spky_tests ...)` list, after the `engine/instrument.cpp`/`tests/test_instrument.cpp` pair, add:

```cmake
    engine/center/center.cpp
    tests/test_center.cpp
```

In the `add_executable(render ...)` list, after `engine/instrument.cpp`, add:

```cmake
    engine/center/center.cpp
```

- [ ] **Step 5: Build and run the tests**

Run: `source env.sh && cmake --build build && ./build/spky_tests --test-case="center morph*"`
Expected: PASS (three morph cases). `cmake --build` auto-reconfigures for the edited `CMakeLists.txt`.
Run: `./build/spky_tests`
Expected: `Status: SUCCESS!`.

- [ ] **Step 6: Commit**

```bash
git add engine/center/center.h engine/center/center.cpp tests/test_center.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(m4): Center skeleton + equal-power MORPH gains

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 5: Center DRIFT (Ornstein–Uhlenbeck weather + six taps)

One slow mean-reverting random walk (τ ≈ 45 s, softly bounded to −1..1), stepped at control rate, tapped by six destinations with hardcoded polarities/scales. `set_drift` scales all taps (smoothed). Rate taps fold into `rate_scale`, shape into `set_shape_offset`, tune into `set_detune_cents`.

**Files:**
- Modify: `engine/center/center.cpp`
- Test: `tests/test_center.cpp` (append)

**Interfaces:**
- Consumes: the Task-4 `update()` skeleton and the zero-effect hook writes.
- Produces: `weather()` now returns the live walk; `update()` writes DRIFT into the three hooks. Rate-tap folding leaves room for the Task-6 COUPLE multiplier (`rate_scale = couple_mult × rate_drift`).

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_center.cpp`:

```cpp
TEST_CASE("center drift: drift 0 leaves the rate hook at unity, weather at 0") {
    Rig r; r.init(7u);
    r.a.set_rate(0.5f); r.b.set_rate(0.5f);
    float base = r.a.base_hz();
    r.c.set_couple(0.f); r.c.set_drift(0.f);
    r.ticks(5000);
    CHECK(r.a.master_hz() == doctest::Approx(base));
    CHECK(r.c.weather()   == doctest::Approx(0.f));
}

TEST_CASE("center drift: weather stays in (-1,1) and mean-reverts around 0") {
    Rig r; r.init(999u);
    r.c.set_drift(1.f);
    double sum = 0; int n = 0;
    for (int k = 0; k < 40000; ++k) {
        r.ticks(1);
        CHECK(r.c.weather() >  -1.f);
        CHECK(r.c.weather() <   1.f);
        sum += r.c.weather(); ++n;
    }
    CHECK(std::fabs(sum / n) < 0.5);
}

TEST_CASE("center drift: deterministic per seed") {
    auto run = [](uint32_t seed) {
        Rig r; r.init(seed);
        r.c.set_drift(1.f);
        float last = 0.f;
        for (int k = 0; k < 3000; ++k) { r.ticks(1); last = r.c.weather(); }
        return last;
    };
    CHECK(run(42u) == run(42u));
    CHECK(run(42u) != run(43u));
}

TEST_CASE("center drift: at full drift the two banks' rates move apart (opposite polarity)") {
    Rig r; r.init(321u);
    r.a.set_rate(0.5f); r.b.set_rate(0.5f);
    r.c.set_drift(1.f);
    bool a_moved = false, b_moved = false;
    float ba = r.a.base_hz(), bb = r.b.base_hz();
    for (int k = 0; k < 6000; ++k) {
        r.ticks(1);
        if (std::fabs(r.a.master_hz() - ba) > 1e-3f) a_moved = true;
        if (std::fabs(r.b.master_hz() - bb) > 1e-3f) b_moved = true;
    }
    CHECK(a_moved);
    CHECK(b_moved);
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `source env.sh && cmake --build build && ./build/spky_tests --test-case="center drift*" 2>&1 | tail -20`
Expected: FAIL — `weather()` stays 0 (walk not stepped) and the rate hook never moves.

- [ ] **Step 3: Implement the weather step + taps in `engine/center/center.cpp`**

Add DRIFT constants to the anonymous namespace at the top:

```cpp
namespace {
constexpr float kQuarter = TWO_PI * 0.25f;   // pi/2, for the equal-power law

constexpr float kOuTau   = 45.f;             // weather time constant (s)
constexpr float kOuSigma = 0.10f;            // weather noise scale

// DRIFT taps (polarity/scale of the full walk — spec table). Index 0 = A, 1 = B.
constexpr float kRateTap[2]  = { 1.0f, -0.6f };   // x kRateOct octaves
constexpr float kShapeTap[2] = { 0.8f, -1.0f };   // x kShapeMax
constexpr float kTuneTap[2]  = { 0.5f, -0.9f };   // x kTuneCents
constexpr float kRateOct   = 0.5f;                // up to +/- 1/2 octave
constexpr float kShapeMax  = 0.15f;               // up to +/- 0.15 shape
constexpr float kTuneCents = 25.f;                // up to +/- 25 cents
}
```
(If you already added `kQuarter` in Task 4, just extend the block — don't duplicate it.)

Replace the `_step_weather()` stub with:

```cpp
void Center::_step_weather() {
    const float dt = static_cast<float>(kCtrlInterval) / _sr;   // control-tick period (s)
    if (_settle_ctr > 0) {
        --_settle_ctr;
        _ou *= _settle_coef;                    // panic: glide the walk to 0
    } else {
        // Ornstein-Uhlenbeck: mean-revert to 0, add scaled white noise.
        _ou += (-_ou / kOuTau) * dt + kOuSigma * std::sqrt(dt) * _weather_rng.next_bipolar();
    }
    _weather = std::tanh(_ou);                  // softly bounded to (-1, 1)
}
```

Rewrite `update()` so DRIFT drives the hooks. Keep the COUPLE multiplier as a unity placeholder (filled in Task 6):

```cpp
void Center::update(SuperModulator& a, SuperModulator& b, Part& pa, Part& pb) {
    // --- MORPH (equal-power, smoothed at control rate) ---
    _morph = _morph_smooth.process(_morph_target);
    _g_a = std::cos(_morph * kQuarter);
    _g_b = std::sin(_morph * kQuarter);

    // --- DRIFT amount (smoothed) + weather step ---
    _drift = _drift_smooth.process(_drift_target);
    _step_weather();
    const float w = _weather * _drift;          // exactly 0 while drift is 0

    const float rate_drift_a = std::pow(2.f, kRateOct * kRateTap[0] * w);
    const float rate_drift_b = std::pow(2.f, kRateOct * kRateTap[1] * w);
    a.set_shape_offset(kShapeMax * kShapeTap[0] * w);
    b.set_shape_offset(kShapeMax * kShapeTap[1] * w);
    pa.set_detune_cents(kTuneCents * kTuneTap[0] * w);
    pb.set_detune_cents(kTuneCents * kTuneTap[1] * w);

    // --- COUPLE multiplier (unity until Task 6) ---
    const float mult_a = 1.f;
    const float mult_b = 1.f;

    // single rate hook = COUPLE x DRIFT rate tap
    a.set_rate_scale(mult_a * rate_drift_a);
    b.set_rate_scale(mult_b * rate_drift_b);
}
```

Note the zero-effect guarantee: at `drift = 0`, `w = 0`, so `rate_drift = pow(2,0) = 1`, shape offset `= 0`, detune `= 0`; with `mult = 1` the rate hook is exactly `1`. `pow(x, 0.f)` returns exactly `1.0f`, so lane rates are bit-identical.

- [ ] **Step 4: Run to verify they pass**

Run: `source env.sh && cmake --build build && ./build/spky_tests --test-case="center drift*"`
Expected: PASS.
Run: `./build/spky_tests`
Expected: `Status: SUCCESS!`.

- [ ] **Step 5: Commit**

```bash
git add engine/center/center.cpp tests/test_center.cpp
git commit -m "$(cat <<'EOF'
feat(m4): Center DRIFT — OU weather walk + six correlated taps

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 6: Center COUPLE (Kuramoto PLL — convergence + phase pull)

Realize COUPLE entirely as rate modulation (no phase jumps). The effective master rate is pulled toward the geometric mean of the two base rates (convergence), and a `sin`-of-phase-error term pushes/pulls the momentary rates until the banks lock. SYNC banks anchor.

**Files:**
- Modify: `engine/center/center.cpp`
- Test: `tests/test_center.cpp` (append)

**Interfaces:**
- Consumes: `SuperModulator::pitch_phase/base_hz/sync_mode` (Task 2); the Task-5 `update()` with its unity COUPLE placeholder.
- Produces: `phase_err()` now returns the wrapped master-lane phase error; `rate_scale = clamp(conv × pull, ½, 2) × rate_drift`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_center.cpp` (`ticks_driven` advances both banks per sample and calls `update` at control rate, so real phases evolve):

```cpp
namespace {
void run_coupled(Rig& r, int samples) {
    int ctrl = 0;
    for (int i = 0; i < samples; ++i) {
        if (ctrl == 0) { r.c.update(r.a, r.b, r.pa, r.pb); ctrl = Center::kCtrlInterval; }
        --ctrl;
        r.a.process(); r.b.process();
    }
}
} // namespace

TEST_CASE("center couple: couple 1 locks two free banks and converges their rates") {
    Rig r; r.init(3u);
    r.a.set_sync_mode(SyncMode::Free); r.b.set_sync_mode(SyncMode::Free);
    r.a.set_rate(0.5f); r.b.set_rate(0.52f);
    r.c.set_couple(1.f);
    run_coupled(r, 48000 * 12);
    CHECK(std::fabs(r.c.phase_err()) < 0.03f);
    CHECK(r.a.master_hz() == doctest::Approx(r.b.master_hz()).epsilon(0.03));
}

TEST_CASE("center couple: couple 0 leaves both rate hooks at unity") {
    Rig r; r.init(3u);
    r.a.set_rate(0.4f); r.b.set_rate(0.7f);
    float ba = r.a.base_hz(), bb = r.b.base_hz();
    r.c.set_couple(0.f); r.c.set_drift(0.f);
    run_coupled(r, 48000);
    CHECK(r.a.master_hz() == doctest::Approx(ba));
    CHECK(r.b.master_hz() == doctest::Approx(bb));
}

TEST_CASE("center couple: a SYNC bank anchors, its rate is not scaled") {
    Rig r; r.init(3u);
    r.a.set_tempo_bpm(120.f); r.b.set_tempo_bpm(120.f);
    r.a.set_sync_mode(SyncMode::Sync); r.a.set_rate(0.625f);   // anchor: 2 Hz
    r.b.set_sync_mode(SyncMode::Free); r.b.set_rate(0.3f);
    float anchor = r.a.base_hz();
    r.c.set_couple(1.f);
    run_coupled(r, 48000 * 8);
    CHECK(r.a.master_hz() == doctest::Approx(anchor));
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `source env.sh && cmake --build build && ./build/spky_tests --test-case="center couple*" 2>&1 | tail -20`
Expected: the lock/anchor cases FAIL (rates never converge; `phase_err` stays 0 because it is not yet computed). The couple-0 case may already pass.

- [ ] **Step 3: Implement the Kuramoto PLL in `update()`**

Add a Kuramoto constant to the anonymous namespace:

```cpp
constexpr float kK           = 0.15f;   // Kuramoto phase-pull gain (tune by ear)
constexpr float kRateClampLo = 0.5f;
constexpr float kRateClampHi = 2.0f;
```

Replace the `// --- COUPLE multiplier (unity until Task 6) ---` block in `update()` with:

```cpp
    // --- COUPLE (Kuramoto PLL: convergence toward the geometric mean + phase pull) ---
    float dphi = a.pitch_phase() - b.pitch_phase();
    dphi -= std::floor(dphi + 0.5f);            // wrap to [-0.5, 0.5)
    _phase_err = dphi;

    const float fa = a.base_hz(), fb = b.base_hz();
    const bool a_free = a.sync_mode() == SyncMode::Free;
    const bool b_free = b.sync_mode() == SyncMode::Free;
    const bool mixed  = a_free != b_free;

    // convergence: FREE banks slide toward the geometric mean; SYNC banks anchor.
    const float conv_a = a_free ? std::pow(fb / fa, _couple * 0.5f) : 1.f;
    const float conv_b = b_free ? std::pow(fa / fb, _couple * 0.5f) : 1.f;

    // phase pull: opposite sign on the two banks; a SYNC bank in a MIXED pair
    // stays the pure anchor (no pull); when both SYNC only the phase pull acts.
    const float s = std::sin(TWO_PI * dphi);
    const float pull_a = (!a_free && mixed) ? 1.f : (1.f - _couple * kK * s);
    const float pull_b = (!b_free && mixed) ? 1.f : (1.f + _couple * kK * s);

    const float mult_a = clampf(conv_a * pull_a, kRateClampLo, kRateClampHi);
    const float mult_b = clampf(conv_b * pull_b, kRateClampLo, kRateClampHi);
```

(Delete the two placeholder `const float mult_a = 1.f; const float mult_b = 1.f;` lines — the final `a.set_rate_scale(mult_a * rate_drift_a);` writes stay.)

Zero-effect at `couple = 0`: `conv = pow(x, 0) = 1`, `pull = 1 - 0 = 1`, `clamp(1) = 1`. Combined with `rate_drift = 1` (drift 0) the hook is exactly `1`.

- [ ] **Step 4: Run to verify they pass**

Run: `source env.sh && cmake --build build && ./build/spky_tests --test-case="center couple*"`
Expected: PASS. If the lock case is marginal, the assumption note in the spec applies — nudge `kK` (0.1–0.25) and/or extend the run; the target is lock within 1–2 cycles at couple 1.
Run: `./build/spky_tests`
Expected: `Status: SUCCESS!`.

- [ ] **Step 5: Commit**

```bash
git add engine/center/center.cpp tests/test_center.cpp
git commit -m "$(cat <<'EOF'
feat(m4): Center COUPLE — Kuramoto PLL (convergence + phase pull, SYNC anchor)

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 7: SPOT & SETTLE end-to-end coverage

The SPOT kick (per-lane phase jump + decaying shape) and SETTLE (weather glide + lane re-center) were wired across Tasks 1 (`ModLane::kick`/`settle`), 4 (`Center::spot`/`settle` forwards) and 5 (weather glide in `_step_weather`). This task locks the two gestures in with end-to-end tests. **This is a verification checkpoint** — the tests should pass on first run against the existing implementation; if one is red, debug with superpowers:systematic-debugging before proceeding.

**Files:**
- Test: `tests/test_center.cpp` (append)

- [ ] **Step 1: Write the tests**

Append to `tests/test_center.cpp` (needs `#include "mod/lane.h"` — add it near the top if not already present):

```cpp
TEST_CASE("center spot: shape kick decays back within ~5 s (lane level)") {
    // Base shape 0.2 keeps base+kick inside the gentle sine->triangle->ramp
    // region, so the output difference tracks the (decaying) shape offset rather
    // than the discontinuous pulse edge near shape 0.85. The spec decay is
    // tau ~ 1.5 s, so at 5 s the offset is ~3.6% of the kick — assert a relative
    // fade, not an absolute floor (which would demand a much shorter tau).
    ModLane kicked; kicked.init(48000.f, 9u); kicked.set_rate_hz(2.f); kicked.set_shape(0.2f);
    ModLane clean;  clean.init(48000.f, 9u);  clean.set_rate_hz(2.f);  clean.set_shape(0.2f);
    kicked.kick(0.f, 0.35f);
    float early = 0.f;
    for (int i = 0; i < 4800; ++i) {                          // first 0.1 s: audible
        float d = std::fabs(kicked.process() - clean.process());
        if (d > early) early = d;
    }
    for (int i = 0; i < 48000 * 5; ++i) { kicked.process(); clean.process(); }   // wait 5 s
    float late = 0.f;
    for (int i = 0; i < 480; ++i) {
        float d = std::fabs(kicked.process() - clean.process());
        if (d > late) late = d;
    }
    CHECK(early > 0.01f);              // the lightning flashed
    CHECK(late < early * 0.15f);       // and faded to a small fraction within ~5 s
}

TEST_CASE("lane settle: accelerates the return of an open shape kick") {
    ModLane ref;  ref.init(48000.f, 3u);  ref.set_rate_hz(1.f);  ref.set_shape(0.5f);
    ModLane slow; slow.init(48000.f, 3u); slow.set_rate_hz(1.f); slow.set_shape(0.5f);
    ModLane fast; fast.init(48000.f, 3u); fast.set_rate_hz(1.f); fast.set_shape(0.5f);
    slow.kick(0.f, 0.3f);
    fast.kick(0.f, 0.3f);
    fast.settle();
    for (int i = 0; i < 24000; ++i) { ref.process(); slow.process(); fast.process(); }
    float dslow = std::fabs(slow.process() - ref.process());
    float dfast = std::fabs(fast.process() - ref.process());
    CHECK(dfast <= dslow);         // settle pulled the kick home faster
}

TEST_CASE("center settle: weather and drift glide to 0 within ~1.5 s") {
    Rig r; r.init(11u);
    r.c.set_drift(1.f);
    r.ticks(4000);
    CHECK(std::fabs(r.c.weather()) > 0.05f);
    r.c.settle(r.a, r.b);
    r.ticks(1500);                 // ~3 s at control rate
    CHECK(std::fabs(r.c.weather()) < 0.03f);
    CHECK(r.c.drift() < 0.05f);
}
```

- [ ] **Step 2: Build and run the new cases**

Run: `source env.sh && cmake --build build && ./build/spky_tests --test-case="center spot*,lane settle*,center settle*"`
Expected: PASS. (If `center spot` shows a nonzero `late`, the FLOW-mode lane must not be frozen — confirm `probability` default is 1 and step mode is off.)

- [ ] **Step 3: Full suite**

Run: `./build/spky_tests`
Expected: `Status: SUCCESS!`.

- [ ] **Step 4: Commit**

```bash
git add tests/test_center.cpp
git commit -m "$(cat <<'EOF'
test(m4): SPOT shape-kick decay + SETTLE weather/lane glide coverage

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 8: Instrument integration (own Center, control-rate update, morph mix)

Wire `Center` into `Instrument`: own one, run `update()` once per 96-sample control tick, apply the equal-power morph gains to each part's dry L/R **and** send L/R before the reverb sum, expose the API and getters, and set boot defaults (morph 0.5, couple 0, drift 0).

**Files:**
- Modify: `engine/instrument.h`
- Modify: `engine/instrument.cpp`
- Test: `tests/test_instrument.cpp` (append)

**Interfaces:**
- Consumes: all of `Center`'s public surface (Tasks 4–7); `Part::mod()` returning `SuperModulator&`.
- Produces (Task 9 relies on these): `Instrument::set_morph/set_couple/set_drift/spot/settle` and getters `morph() couple() drift() weather() phase_err()`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_instrument.cpp`:

```cpp
TEST_CASE("instrument M4: couple 0 + drift 0 -> PITCH lane matches a bare SuperModulator") {
    Instrument inst; inst.init(48000.f);
    inst.set_couple(0.f); inst.set_drift(0.f);
    inst.set_rate(PART_A, 0.5f);
    SuperModulator ref; ref.init(48000.f, 0x1234abcdu);   // PART_A seed (see instrument.cpp)
    ref.set_rate(0.5f);
    bool same = true;
    std::vector<float> l(1), r(1);
    for (int i = 0; i < 20000; ++i) {
        inst.process(nullptr, nullptr, l.data(), r.data(), 1);
        ref.process();
        if (inst.lane_output(PART_A, LANE_PITCH) != ref.lane_output(LANE_PITCH)) same = false;
    }
    CHECK(same);   // Center writes rate_scale=1 / shape_offset=0 -> zero perturbation
}

// Two tests: DRY isolation is exact (no reverb), SEND isolation is a decaying
// tail (with reverb). One combined test was wrong — at morph 1 the DRY path is
// gone immediately, but the shared reverb keeps ringing out the send injected
// during the 0.5->1 morph ramp, so an absolute "difference < 1e-5 within 1 s"
// contradicts the design ("only its already-committed tail rings out").
TEST_CASE("instrument M4: morph=1 isolates part A's dry path") {
    Instrument x; x.init(48000.f);                 // no reverb: a pure dry-isolation check
    Instrument y; y.init(48000.f);
    x.set_morph(1.f); y.set_morph(1.f);            // full B; part A must stop contributing
    x.set_rate(PART_A, 0.3f); x.set_target_base(PART_A, LANE_PITCH, 0.2f);
    y.set_rate(PART_A, 0.9f); y.set_target_base(PART_A, LANE_PITCH, 0.9f);   // A differs a lot
    float xl, xr, yl, yr, maxd = 0.f;
    for (int i = 0; i < 48000; ++i) {
        x.process(nullptr, nullptr, &xl, &xr, 1);
        y.process(nullptr, nullptr, &yl, &yr, 1);
        if (i > 16000) { float d = std::fabs(xl - yl); if (d > maxd) maxd = d; }  // after morph snaps to 1
    }
    CHECK(maxd < 1e-5f);   // A's dry contribution is gone (gain_a = cos(pi/2) ~ 0)
}

TEST_CASE("instrument M4: morph=1 injects no new reverb from part A (send isolated)") {
    static float echoX[PART_COUNT][2][Flux::kMaxSamples];
    static float echoY[PART_COUNT][2][Flux::kMaxSamples];
    static AmbientReverb rvX, rvY;
    FxMem mx, my;
    for (int p = 0; p < PART_COUNT; ++p)
        for (int c = 0; c < 2; ++c) { mx.echo[p][c] = echoX[p][c]; my.echo[p][c] = echoY[p][c]; }
    mx.reverb = &rvX; my.reverb = &rvY;
    Instrument x; x.init(48000.f, mx);
    Instrument y; y.init(48000.f, my);
    x.set_morph(1.f); y.set_morph(1.f);
    x.set_reverb_size(0.1f); y.set_reverb_size(0.1f);   // short tail so 3 s covers full decay
    x.set_rate(PART_A, 0.3f); x.set_target_base(PART_A, LANE_PITCH, 0.2f);
    y.set_rate(PART_A, 0.9f); y.set_target_base(PART_A, LANE_PITCH, 0.9f);
    float xl, xr, yl, yr, early = 0.f, late = 0.f;
    const int N = 48000 * 3;
    for (int i = 0; i < N; ++i) {
        x.process(nullptr, nullptr, &xl, &xr, 1);
        y.process(nullptr, nullptr, &yl, &yr, 1);
        float d = std::fabs(xl - yl);
        if (i < 24000)      { if (d > early) early = d; }   // first 0.5 s: morph ramp injects A
        if (i >= N - 24000) { if (d > late)  late  = d; }   // final 0.5 s: only a decayed tail
    }
    // No new A energy enters the shared reverb at morph 1 -> the divergence is a
    // decaying tail: the final window is far below the early transient, near zero.
    CHECK(early > 1e-4f);
    CHECK(late < early * 0.05f);
    CHECK(late < 1e-4f);
}
```
(Add `#include "mod/super_modulator.h"` at the top of `tests/test_instrument.cpp` if not present. `test_fx_mem()` is not reused here because two instruments need independent FX memory.)

- [ ] **Step 2: Run to verify they fail**

Run: `source env.sh && cmake --build build 2>&1 | tail -20`
Expected: FAIL to compile — `set_couple`, `set_morph` not members of `Instrument`.

- [ ] **Step 3: Edit `engine/instrument.h`**

Add the include near the top with the others:

```cpp
#include "center/center.h"
```

In `public:`, right after the M3 capture block (`bool loop_valid(int p) const ...`), add:

```cpp
    // --- M4 center section ---
    void set_morph(float m)  { _center.set_morph(m); }
    void set_couple(float c) { _center.set_couple(c); }
    void set_drift(float d)  { _center.set_drift(d); }
    void spot()   { _center.spot(_parts[PART_A].mod(),   _parts[PART_B].mod()); }
    void settle() { _center.settle(_parts[PART_A].mod(), _parts[PART_B].mod()); }
    float morph()     const { return _center.morph(); }
    float couple()    const { return _center.couple(); }
    float drift()     const { return _center.drift(); }
    float weather()   const { return _center.weather(); }
    float phase_err() const { return _center.phase_err(); }
```

In `private:`, next to `AmbientReverb* _reverb = nullptr;`, add:

```cpp
    Center _center;
    int    _ctrl_ctr = 0;    // counts down to the next control-rate Center::update
```

- [ ] **Step 4: Edit `engine/instrument.cpp`**

In the two-arg `init`, after `if (_reverb) _reverb->init(sample_rate);`, add:

```cpp
    _center.init(sample_rate, 0x5ce47e12u);
    _ctrl_ctr = 0;
```

Replace `Instrument::process` entirely with:

```cpp
void Instrument::process(const float* /*inL*/, const float* /*inR*/,
                         float* outL, float* outR, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (_ctrl_ctr == 0) {                 // control-rate center update (per 96 samples)
            _center.update(_parts[PART_A].mod(), _parts[PART_B].mod(),
                           _parts[PART_A], _parts[PART_B]);
            _ctrl_ctr = Center::kCtrlInterval;
        }
        --_ctrl_ctr;

        float al, ar, bl, br;
        float asl, asr, bsl, bsr;
        _parts[PART_A].process(al, ar, asl, asr);
        _parts[PART_B].process(bl, br, bsl, bsr);

        const float ga = _center.gain_a();
        const float gb = _center.gain_b();
        float l = al * ga + bl * gb;          // MORPH: equal-power A<->B blend
        float r = ar * ga + br * gb;
        if (_reverb) {
            // MORPH fades dry AND send together (M4 supersedes the M1.6
            // pre-morph-send rule): a fully morphed-away part injects no new
            // reverb; only its already-committed tail rings out.
            float wl, wr;
            _reverb->process(asl * ga + bsl * gb, asr * ga + bsr * gb, wl, wr);
            l += wl;
            r += wr;
        }
        outL[i] = l;
        outR[i] = r;
    }
}
```

- [ ] **Step 5: Run to verify they pass**

Run: `source env.sh && cmake --build build && ./build/spky_tests --test-case="instrument M4*"`
Expected: PASS.
Run: `./build/spky_tests`
Expected: `Status: SUCCESS!`. The pre-existing `all FX off + send 0 is bit-identical` and `boot reverb send is audible` cases still pass (both instruments share the morph-0.5 default, so their gains match).

- [ ] **Step 6: Commit**

```bash
git add engine/instrument.h engine/instrument.cpp tests/test_instrument.cpp
git commit -m "$(cat <<'EOF'
feat(m4): Instrument owns Center — control-rate update + equal-power morph mix

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 9: Render host — scenario actions + CSV columns

Expose the five center actions to JSON scenarios and add five global columns to `mods.csv`.

**Files:**
- Modify: `host/render/scenario.cpp`
- Modify: `host/render/main.cpp`
- Test: `tests/test_scenario.cpp` (append)

**Interfaces:**
- Consumes: `Instrument::set_morph/set_couple/set_drift/spot/settle` and getters (Task 8).
- Produces: scenario actions `set_morph`, `set_couple`, `set_drift`, `spot`, `settle`; CSV columns `morph,couple,drift,weather,phase_err`.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_scenario.cpp` (match its existing includes — it uses `render/scenario.h` and `instrument.h`):

```cpp
TEST_CASE("scenario: center actions dispatch to the instrument") {
    Instrument inst; inst.init(48000.f);

    Event ec; ec.action = "set_couple"; ec.value = 0.7f;
    apply_event(inst, ec);
    CHECK(inst.couple() == doctest::Approx(0.7f));     // couple is not smoothed

    Event ed; ed.action = "set_drift"; ed.value = 0.4f;
    apply_event(inst, ed);
    std::vector<float> l(1), r(1);
    for (int i = 0; i < 48000; ++i) inst.process(nullptr, nullptr, l.data(), r.data(), 1);
    CHECK(inst.drift() == doctest::Approx(0.4f).epsilon(0.05));   // smoothed toward target

    Event es; es.action = "spot";   apply_event(inst, es);   // must not crash
    Event et; et.action = "settle"; apply_event(inst, et);
    CHECK(true);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `source env.sh && cmake --build build && ./build/spky_tests --test-case="scenario: center actions*" 2>&1 | tail -20`
Expected: FAIL — `couple` stays 0 (unknown actions are ignored, so the setters never fire).

- [ ] **Step 3: Edit `host/render/scenario.cpp`**

In `apply_event`, just before the trailing `// unknown actions are ignored` comment, add:

```cpp
    else if (a == "set_morph")           inst.set_morph(e.value);
    else if (a == "set_couple")          inst.set_couple(e.value);
    else if (a == "set_drift")           inst.set_drift(e.value);
    else if (a == "spot")                inst.spot();
    else if (a == "settle")              inst.settle();
```

- [ ] **Step 4: Run to verify it passes**

Run: `source env.sh && cmake --build build && ./build/spky_tests --test-case="scenario: center actions*"`
Expected: PASS.

- [ ] **Step 5: Edit `host/render/main.cpp` — CSV header + row**

Extend the header `fprintf`: change the final `"...,b_v0,b_v1,b_v2,b_v3,b_cap\n"` to end with the five global columns:

```cpp
            "b_fx0,b_fx1,b_fx2,b_fx3,b_fx4,b_voices,b_v0,b_v1,b_v2,b_v3,b_cap,"
            "morph,couple,drift,weather,phase_err\n");
```

In the CSV row block, after the `for (int p = 0; p < 2; ++p) { ... }` loop and **before** `std::fprintf(csv, "\n");`, insert:

```cpp
            std::fprintf(csv, ",%.4f,%.4f,%.4f,%.4f,%.4f",
                         inst.morph(), inst.couple(), inst.drift(),
                         inst.weather(), inst.phase_err());
```

- [ ] **Step 6: Build the render host and confirm the header**

Run: `source env.sh && cmake --build build && ./build/render host/render/scenarios/flow_drone.json renders/_probe.wav renders/_probe.csv && head -1 renders/_probe.csv`
Expected: the header line ends with `...,b_cap,morph,couple,drift,weather,phase_err`. Clean up: `rm renders/_probe.wav renders/_probe.csv`.
Run: `./build/spky_tests`
Expected: `Status: SUCCESS!`.

- [ ] **Step 7: Commit**

```bash
git add host/render/scenario.cpp host/render/main.cpp tests/test_scenario.cpp
git commit -m "$(cat <<'EOF'
feat(m4): render host — center scenario actions + 5 global mods.csv columns

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 10: Demos, re-render, and doc/comment cleanup

Add the two showcase scenarios, render them (with a determinism check), and update the superseded M1.6 pre-morph-send wording and the roadmap.

**Files:**
- Create: `host/render/scenarios/couple_lock.json`
- Create: `host/render/scenarios/weather_spot.json`
- Modify: `engine/fx/reverb.h` (comment), `docs/roadmap.md`

- [ ] **Step 1: Create `host/render/scenarios/couple_lock.json`**

```json
{
  "sample_rate": 48000,
  "bpm": 120,
  "duration_s": 40,
  "init": [
    {"action":"set_engine","part":0,"value":"synth"},
    {"action":"set_engine","part":1,"value":"synth"},
    {"action":"set_sync_mode","part":0,"value":"free"},
    {"action":"set_sync_mode","part":1,"value":"free"},
    {"action":"set_rate","part":0,"value":0.45},
    {"action":"set_rate","part":1,"value":0.62},
    {"action":"set_target_active","part":0,"slot":2,"flag":true},
    {"action":"set_target_active","part":1,"slot":2,"flag":true},
    {"action":"set_morph","value":0.5}
  ],
  "events": [
    {"t":8.0,"action":"set_couple","value":0.4},
    {"t":18.0,"action":"set_couple","value":1.0},
    {"t":30.0,"action":"set_couple","value":0.0}
  ]
}
```

- [ ] **Step 2: Create `host/render/scenarios/weather_spot.json`**

```json
{
  "sample_rate": 48000,
  "bpm": 120,
  "duration_s": 30,
  "init": [
    {"action":"set_engine","part":0,"value":"synth"},
    {"action":"set_engine","part":1,"value":"synth"},
    {"action":"set_rate","part":0,"value":0.5},
    {"action":"set_rate","part":1,"value":0.5},
    {"action":"set_target_active","part":0,"slot":2,"flag":true},
    {"action":"set_target_active","part":1,"slot":2,"flag":true},
    {"action":"set_morph","value":0.5}
  ],
  "events": [
    {"t":2.0,"action":"set_drift","value":0.7},
    {"t":10.0,"action":"spot"},
    {"t":14.0,"action":"spot"},
    {"t":19.0,"action":"spot"},
    {"t":26.0,"action":"settle"}
  ]
}
```

- [ ] **Step 3: Render both and eyeball the CSV**

```bash
source env.sh && cmake --build build
./build/render host/render/scenarios/couple_lock.json renders/couple_lock.wav renders/couple_lock.csv
./build/render host/render/scenarios/weather_spot.json renders/weather_spot.wav renders/weather_spot.csv
```
Expected: `couple_lock.csv` `phase_err` ebbs at couple 0.4, converges to ~0 at couple 1 (both `a_*`/`b_*` master rates equal), and drifts apart again after 0. `weather_spot.csv` shows the `weather` column breathing at drift 0.7, per-lane phase jumps at each `spot`, and `weather` → 0 after `settle` at t=26.

- [ ] **Step 4: Verify the bit-determinism invariant**

```bash
./build/render host/render/scenarios/couple_lock.json renders/_a.wav renders/_a.csv
./build/render host/render/scenarios/couple_lock.json renders/_b.wav renders/_b.csv
cmp renders/_a.wav renders/_b.wav && echo "DETERMINISTIC" || echo "NON-DETERMINISTIC"
rm renders/_a.wav renders/_a.csv renders/_b.wav renders/_b.csv
```
Expected: `DETERMINISTIC`.

- [ ] **Step 5: Update the superseded M1.6 wording**

In `engine/fx/reverb.h`, the class comment says the reverb input is `(post-FX, pre-morph)`. Since M4 morphs the send, change `pre-morph` to reflect the new order — e.g.:

```cpp
// (post-FX; the send is morph-scaled in the Instrument mix) and joins the
// master AFTER the part mix as a wet-only signal.
```
(Keep the existing sentence structure; only correct the `pre-morph` claim.) Then grep for any other stale reference: `grep -rn "pre-morph\|haunt the room" engine/ docs/ tests/` — update prose hits, and if a test asserts the old "a part morphed away can still haunt the room" behavior, delete/replace it (none is expected).

- [ ] **Step 6: Mark M4 in `docs/roadmap.md`**

Change the M4 table row from `⬜ planned` to `✅ done`, and in the `### M4 — Center section` section note the landed behavior (COUPLE Kuramoto PLL, DRIFT OU weather with six taps, MORPH equal-power incl. send, SPOT per-lane kick, SETTLE panic; demos `couple_lock`, `weather_spot`). Match the wording style of the M2/M3 entries.

- [ ] **Step 7: Full suite + final build**

Run: `source env.sh && cmake --build build && ./build/spky_tests`
Expected: `Status: SUCCESS!` (all pre-existing + new cases green).

- [ ] **Step 8: Commit**

```bash
git add host/render/scenarios/couple_lock.json host/render/scenarios/weather_spot.json \
        renders/couple_lock.wav renders/couple_lock.csv \
        renders/weather_spot.wav renders/weather_spot.csv \
        engine/fx/reverb.h docs/roadmap.md
git commit -m "$(cat <<'EOF'
feat(m4): couple_lock + weather_spot demos; supersede M1.6 pre-morph-send wording

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

## Self-Review — spec coverage

_Checked by the plan author against `2026-07-12-spotykach-center-section-design.md`._

| Spec requirement | Task |
|---|---|
| COUPLE = Kuramoto PLL, convergence + phase pull, no phase jumps, SYNC anchor, ×0.5..×2 clamp | 6 |
| DRIFT = one OU weather walk (τ≈45 s, bounded), six hardcoded taps, smoothed `set_drift` | 5 |
| MORPH equal-power gains on dry **and** send; boot 0.5; smoothed | 4, 8 |
| SPOT per-lane phase kick (±½, permanent) + shape kick (±0.35, τ≈1.5 s decay); replay-immune | 1, 2, 7 |
| SETTLE: DRIFT+weather glide to 0, EVOLVE re-center, open kicks decay; COUPLE/MORPH untouched | 1, 4, 5, 7 |
| One `Center` class + narrow hooks (`rate_scale`, `shape_offset`, `kick`, `detune_cents`); control rate | 4–8 |
| `ModLane` hooks | 1 |
| `SuperModulator` hooks + getters | 2 |
| `Part::set_detune_cents` post-quantizer, `pitch_cv()` clean | 3 |
| `Instrument` API + delegation + morph mix + control-rate `update` | 8 |
| Render host: 5 actions + 5 CSV columns | 9 |
| `couple_lock` + `weather_spot` demos | 10 |
| Zero-effect invariant (couple 0 + drift 0 bit-identical; mix level change called out) | 2, 5, 6, 8 |
| Bit-determinism invariant | 8, 10 |
| Supersede M1.6 pre-morph-send comment/test | 8, 10 |

**Tuning notes carried from the spec (verify by ear during Task 6/10):** Kuramoto `kK` (lock in 1–2 cycles at couple 1, no wobble at low couple); DRIFT tap scales (½ oct / 0.15 / 25 cents); OU τ≈45 s; the ±½-cycle phase kick on the LEVEL lane may be too brutal (candidate for per-lane scaling — YAGNI unless it bites).

**Post-M4 by-ear refinements (2026-07-12, after the whole-branch review):**
- **SPOT skips the PITCH master lane entirely** (`SuperModulator::spot`): the ±½-cycle phase kick threw a live melody around. Pitch is now immune to the stumble (live or replaying); the other four lanes still lurch. Test `super: spot stumbles every lane except the PITCH master lane`. Spec + dev diary updated.
- **`couple_lock` / `weather_spot` demos retuned:** both drove the PITCH lane full-range (default `range` 1.0 → 3-octave siren). Now stepped pentatonic melodies in a moderate *unipolar* range (`apply_range` gives `[0, 2·range]`, so PITCH `base` is kept low to avoid top-note pinning); `weather_spot` activates TIMBRE/FILTER/MOTION so SPOT has something to stumble, with taps off the step grid. Verified: pitch column is byte-identical with vs without SPOT; timbre/filter differ.


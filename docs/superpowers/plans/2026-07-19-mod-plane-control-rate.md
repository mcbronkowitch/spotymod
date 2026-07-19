# Mod Plane Control Rate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The four texture lanes per part advance on the 96-sample control raster via a new `ModLane::tick()`; the PITCH lane stays per-sample, keeping every fire bit-identical. Expected saving ~17–19 % of the block budget.

**Architecture:** `ModLane` gains a second advance path `tick()` that replays every step/wrap boundary inside a 96-sample interval in phase order (identical RNG draws and sequencer state), then emits one smoothed output. `SuperModulator::process()` keeps calling the PITCH lane per sample and ticks the texture lanes on an internal counter that fires on the first call — phase-aligned with `Part::_control_tick()`, which consumes the values on the same samples. Both paths stay in the binary permanently; a per-sample-vs-tick equivalence suite pins them against each other.

**Tech Stack:** C++17 engine (`namespace spky`, no heap/libDaisy), doctest, desktop build via `source env.sh` (clang+ninja), render host for determinism gates.

**Spec:** `docs/superpowers/specs/2026-07-19-mod-plane-control-rate-design.md` (committed `9dd3115`). Read it before starting.

## Global Constraints

- Branch: `cpu-hunt`. Shared-history discipline: stage **only explicit paths** (never `git add -A` / `-u`).
- Build: `source env.sh && cmake --build build` (from repo root, bash). Tests: `./build/spky_tests`. Render: `./build/render <scenario.json> [out.wav] [mods.csv]`.
- Commit trailer (both lines, every commit):
  `Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>`
  `Claude-Session: https://claude.ai/code/session_01T727NNiydTvBtBBxpZ3oDG`
- **Hardest gate (spec §Testing 4): no existing PITCH / groove / CHOKE / center test may be edited.** If a melodic-path test needs touching, STOP and report — the design is violated.
- Engine purity: no heap, no `<random>`, no libDaisy includes in `engine/`.
- The mod layer must not include synth headers; the raster alignment is pinned by a `static_assert` in `part.cpp` only.

---

### Task 1: Extract `_wrap_events()` + `kTickInterval` constant (behavior-frozen refactor)

**Files:**
- Modify: `engine/mod/lane.h`
- Modify: `engine/mod/lane.cpp`
- Modify: `engine/parts/part.cpp`

**Interfaces:**
- Produces: `ModLane::kTickInterval` (public `static constexpr int` = 96), private `void ModLane::_wrap_events()`. Task 2 calls both.
- This task changes **zero behavior**. The gate is byte-identity.

- [ ] **Step 1: Add the constant and the private method to `lane.h`**

In the `public:` section of `class ModLane` (after the constructor-less top, e.g. right below `void init(...)`):

```cpp
    // Control-raster interval of the tick() path (spec 2026-07-19
    // mod-plane-control-rate). part.cpp static_asserts this against
    // SynthEngine::kCtrlInterval -- the mod layer must not include synth.
    static constexpr int kTickInterval = 96;
```

In the `private:` section, next to `void _on_boundary();`:

```cpp
    void  _wrap_events();           // regen/EVOLVE/groove events at a cycle wrap
```

- [ ] **Step 2: Extract the wrap block in `lane.cpp`**

In `ModLane::process()`, the entire `if (wrapped) { ... }` body (the block containing `_regen_pending`, the GROW `_ev_*` walk, the RENEW branch and both `_mutate_groove` calls) moves verbatim into a new method; `process()` keeps only the call:

```cpp
// Cycle-wrap events, shared by process() and tick(): pending phrase regen,
// the EVOLVE walk (GROW) or walk decay + per-unit regen (RENEW), and the
// outer-zone groove mutations. Order is load-bearing and identical to the
// old inline block.
void ModLane::_wrap_events() {
    if (_regen_pending && _melodic && _step_mode) {
        generate_phrase(_principle, _rng, _steps, _seq, _gate, _motif_id, _layout);
        pg_gen_groove(_rng, _layout.motif_len, _groove);
        _regen_pending = false;
        _ev_phase = _ev_shape = _ev_rate = 0.f; // present fresh phrase un-warped
    }
    if (_variation > 0.f) {                 // GROW: EVOLVE contour walk (live)
        _ev_phase = clampf(_ev_phase + _rng.next_bipolar() * 0.01f * _variation, -0.5f, 0.5f);
        _ev_shape = clampf(_ev_shape + _rng.next_bipolar() * 0.02f * _variation, -0.25f, 0.25f);
        _ev_rate  = clampf(_ev_rate  + _rng.next_bipolar() * 0.01f * _variation, -0.2f, 0.2f);
        _mutate_groove(false);              // outer zone: rhythm drifts too
    } else if (_variation < 0.f) {          // RENEW: per-unit regen + walk decay
        if (_melodic && _step_mode) _renew_units();
        else if (!_melodic) {
            if (_rng.next_unipolar() < _variation * _variation) _renew_walk();
        }
        float decay = 1.f + 0.2f * _variation;  // variation -1 -> x0.8/cycle
        _ev_phase *= decay; _ev_shape *= decay; _ev_rate *= decay;
        _mutate_groove(true);               // outer zone: re-decide pushes
    }                                       // variation 0 (LOOP): walk frozen
}
```

and in `process()`:

```cpp
    if (wrapped) _wrap_events();
```

(The comments move with the code; delete the now-empty inline block.)

- [ ] **Step 3: Pin the raster alignment in `part.cpp`**

At the top of `engine/parts/part.cpp`, after the `using namespace spky;` line:

```cpp
// The mod tick must ride the same raster the engine control tick uses --
// Part::_control_tick() reads texture lane outputs the sample they are
// produced (spec 2026-07-19 mod-plane-control-rate).
static_assert(ModLane::kTickInterval == SynthEngine::kCtrlInterval,
              "mod tick interval must equal the engine control raster");
```

- [ ] **Step 4: Build and run the full suite**

Run: `source env.sh && cmake --build build && ./build/spky_tests`
Expected: build clean, **319/319** test cases pass (0 failed).

- [ ] **Step 5: Byte-identity gate (refactor proof)**

```bash
./build/render host/render/scenarios/demo_step_melody.json /tmp/t1_a.wav /tmp/t1_a.csv
git stash            # stash the refactor
cmake --build build
./build/render host/render/scenarios/demo_step_melody.json /tmp/t1_ref.wav /tmp/t1_ref.csv
git stash pop
cmake --build build
cmp /tmp/t1_a.wav /tmp/t1_ref.wav && cmp /tmp/t1_a.csv /tmp/t1_ref.csv && echo IDENTICAL
```

Expected: `IDENTICAL`. (If `git stash` is risky in your environment because of concurrent edits, an acceptable substitute is a double render of the SAME build — but the stash A/B is the real refactor proof; prefer it. The tree is expected clean apart from this task's edits.)

- [ ] **Step 6: Commit**

```bash
git add engine/mod/lane.h engine/mod/lane.cpp engine/parts/part.cpp
git commit -m "refactor(lane): extract _wrap_events, pin the mod tick interval"
```

(with the two trailer lines from Global Constraints).

---

### Task 2: `ModLane::tick()` + tick-rate coefficients + core equivalence tests

**Files:**
- Modify: `engine/util/onepole.h`
- Modify: `engine/mod/lane.h`
- Modify: `engine/mod/lane.cpp`
- Create: `tests/test_lane_tick.cpp`
- Modify: `CMakeLists.txt` (add the test file to the explicit test-source list next to `tests/test_lane.cpp`)

**Interfaces:**
- Consumes: `ModLane::kTickInterval`, `_wrap_events()` (Task 1).
- Produces: `float ModLane::tick()` — advances exactly `kTickInterval` samples, returns the post-range output. `OnePole::set_coef(float k)`. Task 4 calls `tick()` from `SuperModulator`.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_lane_tick.cpp`:

```cpp
#include <doctest/doctest.h>
#include <cmath>
#include "mod/lane.h"
using namespace spky;

// Per-sample-vs-tick equivalence harness (spec 2026-07-19 mod-plane-control-
// rate, "Testing 1"). ref is driven by 96x process(), dut by one tick();
// both start from identical seed + config, so their private RNG streams are
// the same stream. Any skipped boundary or reordered draw desyncs the
// streams and explodes the target comparison within a few cycles.
namespace {
constexpr float kSr   = 48000.f;
constexpr int   kTick = ModLane::kTickInterval;

struct TickPair {
    ModLane ref, dut;
    float ref_out = 0.f, dut_out = 0.f;
    int   ref_fires = 0;
    bool  dut_fired = false;

    void boot(uint32_t seed, void (*cfg)(ModLane&)) {
        ref.init(kSr, seed); cfg(ref);
        dut.init(kSr, seed); cfg(dut);
    }
    void advance_one_tick() {
        ref_fires = 0;
        for (int i = 0; i < kTick; ++i) {
            ref_out = ref.process();
            if (ref.fired()) ++ref_fires;
        }
        dut_out = dut.tick();
        dut_fired = dut.fired();
    }
};
} // namespace

TEST_CASE("tick: STEP S&H targets and fires match the per-sample path exactly") {
    // shape 1.0 returns the S&H operand EXACTLY (entropy-sequencer fix), so
    // the boundary target is phase-independent: bit-equal across both paths.
    TickPair tp;
    tp.boot(42u, [](ModLane& l) {
        l.set_range(1.f); l.set_shape(1.f); l.set_smooth(0.f);
        l.set_step(true, 8); l.set_rate_hz(2.3f);   // boundary every ~2609 smp
    });
    // One caveat: the two paths accumulate phase differently (96 rounded
    // adds vs one fused product), so a boundary landing within float-eps of
    // a tick edge can be detected one tick apart. That skew self-corrects on
    // the next tick and skips no RNG draw; the guard below tolerates exactly
    // that -- a real RNG desync would never re-converge and still fails.
    // Each straddle shows as TWO adjacent parity mismatches (early window +
    // missing next window), hence the doubled skew_events budget.
    int skew = 0, skew_events = 0;
    for (int t = 0; t < 400; ++t) {
        tp.advance_one_tick();
        if ((tp.ref_fires > 0) != tp.dut_fired) { skew = 1; ++skew_events; continue; }
        if (skew > 0) { --skew; continue; }
        CHECK(tp.dut.target() == tp.ref.target());
        CHECK(tp.dut_out == tp.ref_out);            // smooth 0 = passthrough
    }
    CHECK(skew_events <= 4);   // isolated float coincidences, never systematic
}

TEST_CASE("tick: GROW mutation dice stay on the same RNG stream") {
    // variation > 0 draws dice + walk deltas per boundary/wrap. 300 ticks
    // (~7 cycles) of exact target equality proves no draw was skipped.
    TickPair tp;
    tp.boot(7u, [](ModLane& l) {
        l.set_range(1.f); l.set_shape(1.f); l.set_smooth(0.f);
        l.set_step(true, 8); l.set_rate_hz(3.7f);
        l.set_variation(0.7f);
    });
    // Same tick-edge skew guard as the S&H case: seed 7 / 3.7 Hz hits one
    // straddle (~tick 250). The draw is delayed one tick, never skipped;
    // exact equality must resume immediately after.
    int skew = 0, skew_events = 0;
    for (int t = 0; t < 300; ++t) {
        tp.advance_one_tick();
        if ((tp.ref_fires > 0) != tp.dut_fired) { skew = 1; ++skew_events; continue; }
        if (skew > 0) { --skew; continue; }
        CHECK(tp.dut.target() == tp.ref.target());
    }
    CHECK(skew_events <= 4);
}

TEST_CASE("tick: RENEW walk regen stays on the same RNG stream") {
    TickPair tp;
    tp.boot(11u, [](ModLane& l) {
        l.set_range(1.f); l.set_shape(1.f); l.set_smooth(0.f);
        l.set_step(true, 8); l.set_rate_hz(3.1f);
        l.set_variation(-0.8f);
    });
    int skew = 0, skew_events = 0;
    for (int t = 0; t < 300; ++t) {
        tp.advance_one_tick();
        if ((tp.ref_fires > 0) != tp.dut_fired) { skew = 1; ++skew_events; continue; }
        if (skew > 0) { --skew; continue; }
        CHECK(tp.dut.target() == tp.ref.target());
    }
    CHECK(skew_events <= 4);
}

TEST_CASE("tick: FLOW output tracks the per-sample path") {
    // Continuous FLOW: same end phase modulo float accumulation -- the tick
    // path adds one fused product where the reference adds 96 rounded
    // increments. Loose epsilon, wrap-fire parity exact.
    TickPair tp;
    tp.boot(3u, [](ModLane& l) {
        l.set_range(1.f); l.set_shape(0.3f); l.set_smooth(0.f);
        l.set_rate_hz(1.9f);
    });
    for (int t = 0; t < 500; ++t) {
        tp.advance_one_tick();
        CHECK((tp.ref_fires > 0) == tp.dut_fired);
        CHECK(tp.dut_out == doctest::Approx(tp.ref_out).epsilon(0.01));
    }
}

TEST_CASE("tick: SMOOTH slew matches outside a post-boundary blackout") {
    // The tick coefficient is the exact 96-sample compound of the per-sample
    // coefficient, so held segments converge identically. A boundary lands
    // mid-interval for the reference but takes effect at the tick edge for
    // the dut -- allow a 2-tick blackout after each fire, then require the
    // paths to agree again.
    TickPair tp;
    tp.boot(9u, [](ModLane& l) {
        l.set_range(1.f); l.set_shape(1.f); l.set_smooth(0.5f);
        l.set_step(true, 8); l.set_rate_hz(2.3f);
    });
    int blackout = 2;
    for (int t = 0; t < 400; ++t) {
        tp.advance_one_tick();
        if (tp.ref_fires > 0 || tp.dut_fired) { blackout = 2; continue; }
        if (blackout > 0) { --blackout; continue; }
        CHECK(tp.dut_out == doctest::Approx(tp.ref_out).epsilon(0.02));
    }
}
```

Add `tests/test_lane_tick.cpp` to the test-source list in `CMakeLists.txt` (directly after the `tests/test_lane.cpp` line).

- [ ] **Step 2: Build to verify the tests fail**

Run: `source env.sh && cmake --build build`
Expected: **compile error** — `ModLane` has no member `tick`.

- [ ] **Step 3: Implement**

`engine/util/onepole.h` — add below `init(...)`:

```cpp
    // Direct coefficient override -- used by ModLane's tick-rate slew twin,
    // whose exact coefficient (1 - (1-k)^N) has no time_s equivalent.
    void set_coef(float k) { _kof = k < 0.f ? 0.f : (k > 1.f ? 1.f : k); }
```

`engine/mod/lane.h` — in `public:`, after `float process();`:

```cpp
    float tick();                     // advance kTickInterval samples in one call
```

in `private:`, next to the existing `OnePole _slew;` and the kick/settle members:

```cpp
    OnePole _slew_tick;          // tick-rate twin of _slew; a lane is driven by
                                 // exactly ONE path, so the twin's state never
                                 // fights the per-sample instance
    float _kick_coef_tick   = 1.f;   // _kick_coef ^ kTickInterval
    float _settle_coef_tick = 1.f;   // _settle_coef ^ kTickInterval
```

`engine/mod/lane.cpp`:

In `init()`, directly after the `_kick_coef` / `_settle_coef` assignments:

```cpp
    _kick_coef_tick   = std::pow(_kick_coef,   static_cast<float>(kTickInterval));
    _settle_coef_tick = std::pow(_settle_coef, static_cast<float>(kTickInterval));
```

and change the final line `_slew.reset(0.f);` to:

```cpp
    _slew.reset(0.f);
    _slew_tick.reset(0.f);
```

In `_update_slew()` (replace the whole body):

```cpp
void ModLane::_update_slew() {
    // smooth 0 -> ~1 sample (near passthrough), smooth 1 -> ~0.5 s.
    float t = _fixed_slew ? 0.02f : (0.00002f * std::pow(25000.f, _smooth));
    _slew.init(_sr, t);
    // Tick twin: the exact kTickInterval-sample compound of the per-sample
    // coefficient, so held segments converge identically at tick sampling.
    float k = 1.f / (t * _sr);
    if (k > 1.f) k = 1.f;
    _slew_tick.set_coef(1.f - std::pow(1.f - k, static_cast<float>(kTickInterval)));
}
```

In `reset()`, change `_slew.reset(_target);` to:

```cpp
    _slew.reset(_target);
    _slew_tick.reset(_target);
```

New method (place directly below `process()`):

```cpp
// Advance exactly kTickInterval samples in one call -- the texture-lane path
// (spec 2026-07-19 mod-plane-control-rate). Mirrors process()'s observable
// sequence: every edge (step boundary or wrap) inside the interval runs in
// phase order with identical RNG draws, note aging and mutations; wrap
// events run at their phase position (before the new cycle's step 0); only
// the last target is visible. Boundary targets are evaluated at the grid
// phase (step/steps, resp. 0 at a wrap) instead of the per-sample path's
// detection overshoot (< 1 sample of phase) -- an equally valid sampling of
// the same waveform, covered by the equivalence suite.
float ModLane::tick() {
    _fired = false;
    _kick_shape *= _kick_coef_tick;
    if (_settle_ctr > 0) {
        _settle_ctr = _settle_ctr > kTickInterval ? _settle_ctr - kTickInterval : 0;
        _ev_phase   *= _settle_coef_tick;
        _ev_shape   *= _settle_coef_tick;
        _ev_rate    *= _settle_coef_tick;
        _kick_shape *= _settle_coef_tick;
    }

    // Pending step mismatch first: init/reset leave _cur_step = -1 and the
    // per-sample path fires step 0 on its very first sample the same way.
    if (_step_mode) {
        int step = static_cast<int>(_phase * static_cast<float>(_steps));
        if (step >= _steps) step = _steps - 1;
        if (step != _cur_step) { _cur_step = step; _on_boundary(); }
    }

    // Walk every edge inside the interval, in order. Panel-reachable worst
    // case is ~8 edges (480 Hz effective STEP rate, ~12.5 samples/step); the
    // cap is a safety bound, unreachable from the panel (spec: 2*kSeqSlots).
    float samples_left = static_cast<float>(kTickInterval);
    int guard = 2 * kSeqSlots;
    while (guard-- > 0) {
        // _ev_rate can change at a wrap (GROW walk), so the per-sample rate
        // is re-derived per edge -- the per-sample path does the same.
        const float dp1 = _phase_inc * (1.f + _ev_rate);
        const float next_edge = _step_mode
            ? static_cast<float>(_cur_step + 1) / static_cast<float>(_steps)
            : 1.f;
        const float dist = next_edge - _phase;
        const float to_edge = dp1 > 0.f ? dist / dp1 : 1e30f;
        if (to_edge > samples_left) { _phase += samples_left * dp1; break; }
        samples_left -= to_edge;
        if (next_edge >= 1.f) {
            _phase = 0.f;
            _wrap_events();
            if (_step_mode) _cur_step = 0;
            _on_boundary();              // FLOW fires per wrap; STEP fires step 0
        } else {
            _phase = next_edge;
            ++_cur_step;
            _on_boundary();
        }
    }

    if (!_step_mode && !_frozen) _target = _compute_raw();   // continuous FLOW

    float smoothed = _slew_tick.process(_target);
    return apply_range(smoothed, _range);
}
```

- [ ] **Step 4: Build and run the new tests**

Run: `source env.sh && cmake --build build && ./build/spky_tests -tc="tick:*"`
Expected: all 5 new cases PASS.

- [ ] **Step 5: Run the full suite**

Run: `./build/spky_tests`
Expected: 324/324 pass (319 + 5 new), 0 failed. Existing tests are untouched by construction (only new code paths).

- [ ] **Step 6: Commit**

```bash
git add engine/util/onepole.h engine/mod/lane.h engine/mod/lane.cpp tests/test_lane_tick.cpp CMakeLists.txt
git commit -m "feat(lane): tick() -- one call advances the 96-sample raster"
```

---

### Task 3: Edge-case battery — multi-boundary, wrap ordering, kick/settle

**Files:**
- Modify: `tests/test_lane_tick.cpp`
- Modify (only if a test exposes a real defect): `engine/mod/lane.cpp`

**Interfaces:**
- Consumes: `ModLane::tick()`, the `TickPair` harness (Task 2).
- Produces: nothing new — hardening only.

- [ ] **Step 1: Append the edge-case tests**

Append to `tests/test_lane_tick.cpp`:

```cpp
TEST_CASE("tick: multiple boundaries inside one interval are replayed in order") {
    // 500 Hz at 8 steps = one boundary every 12 samples = 8 per tick. With
    // GROW dice active, a single skipped or reordered boundary desyncs the
    // RNG stream and the exact target comparison fails within a few ticks.
    TickPair tp;
    tp.boot(21u, [](ModLane& l) {
        l.set_range(1.f); l.set_shape(1.f); l.set_smooth(0.f);
        l.set_step(true, 8); l.set_rate_hz(500.f);
        l.set_variation(0.6f);
    });
    // A boundary landing within float-eps of a tick edge shifts that one
    // boundary into the neighbouring window: the straddle tick compares
    // different "last boundary" targets, then equality resumes. Tolerate
    // isolated straddle ticks, never sustained divergence -- a skipped or
    // reordered boundary desyncs the RNG stream permanently and blows the
    // mismatch budget.
    int mismatch = 0;
    for (int t = 0; t < 200; ++t) {
        tp.advance_one_tick();
        if (tp.dut.target() != tp.ref.target()) { ++mismatch; continue; }
    }
    CHECK(mismatch <= 2);                          // isolated straddles only
    CHECK(tp.dut.target() == tp.ref.target());     // re-converged at the end
}

TEST_CASE("tick: wrap events land before the new cycle's step 0") {
    // variation -1 makes the RENEW walk-regen dice certain (v^2 = 1), so the
    // whole _seq walk regenerates at EVERY wrap. Step 0's target right after
    // the seam must sample the NEW walk -- if tick() ran the step-0 boundary
    // before _wrap_events(), it would sample the old walk and diverge from
    // the per-sample reference immediately.
    TickPair tp;
    tp.boot(33u, [](ModLane& l) {
        l.set_range(1.f); l.set_shape(1.f); l.set_smooth(0.f);
        l.set_step(true, 8); l.set_rate_hz(4.3f);
        l.set_variation(-1.f);
    });
    int skew = 0, skew_events = 0;
    for (int t = 0; t < 300; ++t) {
        tp.advance_one_tick();
        if ((tp.ref_fires > 0) != tp.dut_fired) { skew = 1; ++skew_events; continue; }
        if (skew > 0) { --skew; continue; }
        CHECK(tp.dut.target() == tp.ref.target());
    }
    CHECK(skew_events <= 4);
}

TEST_CASE("tick: SPOT kick equivalence at tick granularity") {
    // Kick applied to both paths at a tick edge (the only place Center can
    // apply it in production -- SPOT runs on the control tick).
    TickPair tp;
    tp.boot(5u, [](ModLane& l) {
        l.set_range(1.f); l.set_shape(0.4f); l.set_smooth(0.f);
        l.set_step(true, 8); l.set_rate_hz(2.9f);
    });
    for (int t = 0; t < 50; ++t) tp.advance_one_tick();
    tp.ref.kick(0.3f, 0.2f);
    tp.dut.kick(0.3f, 0.2f);
    int blackout = 0;
    for (int t = 0; t < 400; ++t) {
        tp.advance_one_tick();
        if (tp.ref_fires > 0 || tp.dut_fired) { blackout = 2; continue; }
        if (blackout > 0) { --blackout; continue; }
        // shape 0.4 is phase-dependent: boundary targets differ by the
        // detection-overshoot phase (< 1 sample) -- loose but real bound.
        CHECK(tp.dut_out == doctest::Approx(tp.ref_out).epsilon(0.05));
    }
}

TEST_CASE("tick: SETTLE glides the audible phase the same way") {
    // Build up EVOLVE walks first (variation > 0), then settle both paths and
    // compare the audible phase while the glide runs (tau 0.3 s, ctr 1 s).
    TickPair tp;
    tp.boot(13u, [](ModLane& l) {
        l.set_range(1.f); l.set_shape(0.5f); l.set_smooth(0.f);
        l.set_rate_hz(2.f);
        l.set_variation(0.8f);
    });
    for (int t = 0; t < 1500; ++t) tp.advance_one_tick();   // ~3 s of walk
    tp.ref.settle();
    tp.dut.settle();
    for (int t = 0; t < 600; ++t) {                          // ~1.2 s glide
        tp.advance_one_tick();
        // circular distance: phases straddling the 1.0 wrap must not read
        // as a full-cycle disagreement (0.999 vs 0.001 is 0.002 apart)
        float d = std::fabs(tp.dut.phase_eff() - tp.ref.phase_eff());
        if (d > 0.5f) d = 1.f - d;
        CHECK(d < 0.01f);
    }
}
```

- [ ] **Step 2: Build and run the battery**

Run: `source env.sh && cmake --build build && ./build/spky_tests -tc="tick:*"`
Expected: all 9 `tick:` cases PASS. If a case fails, the defect is in `tick()` (Task 2's implementation) — fix `engine/mod/lane.cpp`, never weaken an exact (`==`) assertion to an epsilon without reporting why.

- [ ] **Step 3: Full suite**

Run: `./build/spky_tests`
Expected: 328/328 pass, 0 failed.

- [ ] **Step 4: Commit**

```bash
git add tests/test_lane_tick.cpp
git commit -m "test(lane): tick edge battery -- multi-boundary, wrap order, kick/settle"
```

(add `engine/mod/lane.cpp` to the `git add` only if a fix was needed; say so in the commit body.)

---

### Task 4: SuperModulator drives the texture lanes at tick rate

**Files:**
- Modify: `engine/mod/super_modulator.h`
- Modify: `engine/mod/super_modulator.cpp`
- Modify: `tests/test_super_modulator.cpp` (append one new case only)

**Interfaces:**
- Consumes: `ModLane::tick()` (Task 2).
- Produces: the production behavior change — `SuperModulator::process()` per-sample cost drops to one PITCH lane + a counter.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_super_modulator.cpp`:

```cpp
TEST_CASE("super: texture lanes hold between control ticks, pitch stays per-sample") {
    SuperModulator m;
    m.init(48000.f, 42u);
    m.set_rate(0.6f);
    m.set_shape(0.3f);          // continuous FLOW: per-sample path would move
    m.set_smooth(0.f);
    m.process();                // counter boots at 0: the first call ticks
    float held[LANE_COUNT];
    for (int s = 0; s < LANE_COUNT; ++s) held[s] = m.lane_output(s);
    bool stair_ok = true;
    for (int i = 1; i < 96 * 20; ++i) {
        m.process();
        for (int s = 0; s < LANE_COUNT; ++s) {
            if (s == LANE_PITCH) continue;
            if (i % 96 == 0) held[s] = m.lane_output(s);
            else if (m.lane_output(s) != held[s]) stair_ok = false;
        }
    }
    CHECK(stair_ok);            // texture = 96-sample staircase by construction
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `source env.sh && cmake --build build && ./build/spky_tests -tc="*hold between control ticks*"`
Expected: FAIL (`stair_ok` false — texture lanes still move per sample).

- [ ] **Step 3: Implement**

`engine/mod/super_modulator.h` — add to the private members (below `_tide_mult`):

```cpp
    int      _tick_ctr = 0;        // texture-lane raster; 0 = tick on next process()
```

`engine/mod/super_modulator.cpp`:

In `init()`, after the lane loop: `_tick_ctr = 0;`

Replace `process()`:

```cpp
void SuperModulator::process() {
    // The PITCH lane is the anchor: per-sample, fires bit-identical to the
    // pre-rework engine. The four texture lanes advance on the 96-sample
    // raster (spec 2026-07-19 mod-plane-control-rate); the counter boots at
    // 0 so the first call ticks, which lands the mod tick on the same
    // samples as Part::_control_tick() -- the sole audio-path consumer reads
    // values that are 0 samples old.
    _out[LANE_PITCH] = _lanes[LANE_PITCH].process();
    if (_tick_ctr == 0) {
        _tick_ctr = ModLane::kTickInterval;
        for (int i = 0; i < LANE_COUNT; ++i)
            if (i != LANE_PITCH) _out[i] = _lanes[i].tick();
    }
    --_tick_ctr;
}
```

- [ ] **Step 4: Run the new test**

Run: `cmake --build build && ./build/spky_tests -tc="*hold between control ticks*"`
Expected: PASS.

- [ ] **Step 5: Full suite — the pitch-untouched gate**

Run: `./build/spky_tests`
Expected: **329/329 pass.** Analysis says the existing suite survives: every texture-observing test compares two instances that both run the new path (`test_mod_tide.cpp:38/134/155/173`, `test_super_modulator.cpp:98`), and `test_super_modulator.cpp:75` only needs pitch and SOURCE to *differ* somewhere. If ANY test fails:
  - a PITCH / groove / CHOKE / center test failing = **STOP, report** (spec gate — design violated, do not patch the test);
  - a texture-granularity assertion failing = adapt ONLY that assertion to read on the 96-sample grid (sample the compared values at `i % 96 == 0`), and say so in the commit body.

- [ ] **Step 6: Determinism gate**

```bash
./build/render host/render/scenarios/demo_step_melody.json /tmp/t4_a.wav /tmp/t4_a.csv
./build/render host/render/scenarios/demo_step_melody.json /tmp/t4_b.wav /tmp/t4_b.csv
cmp /tmp/t4_a.wav /tmp/t4_b.wav && cmp /tmp/t4_a.csv /tmp/t4_b.csv && echo DETERMINISTIC
./build/render host/render/scenarios/ambient_wash.json /tmp/t4_c.wav /tmp/t4_c.csv
./build/render host/render/scenarios/ambient_wash.json /tmp/t4_d.wav /tmp/t4_d.csv
cmp /tmp/t4_c.wav /tmp/t4_d.wav && echo DETERMINISTIC2
```

Expected: `DETERMINISTIC` and `DETERMINISTIC2`.

- [ ] **Step 7: Commit**

```bash
git add engine/mod/super_modulator.h engine/mod/super_modulator.cpp tests/test_super_modulator.cpp
git commit -m "perf(mod): texture lanes advance on the 96-sample raster"
```

---

### Task 5: Re-pin the identity gate + docs

**Files:**
- Modify: `host/render/scenarios/ctrl_identity.sha256`
- Modify: `docs/roadmap.md` (the CPU cut list)

**Interfaces:**
- Consumes: the finished behavior change (Task 4).
- Produces: a valid determinism gate for future sessions.

- [ ] **Step 1: Re-render and re-pin `ctrl_identity`**

The texture-lane smoothing granularity change makes the old checksum stale by design (spec §"What stays bit-identical, what changes"). Re-pin it:

```bash
./build/render host/render/scenarios/ctrl_identity.json /tmp/ctrl_id.wav /tmp/ctrl_id.csv
sha256sum /tmp/ctrl_id.wav
cat host/render/scenarios/ctrl_identity.sha256   # note the existing format
```

Overwrite `host/render/scenarios/ctrl_identity.sha256` with the new hash **in exactly the existing file's format** (same layout, hash replaced). Then prove the pin: re-render into a second file, `sha256sum` it, confirm it matches the new pinned value.

- [ ] **Step 2: Update the roadmap cut list**

In `docs/roadmap.md`, find the ranked CPU cut list (the section discussing the mod-plane / part-glue / FLUX cuts, around lines 420–486) and mark the mod-plane control-rate item as implemented, e.g. append to its line: `— DONE 2026-07-19 (texture lanes on the 96er raster, spec 2026-07-19-mod-plane-control-rate-design.md; measured saving pending the next hardware bench)`. Do not restructure the list.

- [ ] **Step 3: Full suite one last time**

Run: `./build/spky_tests`
Expected: 329/329 pass.

- [ ] **Step 4: Commit**

```bash
git add host/render/scenarios/ctrl_identity.sha256 docs/roadmap.md
git commit -m "docs: re-pin ctrl_identity, mark the mod-plane cut done"
```

---

## Out of scope / deferred

- **Hardware bench re-run** (`python bench/run.py`, Daisy Seed attached) — user step; the ~17–19-point estimate is not trusted until measured. Commit code BEFORE measuring (run.py names result files by HEAD hash).
- **VCV plugin rebuild** — engine-only change; the installed plugin needs a rebuild before any Rack play test.
- **`renders/` refresh** — all references diverge byte-wise (accepted); belongs to the user's listening pass.
- **Approach B** (PITCH lane on the raster + analytic fire scheduling) — named follow-up in the spec, only if the budget stays tight.

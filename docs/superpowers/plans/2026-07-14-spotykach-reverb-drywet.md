# Spotykach M4.8 — Reverb Dry/Wet Mix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One MIX control on the shared ambient reverb: equal-power dry/wet crossfade at the master join, with a true CPU-saving bypass (clear-on-sleep) at 0 %.

**Architecture:** The reverb is already a send/return (`Instrument::process` sums per-part sends into `AmbientReverb`, adds the wet return after the part mix). MIX is two smoothed gains at that join plus a sleep gate that skips `_reverb->process()` entirely at 0 %. The Oliverb core gains a `Clear()`; the facade exposes it as `clear()`.

**Tech Stack:** Portable C++17 engine (no heap, deterministic), doctest, CMake+clang+ninja desktop build, VCV Rack plugin host (Rack-SDK 2.6.6 + WinLibs mingw), Python panel generator.

**Spec:** `docs/superpowers/specs/2026-07-14-spotykach-reverb-drywet-design.md` (residency repo)

## Global Constraints

- **Repo:** `c:\Users\bernd\Documents\AI\Spotykach` (NOT the residency repo). Branch from `melody-engine-rework` (M4.7 HEAD, currently `152b88e`): `git checkout -b reverb-drywet`.
- **Engine build/test** (Git Bash): `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure`. Single doctest binary: `./build/spky_tests.exe -tc="<filter>*"`.
- **No heap in the engine.** `AmbientReverb` is BIG (~130 KB inline buffer) — never stack-allocate; tests use `static` instances (existing pattern).
- **Determinism is an invariant:** identical call sequence → bit-identical output. Nothing in this feature may read wall-clock, use uninitialized state, or diverge across runs.
- **Equal-power law with exact endpoints:** `dry = cos(m·π/2)`, `wet = sin(m·π/2)`, but m ≤ 0 → (1, 0) and m ≥ 1 → (0, 1) exactly. Default mix **0.25**. Gain glide **10 ms** one-pole (`spky::OnePole`).
- `kWetGain` (0.40 in `engine/fx/reverb.cpp`) stays; only its stale comment changes.
- **Commit trailer** (every commit): `Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>` — never the default Claude/Anthropic trailer.

---

### Task 1: Baseline render + `Oliverb::Clear()` / `AmbientReverb::clear()` + `kWetGain` comment fix

**Files:**
- Modify: `third_party/oliverb/oliverb.h` (add `Clear()`, extend the port-mod header comment)
- Modify: `engine/fx/reverb.h` (declare `clear()`)
- Modify: `engine/fx/reverb.cpp` (define `clear()`, fix the `kWetGain` comment)
- Test: `tests/test_reverb.cpp` (append one test case)

**Interfaces:**
- Consumes: `clouds::FxEngine::Clear()` (already exists in `third_party/oliverb/fx_engine.h:82` — zeroes the buffer, resets the write pointer).
- Produces: `void AmbientReverb::clear()` — empties the room (delay buffer + loop damping filter state), parameter state survives. Task 3 calls this on falling asleep.

- [ ] **Step 1: Create the branch and render the by-ear baseline**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
git checkout -b reverb-drywet
source env.sh
cmake -S . -B build && cmake --build build
./build/render.exe host/render/scenarios/ambient_wash.json build/ambient_wash_baseline.wav
```

Expected: branch created; `build/ambient_wash_baseline.wav` written (`build/` is untracked). This is the pre-M4.8 reference for the final by-ear comparison in Task 6.

- [ ] **Step 2: Write the failing test**

Append to `tests/test_reverb.cpp` (match its existing includes/`using`):

```cpp
TEST_CASE("reverb: clear() empties the room but keeps the parameter state") {
    static AmbientReverb rv;             // BIG object: never stack-allocate
    rv.init(48000.f);
    rv.set_decay(0.8f);
    float l, r;
    // ring up a tail: periodic impulses for 0.25 s
    for (int i = 0; i < 12000; ++i) {
        float in = (i % 4800 == 0) ? 0.9f : 0.f;
        rv.process(in, in, l, r);
    }
    float energy = 0.f;                  // the room is audibly ringing
    for (int i = 0; i < 4800; ++i) { rv.process(0.f, 0.f, l, r); energy += l * l + r * r; }
    CHECK(energy > 1e-6f);

    rv.clear();
    // silence in -> exact silence out: buffer AND loop filter state are zeroed
    for (int i = 0; i < 4800; ++i) {
        rv.process(0.f, 0.f, l, r);
        CHECK(l == 0.f);
        CHECK(r == 0.f);
    }
    // parameters survived the clear: a fresh impulse still rings the same room
    rv.process(0.9f, 0.9f, l, r);
    float energy2 = 0.f;
    for (int i = 0; i < 9600; ++i) { rv.process(0.f, 0.f, l, r); energy2 += l * l + r * r; }
    CHECK(energy2 > 1e-6f);
}
```

- [ ] **Step 3: Run the test to verify it fails**

```bash
cmake --build build && ./build/spky_tests.exe -tc="reverb: clear*"
```

Expected: **compile error** — `AmbientReverb` has no member `clear` (that is the TDD failure mode for a missing C++ API).

- [ ] **Step 4: Implement `Oliverb::Clear()` and `AmbientReverb::clear()`**

In `third_party/oliverb/oliverb.h`, after `Prepare()` (line ~85), add:

```cpp
  // Spotykach port addition: empty the room without touching parameters —
  // zeroes the delay buffer and the loop damping filter state. Used by the
  // engine's dry/wet clear-on-sleep bypass (M4.8).
  void Clear() {
    engine_.Clear();
    lp_decay_1_ = lp_decay_2_ = 0.0f;
    hp_decay_1_ = hp_decay_2_ = 0.0f;
  }
```

And append one line to the port-modifications comment block at the top of the file (after the `size smoothing coefficient` bullet):

```cpp
//  - Clear() added: empties buffer + loop filter state, params survive
//    (backs the engine's M4.8 dry/wet clear-on-sleep bypass)
```

In `engine/fx/reverb.h`, after `void init(float sample_rate);` add:

```cpp
    void clear();                 // empty the room (buffer + loop filter state); params survive
```

In `engine/fx/reverb.cpp`, after `AmbientReverb::init`, add:

```cpp
void AmbientReverb::clear() {
    _verb.Clear();
    _ctrl = 0;   // refresh the LFO slopes on the next process()
}
```

- [ ] **Step 5: Fix the stale `kWetGain` comment**

In `engine/fx/reverb.cpp`, the comment above `kWetGain` currently ends with *"…until the M6 master soft-clip exists. Ear-tunable in [0.40, 0.50]; kept at the low end of that range to hold the ambient_wash showcase's bloom under the hard clip ceiling."* Replace the whole comment block (lines 17–22) with:

```cpp
// The self-oscillating bloom (decay > 1.0) plateaus near digital full scale
// at the core's output taps (they carry 2x the in-loop signal, plus Hermite
// overshoot under depth modulation). Trim the wet-only room -8 dB so the
// bloom leaves headroom at the master sum — the M4.6 limiter is a ceiling,
// not a mixer; don't lean on it. Ear-tunable in [0.40, 0.50]; kept at the
// low end to hold the ambient_wash showcase's bloom clear of the ceiling.
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: all suites PASS (the new case and every pre-existing one).

- [ ] **Step 7: Commit**

```bash
git add third_party/oliverb/oliverb.h engine/fx/reverb.h engine/fx/reverb.cpp tests/test_reverb.cpp
git commit -m "feat(reverb): clear() empties the room, params survive

Backs the M4.8 dry/wet clear-on-sleep bypass. Also corrects the stale
kWetGain comment (the master limiter shipped in M4.6).

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 2: `Instrument::set_reverb_mix` — equal-power gains at the master join

**Files:**
- Modify: `engine/instrument.h` (members, setter declaration; include `util/onepole.h`)
- Modify: `engine/instrument.cpp` (constants, setter body, init wiring, mix at the join in `process`)
- Test: `tests/test_instrument.cpp` (three new cases, two existing cases touched)

**Interfaces:**
- Consumes: `spky::OnePole` (`engine/util/onepole.h`: `init(sr, time_s)`, `process(target)` → smoothed value that **snaps exactly to target** when within 0.0005, `reset(value)`), `clampf` (`engine/util/math.h`).
- Produces: `void Instrument::set_reverb_mix(float n)` (0..1, clamped; exact-endpoint equal-power targets) and private members `_rev_dry_target`, `_rev_wet_target` (floats), `_rev_dry`, `_rev_wet` (OnePole), `_rev_primed` (bool). Task 3 extends the same code paths with the sleep gate; Tasks 4/5 forward to this setter.

- [ ] **Step 1: Write the failing tests**

In `tests/test_instrument.cpp`:

**(a)** Extend the existing `"instrument: all FX off + send 0 is bit-identical to the no-FX build"` — the mix's dry gain (default 0.25 → cos ≈ 0.9239) would now break strict bit-identity, so the invariant becomes *FX off + send 0 + MIX 0*. After the two `set_fx_target_base(..., FXT_REV_SEND, 0.f)` lines add:

```cpp
    fx.set_reverb_mix(0.f);                        // MIX 0: dry passes untouched
```

**(b)** Extend the existing `"instrument: fx setters reach the parts and reverb setters are null-safe"` — after `inst.set_reverb_depth(0.5f);` add:

```cpp
    inst.set_reverb_mix(0.7f);
```

**(c)** Append three new cases:

```cpp
TEST_CASE("instrument M4.8: mix 0 is bit-identical to the engine-only build") {
    Instrument plain;
    plain.init(48000.f);
    Instrument fx;
    fx.init(48000.f, test_fx_mem());
    fx.set_reverb_mix(0.f);            // before the first process(): snaps
    // NOTE: the default sends stay live — the wet return is simply discarded
    float pl, pr, fl, fr;
    for (int i = 0; i < 48000; ++i) {
        plain.process(nullptr, nullptr, &pl, &pr, 1);
        fx.process(nullptr, nullptr, &fl, &fr, 1);
        CHECK(fl == pl);
        CHECK(fr == pr);
    }
}

TEST_CASE("instrument M4.8: mix 1 with muted sends is exact silence (dry fully gone)") {
    Instrument fx;
    fx.init(48000.f, test_fx_mem());
    fx.set_reverb_mix(1.f);
    for (int p = 0; p < PART_COUNT; ++p)
        fx.set_fx_target_base(p, FXT_REV_SEND, 0.f);   // empty room: wet is silence
    float l, r;
    for (int i = 0; i < 48000; ++i) {
        fx.process(nullptr, nullptr, &l, &r, 1);
        CHECK(l == 0.f);               // dry gain is EXACTLY 0 at the endpoint
        CHECK(r == 0.f);
    }
}

TEST_CASE("instrument M4.8: mix 0.5 sits at equal power (both gains cos(pi/4))") {
    // Three identically-seeded fx instruments at MIX 0 / 0.5 / 1, default
    // sends live. Their dry and wet streams are bit-identical, so:
    //   out0   = dry            out1   = wet
    //   out05  = 0.7071*dry + 0.7071*wet
    // => rms(out05 - 0.7071*out0) / rms(out1) == 0.7071  (wet gain)
    //    rms(out05 - 0.7071*out1) / rms(out0) == 0.7071  (dry gain)
    static float echoEP[3][PART_COUNT][2][Flux::kMaxSamples];
    static AmbientReverb rvEP[3];
    Instrument inst[3];
    const float mixes[3] = { 0.f, 0.5f, 1.f };
    for (int k = 0; k < 3; ++k) {
        FxMem m;
        for (int p = 0; p < PART_COUNT; ++p)
            for (int c = 0; c < 2; ++c) m.echo[p][c] = echoEP[k][p][c];
        m.reverb = &rvEP[k];
        inst[k].init(48000.f, m);
        inst[k].set_reverb_mix(mixes[k]);
    }
    float l[3], r[3];
    for (int i = 0; i < 48000; ++i)                     // settle: gains + room fill
        for (int k = 0; k < 3; ++k) inst[k].process(nullptr, nullptr, &l[k], &r[k], 1);
    const float g = 0.70710678f;
    double accW = 0.0, acc1 = 0.0, accD = 0.0, acc0 = 0.0;
    for (int i = 0; i < 96000; ++i) {
        for (int k = 0; k < 3; ++k) inst[k].process(nullptr, nullptr, &l[k], &r[k], 1);
        float wet_half = l[1] - g * l[0];
        float dry_half = l[1] - g * l[2];
        accW += wet_half * wet_half;  acc1 += l[2] * l[2];
        accD += dry_half * dry_half;  acc0 += l[0] * l[0];
    }
    CHECK(std::sqrt(accW / acc1) == doctest::Approx(g).epsilon(0.02));
    CHECK(std::sqrt(accD / acc0) == doctest::Approx(g).epsilon(0.02));
}

TEST_CASE("instrument M4.8: hard MIX jumps are smoothed (no zipper)") {
    static float echoZ[PART_COUNT][2][Flux::kMaxSamples];
    static AmbientReverb rvZ;
    auto run_maxd = [&](bool stepped) {
        FxMem m;
        for (int p = 0; p < PART_COUNT; ++p)
            for (int c = 0; c < 2; ++c) m.echo[p][c] = echoZ[p][c];
        m.reverb = &rvZ;
        Instrument inst;
        inst.init(48000.f, m);                 // init() re-clears the shared statics
        float l = 0.f, r = 0.f, prev = 0.f, maxd = 0.f;
        for (int i = 0; i < 96000; ++i) {
            if (stepped && i == 48000) inst.set_reverb_mix(1.f);
            if (stepped && i == 72000) inst.set_reverb_mix(0.f);
            inst.process(nullptr, nullptr, &l, &r, 1);
            if (i > 0) { float d = std::fabs(l - prev); if (d > maxd) maxd = d; }
            prev = l;
        }
        return maxd;
    };
    float steady = run_maxd(false);
    float stepped = run_maxd(true);
    // an unsmoothed 0->1 gain jump would spike the per-sample delta far above
    // the drone's own; the 10 ms glide keeps it in the same ballpark
    CHECK(stepped < 2.f * steady + 0.01f);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cmake --build build 2>&1 | head -30
```

Expected: **compile error** — `Instrument` has no member `set_reverb_mix`.

- [ ] **Step 3: Implement the setter and the mix at the join**

`engine/instrument.h`:

1. Add include (with the other `fx/` includes): `#include "util/onepole.h"`
2. After `void set_reverb_depth(float n) { ... }` (line ~65) add the declaration:

```cpp
    void set_reverb_mix(float n);   // 0..1 equal-power dry/wet at the master join
```

3. In the private section, after `AmbientReverb* _reverb = nullptr;`:

```cpp
    float   _rev_dry_target = 1.f;  // equal-power gain targets (exact endpoints)
    float   _rev_wet_target = 0.f;
    OnePole _rev_dry, _rev_wet;     // 10 ms glide at the master join
    bool    _rev_primed = false;    // first process() snaps the mix gains
```

`engine/instrument.cpp`:

1. Includes at the top:

```cpp
#include "instrument.h"
#include "util/math.h"
#include <cmath>
```

2. Anonymous namespace after `using namespace spky;`:

```cpp
namespace {
constexpr float kHalfPi = 1.57079632679489661923f;
// ~= the pre-M4.8 fixed balance: cos/sin(0.25*pi/2) = dry 0.92 / wet 0.38
// against the old dry 1.0 / wet 0.40 (dry -0.7 dB; same ratio within a hair)
constexpr float kDefaultReverbMix = 0.25f;
constexpr float kMixSmoothS = 0.010f;    // dry/wet gain glide; ear-tunable
}
```

3. In `Instrument::init(float, const FxMem&)`, after `if (_reverb) _reverb->init(sample_rate);`:

```cpp
    _rev_dry.init(sample_rate, kMixSmoothS);
    _rev_wet.init(sample_rate, kMixSmoothS);
    _rev_primed = false;
    set_reverb_mix(kDefaultReverbMix);
```

4. New setter (control-rate libm is fine — same policy as `AmbientReverb::set_tone`):

```cpp
void Instrument::set_reverb_mix(float n) {
    n = clampf(n, 0.f, 1.f);
    if (n <= 0.f)      { _rev_dry_target = 1.f; _rev_wet_target = 0.f; }
    else if (n >= 1.f) { _rev_dry_target = 0.f; _rev_wet_target = 1.f; }
    else {
        _rev_dry_target = std::cos(n * kHalfPi);   // equal-power crossfade
        _rev_wet_target = std::sin(n * kHalfPi);
    }
}
```

5. In `Instrument::process`, replace the reverb block (keep the existing MORPH comment):

```cpp
        if (_reverb) {
            if (!_rev_primed) {              // snap a mix set before the first block
                _rev_dry.reset(_rev_dry_target);
                _rev_wet.reset(_rev_wet_target);
                _rev_primed = true;
            }
            const float dg = _rev_dry.process(_rev_dry_target);
            const float wg = _rev_wet.process(_rev_wet_target);
            // MORPH fades dry AND send together (M4 supersedes the M1.6
            // pre-morph-send rule): a fully morphed-away part injects no new
            // reverb; only its already-committed tail rings out.
            float wl, wr;
            _reverb->process(asl * ga + bsl * gb, asr * ga + bsr * gb, wl, wr);
            l = l * dg + wl * wg;
            r = r * dg + wr * wg;
        }
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: all suites PASS — including the two modified pre-existing cases and the four new M4.8 cases. If the equal-power case misses its 0.02 epsilon, suspect the master limiter engaging (it must be transparent at drone level) — investigate before loosening the epsilon.

- [ ] **Step 5: Commit**

```bash
git add engine/instrument.h engine/instrument.cpp tests/test_instrument.cpp
git commit -m "feat(instrument): equal-power reverb dry/wet MIX at the master join

set_reverb_mix (0..1): dry cos / wet sin with exact endpoints, 10 ms
one-pole glide, default 0.25 ~= the old fixed dry/wet balance.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 3: Clear-on-sleep bypass at MIX 0

**Files:**
- Modify: `engine/instrument.h` (`_rev_asleep` member, `reverb_asleep()` getter, wake in the setter path)
- Modify: `engine/instrument.cpp` (sleep/wake logic in `set_reverb_mix`, `init`, `process`)
- Test: `tests/test_instrument.cpp` (three new cases, one line added to a Task 2 case)

**Interfaces:**
- Consumes: `AmbientReverb::clear()` (Task 1), the Task 2 members/gains.
- Produces: `bool Instrument::reverb_asleep() const` — true while the room is bypassed (M6 can wire an LED to it; the tests observe the gate through it).

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_instrument.cpp`:

```cpp
TEST_CASE("instrument M4.8: MIX 0 sleeps the room, any MIX > 0 wakes it") {
    Instrument fx;
    fx.init(48000.f, test_fx_mem());
    float l, r;
    fx.process(nullptr, nullptr, &l, &r, 1);
    CHECK(!fx.reverb_asleep());            // boot mix 0.25: awake
    fx.set_reverb_mix(0.f);                // runtime fade-out -> sleep
    for (int i = 0; i < 9600; ++i) fx.process(nullptr, nullptr, &l, &r, 1);
    CHECK(fx.reverb_asleep());             // 0.2 s >> the 10 ms glide + snap
    fx.set_reverb_mix(0.4f);
    CHECK(!fx.reverb_asleep());            // waking is immediate
}

TEST_CASE("instrument M4.8: waking from sleep starts with an empty room (no ghost tail)") {
    static float echoGX[PART_COUNT][2][Flux::kMaxSamples];
    static float echoGY[PART_COUNT][2][Flux::kMaxSamples];
    static AmbientReverb rvGX, rvGY;
    FxMem mx, my;
    for (int p = 0; p < PART_COUNT; ++p)
        for (int c = 0; c < 2; ++c) { mx.echo[p][c] = echoGX[p][c]; my.echo[p][c] = echoGY[p][c]; }
    mx.reverb = &rvGX; my.reverb = &rvGY;
    Instrument x; x.init(48000.f, mx);
    Instrument y; y.init(48000.f, my);
    x.set_reverb_decay(0.85f); y.set_reverb_decay(0.85f);  // a surviving ghost would ring loud
    // Y is the reference: sends muted from boot, MIX 0.5 from boot
    y.set_reverb_mix(0.5f);
    for (int p = 0; p < PART_COUNT; ++p) y.set_fx_target_base(p, FXT_REV_SEND, 0.f);
    float xl, xr, yl, yr;
    // phase 1 (1 s): X rings up a loud tail on the default sends
    for (int i = 0; i < 48000; ++i) { x.process(nullptr, nullptr, &xl, &xr, 1);
                                      y.process(nullptr, nullptr, &yl, &yr, 1); }
    // phase 2 (0.5 s): X mutes its sends and goes to sleep at MIX 0
    for (int p = 0; p < PART_COUNT; ++p) x.set_fx_target_base(p, FXT_REV_SEND, 0.f);
    x.set_reverb_mix(0.f);
    for (int i = 0; i < 24000; ++i) { x.process(nullptr, nullptr, &xl, &xr, 1);
                                      y.process(nullptr, nullptr, &yl, &yr, 1); }
    CHECK(x.reverb_asleep());
    // phase 3 (0.5 s settle): X wakes at MIX 0.5; gains glide and snap
    x.set_reverb_mix(0.5f);
    for (int i = 0; i < 24000; ++i) { x.process(nullptr, nullptr, &xl, &xr, 1);
                                      y.process(nullptr, nullptr, &yl, &yr, 1); }
    // both rooms are now empty and unfed, the part streams are identical:
    // any difference left would be X's pre-sleep tail — it must be GONE
    float maxd = 0.f;
    for (int i = 0; i < 24000; ++i) {
        x.process(nullptr, nullptr, &xl, &xr, 1);
        y.process(nullptr, nullptr, &yl, &yr, 1);
        float d = std::fabs(xl - yl); if (d > maxd) maxd = d;
    }
    CHECK(maxd == 0.f);
}

TEST_CASE("instrument M4.8: mix automation incl. sleep is deterministic end to end") {
    auto run = [] {
        static float echoD[PART_COUNT][2][Flux::kMaxSamples];
        static AmbientReverb rvD;
        FxMem m;
        for (int p = 0; p < PART_COUNT; ++p)
            for (int c = 0; c < 2; ++c) m.echo[p][c] = echoD[p][c];
        m.reverb = &rvD;
        Instrument inst;
        inst.init(48000.f, m);            // init() re-clears the shared statics
        std::vector<float> out;
        float l[96], r[96];
        for (int b = 0; b < 500; ++b) {
            if (b == 100) inst.set_reverb_mix(0.8f);
            if (b == 250) inst.set_reverb_mix(0.f);   // sleeps mid-run
            if (b == 400) inst.set_reverb_mix(0.5f);  // wakes again
            inst.process(nullptr, nullptr, l, r, 96);
            for (int i = 0; i < 96; ++i) out.push_back(l[i]);
        }
        return out;
    };
    auto a = run(), b = run();
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) CHECK(a[i] == b[i]);
}
```

Also add one line to Task 2's `"instrument M4.8: mix 0 is bit-identical to the engine-only build"` — after the loop, assert the CPU gate actually engaged:

```cpp
    CHECK(fx.reverb_asleep());
}
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cmake --build build 2>&1 | head -30
```

Expected: **compile error** — `Instrument` has no member `reverb_asleep`.

- [ ] **Step 3: Implement the sleep gate**

`engine/instrument.h`:

1. Next to the other center-section getters (`weather()`, `phase_err()`), add:

```cpp
    bool reverb_asleep() const { return _rev_asleep; }
```

2. In the private section, after `bool _rev_primed = false;`:

```cpp
    bool    _rev_asleep = false;    // MIX 0 gate: room cleared, process() skipped
```

`engine/instrument.cpp`:

1. In `set_reverb_mix`, after the target assignment block, add:

```cpp
    if (_rev_wet_target > 0.f) _rev_asleep = false;   // wake into the cleared room
```

2. In `init`, before `set_reverb_mix(kDefaultReverbMix);`:

```cpp
    _rev_asleep = false;
```

3. In `process`, replace the Task 2 reverb block with:

```cpp
        if (_reverb) {
            if (!_rev_primed) {              // snap a mix set before the first block
                _rev_dry.reset(_rev_dry_target);
                _rev_wet.reset(_rev_wet_target);
                if (_rev_wet_target == 0.f) { _reverb->clear(); _rev_asleep = true; }
                _rev_primed = true;
            }
            const float dg = _rev_dry.process(_rev_dry_target);
            const float wg = _rev_wet.process(_rev_wet_target);
            if (!_rev_asleep) {
                // MORPH fades dry AND send together (M4 supersedes the M1.6
                // pre-morph-send rule): a fully morphed-away part injects no new
                // reverb; only its already-committed tail rings out.
                float wl, wr;
                _reverb->process(asl * ga + bsl * gb, asr * ga + bsr * gb, wl, wr);
                l = l * dg + wl * wg;
                r = r * dg + wr * wg;
                if (wg == 0.f && dg == 1.f && _rev_wet_target == 0.f) {
                    _reverb->clear();        // clear-on-sleep: waking starts empty
                    _rev_asleep = true;      // Oliverb CPU is off until MIX reopens
                }
            }
            // asleep: dry passes bit-exact (dg has snapped to 1), sends discarded
        }
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: all suites PASS. If the ghost-tail case's `maxd == 0.f` fails with a tiny (< 1e-6) residue, the cause is limiter-envelope history diverging between X and Y — verify that hypothesis explicitly (print `maxd`, check it shrinks with a longer phase 3) before relaxing the check to `maxd < 1e-6f` with a comment.

- [ ] **Step 5: Commit**

```bash
git add engine/instrument.h engine/instrument.cpp tests/test_instrument.cpp
git commit -m "feat(instrument): clear-on-sleep reverb bypass at MIX 0

When the wet gain fades out at MIX 0 the room is cleared once and
process() is skipped (Oliverb CPU -> 0, blooms genuinely killed).
Waking starts from an empty room; reverb_asleep() exposes the gate.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 4: Render-host action `set_reverb_mix`

**Files:**
- Modify: `host/render/scenario.cpp:113` (one dispatch line)
- Test: `tests/test_scenario.cpp` (extend the fx-actions case)

**Interfaces:**
- Consumes: `Instrument::set_reverb_mix(float)` (Task 2).
- Produces: scenario action string `"set_reverb_mix"` (global, no part, null-safe like the other reverb actions).

- [ ] **Step 1: Write the failing test**

In `tests/test_scenario.cpp`, inside `TEST_CASE("scenario: fx actions reach the instrument")`, after the `set_reverb_decay` event block (line ~97), add:

```cpp
    Event mix;     // global reverb action: no part, null-safe
    mix.action = "set_reverb_mix";
    mix.value = 0.3f;
    apply_event(inst, mix);
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build build && ./build/spky_tests.exe -tc="scenario: fx actions*"
```

Expected: build succeeds; behaviorally the unknown action is what the dispatcher must not silently ignore — check how `apply_event` handles unknown actions (it may `CHECK`/abort or ignore). If unknown actions are silently ignored, this step passes trivially: note that, and rely on Step 4's full run. (The dispatch line is still required; the real consumer is the scenario JSON surface.)

- [ ] **Step 3: Implement the dispatch line**

In `host/render/scenario.cpp`, after the `set_reverb_depth` line (113):

```cpp
    else if (a == "set_reverb_mix")       inst.set_reverb_mix(e.value);
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: all suites PASS.

- [ ] **Step 5: Commit**

```bash
git add host/render/scenario.cpp tests/test_scenario.cpp
git commit -m "feat(host): set_reverb_mix scenario action

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 5: VCV host — REV_MIX knob, panel regen, plugin rebuild + install

**Files:**
- Modify: `host/vcv/res/gen_panel.py:104` (one `Ctl` line in `SHARED`)
- Regenerate: `host/vcv/src/generated_panel.hpp`, `host/vcv/res/Spotymod.svg` (both committed, emitted by the generator — never hand-edit)
- Modify: `host/vcv/src/Spotymod.cpp` (default + forwarding)

**Interfaces:**
- Consumes: `Instrument::set_reverb_mix(float)` (Task 2); generated enum `REV_MIX` (from the generator).
- Produces: a usable MIX knob in VCV Rack.

*(No engine-test cycle here — the VCV host has no test suite; the gate is a clean plugin build + the Rack smoke test.)*

- [ ] **Step 1: Add REV_MIX to the panel generator and regenerate**

In `host/vcv/res/gen_panel.py`, in the `SHARED` list after the `REV_DEPTH` line (104), add:

```python
    Ctl("REV_MIX",   SMKNOB, CX, 78.0, "MIX"),
```

(Center column, below TEMPO at y 66, above the IN/CLK row at y 88 — no glyph overlap: SMKNOB r 3.0 vs IN r 4.2, 10 mm apart.)

Regenerate (Git Bash):

```bash
cd /c/Users/bernd/Documents/AI/Spotykach/host/vcv && python res/gen_panel.py
git diff --stat
```

Expected: `src/generated_panel.hpp` and `res/Spotymod.svg` change; the header gains `REV_MIX` in the enum and one control-table row `{REV_MIX, WK_SMKNOB, {..., 78.000f}, "MIX"}`.

- [ ] **Step 2: Wire default and forwarding in Spotymod.cpp**

In `host/vcv/src/Spotymod.cpp`:

1. In `defaultFor(...)`, after `case REV_DEPTH:    return 0.25f;` (line ~98):

```cpp
            case REV_MIX:      return 0.25f;   // ~= the pre-M4.8 fixed balance
```

2. In the parameter-forwarding block, after `inst.set_reverb_depth(...)` (line ~187):

```cpp
        inst.set_reverb_mix(params[REV_MIX].getValue());
```

- [ ] **Step 3: Build the plugin**

```bash
/c/msys64/usr/bin/bash.exe -lc 'export PATH="/c/Users/bernd/Documents/AI/mingw64/bin:$PATH"; cd "/c/Users/bernd/Documents/AI/Spotykach/host/vcv"; make -j4 CC=gcc CXX=g++'
```

Expected: compiles clean, produces `host/vcv/plugin.dll`. (WinLibs MinGW must be prepended — the system `g++` is the Daisy ARM cross-compiler. The Makefile already carries `EXTRA_CXXFLAGS += -std=c++17`.)

- [ ] **Step 4: Install into Rack**

```bash
/c/msys64/usr/bin/bash.exe -lc 'export PATH="/c/Users/bernd/Documents/AI/mingw64/bin:$PATH"; cd "/c/Users/bernd/Documents/AI/Spotykach/host/vcv"; make install RACK_USER_DIR="/c/Users/bernd/AppData/Local/Rack2"'
```

Expected: `.vcvplugin` packaged into `C:\Users\bernd\AppData\Local\Rack2\plugins-win-x64\`. (`RACK_USER_DIR` must be passed explicitly — `$LOCALAPPDATA` is unset in the msys login shell.)

- [ ] **Step 5: Hand the smoke test to the user**

Ask the user to open VCV Rack (Rack2Pro) and verify on the Spotymod module: MIX at 0 % = dry only (identical to before), 50 % = balanced dry/wet at constant loudness, 100 % = wet only (dry gone); turning to 0 and back up = tail restarts clean (no ghost). This is the spec's manual acceptance criterion — do not mark it done on the user's behalf.

- [ ] **Step 6: Commit**

```bash
git add host/vcv/res/gen_panel.py host/vcv/src/generated_panel.hpp host/vcv/res/Spotymod.svg host/vcv/src/Spotymod.cpp
git commit -m "feat(vcv): REV_MIX dry/wet knob on the shared center strip

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 6: Roadmap entry + final verification

**Files:**
- Modify: `docs/roadmap.md` (milestone table row + Done section entry)
- Verify: full test suite, `ambient_wash` A/B render

**Interfaces:**
- Consumes: everything above. Produces: nothing new — closes out M4.8.

- [ ] **Step 1: Add the roadmap entries**

In `docs/roadmap.md`, milestone table: add directly under the last M4.x row present (M4.6 today; after the M4.7 row instead if the melody-rework branch has added one by then):

```markdown
| **M4.8** | Reverb dry/wet — equal-power MIX at the master join + clear-on-sleep CPU bypass | ✅ **done** (engine + host; UI wiring deferred to M6) |
```

In the Done section, after the most recent `### M4.x` entry, add:

```markdown
### M4.8 — Reverb dry/wet mix ✅

- `set_reverb_mix` (0..1): equal-power dry/wet crossfade at the master join —
  dry = cos(m·π/2), wet = sin(m·π/2) with exact endpoints, 10 ms one-pole
  glide. Default 0.25 ≈ the old fixed balance (dry 1.0 / wet 0.40). The wet
  path keeps its internal −8 dB bloom-headroom trim; the send input is
  untouched by MIX, so the tail character never changes while turning.
- MIX 0 is a true bypass: the wet gain fades out, the room is cleared once
  (`AmbientReverb::clear()` — buffer + loop filter state, params survive) and
  `process()` is skipped. Oliverb CPU drops to zero; a self-oscillating bloom
  is genuinely killed. Any MIX > 0 wakes into a clean, empty room
  (`reverb_asleep()` exposes the gate for the M6 UI).
- Hosts: VCV `REV_MIX` knob (shared center strip, default 0.25), render
  action `set_reverb_mix`.
```

- [ ] **Step 2: Run the full suite**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: every test PASSES.

- [ ] **Step 3: A/B render for the by-ear default check**

```bash
./build/render.exe host/render/scenarios/ambient_wash.json build/ambient_wash_m48.wav
```

Compare `build/ambient_wash_m48.wav` against `build/ambient_wash_baseline.wav` (Task 1). Expected: near-indistinguishable (dry −0.7 dB, same wet ratio). Numeric sanity check (peak/RMS per file) is fine as a proxy; flag anything beyond ~1 dB RMS drift. Hand both files to the user for the final listen — the spec's by-ear criterion is theirs to sign off, along with a wet-only listen (render a copy of `ambient_wash.json` with an added init event `{"action":"set_reverb_mix","value":1.0}` to `build/ambient_wash_wetonly.wav` and include it).

- [ ] **Step 4: Commit**

```bash
git add docs/roadmap.md
git commit -m "docs(roadmap): M4.8 reverb dry/wet mix — done

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

## Post-plan notes

- **Branch finish:** after all tasks + user sign-off (Rack smoke test, renders), use superpowers:finishing-a-development-branch to decide merge/PR for `reverb-drywet`.
- **Out of scope (per spec):** a MIX ride in `ambient_wash.json`, M6 knob-map wiring, any `kWetGain` retuning.

# Scale Quantization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Quantize the PITCH target to a global musical scale (boot default Dorian) with per-part SCALE/CHROM/FREE modes, wired through engine, Instrument API, and the render host.

**Architecture:** A small header-only `Quantizer` (nearest-note lookup over a 12-bit scale mask, hysteresis, change slew) is applied by `Part` as the last stage of the PITCH target, after base+depth and lane smoothing. `pitch_cv()` therefore yields the quantized value for engine, CSV/CV, and future capture — one source of truth. Host scenarios gain `set_scale` / `set_quant_mode` / `set_root` actions.

**Tech Stack:** C++17, doctest (vendored), nlohmann/json (vendored), CMake + Ninja + clang (desktop host).

**Spec:** `docs/superpowers/specs/2026-07-11-spotykach-scales-design.md` (residency repo). Hardware gestures (ALT+TUNE etc.) and preset persistence are explicitly out of scope here — they land in the UI/firmware milestone.

## Global Constraints

- Code repo: `c:\Users\bernd\Documents\AI\Spotykach` — all code commits go there, on branch `feat/scale-quantization` (created in Task 1). This plan file lives in the residency repo.
- Build/test (Git Bash): `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake -B build && cmake --build build --target spky_tests && ./build/spky_tests.exe`
- Pitch contract: normalized `0..1` = **36 semitones (3 octaves)** — matches `TestToneEngine` (`110·8^p` → 110–880 Hz). All engines and CV follow it.
- Boot default: global scale **Dorian**, both parts in **SCALE** mode, root 0. No implicit FREE anywhere — existing scenarios opt out explicitly.
- Hysteresis ±15 cents at raster boundaries; scale/mode/root changes apply immediately, softened by a ~40 ms slew; FREE is a pure passthrough (no computation, switches instantly).
- Engine stays hardware-free (nothing under `src/` or `lib/` is touched). No heap allocation in the audio path. C++17, match existing style (4-space indent, `_member` fields, doctest `TEST_CASE`).

---

### Task 1: Scale table + Quantizer core (modes, root, nearest-note)

**Files:**
- Create: `engine/pitch/quantizer.h`
- Test: `tests/test_quantizer.cpp`
- Modify: `CMakeLists.txt` (add the test file to `spky_tests`)

**Interfaces:**
- Consumes: `clampf`, `lerpf` from `engine/util/math.h`.
- Produces (later tasks rely on these exact names):
  - `enum class QuantMode { Scale, Chrom, Free }`
  - `enum ScaleId { SCALE_MIN_PENT=0, SCALE_AEOLIAN, SCALE_DORIAN, SCALE_MAJ_PENT, SCALE_LYDIAN, SCALE_WHOLE, SCALE_LIST_COUNT }`
  - `constexpr uint16_t SCALE_MASKS[SCALE_LIST_COUNT]`
  - `class Quantizer` with `void init(float sample_rate)`, `void set_scale(uint16_t mask12)`, `void set_mode(QuantMode)`, `void set_root(int semis)`, `QuantMode mode() const`, `float process(float norm)` (0..1 in, 0..1 out)

- [ ] **Step 1: Create the branch**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach && git checkout -b feat/scale-quantization
```

- [ ] **Step 2: Write the failing tests**

Create `tests/test_quantizer.cpp`:

```cpp
#include <doctest/doctest.h>
#include <cmath>
#include <initializer_list>
#include "pitch/quantizer.h"
using namespace spky;

TEST_CASE("quantizer: FREE is a bit-identical passthrough") {
    Quantizer q;
    q.init(48000.f);
    q.set_mode(QuantMode::Free);
    for (float v : {0.f, 0.123456f, 0.5f, 0.987654f, 1.f})
        CHECK(q.process(v) == v);   // exact, not Approx
}

TEST_CASE("quantizer: CHROM snaps to the nearest semitone (36-semi contract)") {
    Quantizer q;
    q.init(48000.f);
    q.set_mode(QuantMode::Chrom);
    CHECK(q.process(17.4f / 36.f) == doctest::Approx(17.f / 36.f));
    Quantizer q2;
    q2.init(48000.f);
    q2.set_mode(QuantMode::Chrom);
    CHECK(q2.process(17.6f / 36.f) == doctest::Approx(18.f / 36.f));
}

TEST_CASE("quantizer: SCALE default is dorian, ties resolve to the lower note") {
    Quantizer q;
    q.init(48000.f);
    // 18 semis: degree 6 is not in dorian; 17 and 19 are equidistant -> 17
    CHECK(q.process(0.5f) == doctest::Approx(17.f / 36.f));
}

TEST_CASE("quantizer: root shifts the allowed degrees") {
    Quantizer q;
    q.init(48000.f);
    q.set_root(1);   // dorian on root 1: degree (18-1)%12 = 5 is allowed
    CHECK(q.process(0.5f) == doctest::Approx(18.f / 36.f));
}

TEST_CASE("quantizer: every scale mask maps output onto its own degrees") {
    for (int s = 0; s < SCALE_LIST_COUNT; ++s) {
        Quantizer q;
        q.init(48000.f);
        q.set_scale(SCALE_MASKS[s]);
        for (float v : {0.f, 0.2f, 0.4f, 0.6f, 0.8f, 1.f}) {
            Quantizer fresh;
            fresh.init(48000.f);
            fresh.set_scale(SCALE_MASKS[s]);
            float semis = fresh.process(v) * 36.f;
            int k = static_cast<int>(semis + 0.5f);
            CHECK(std::fabs(semis - k) < 1e-4f);
            CHECK(((SCALE_MASKS[s] >> (k % 12)) & 1) == 1);
        }
    }
}
```

- [ ] **Step 3: Add the test file to CMake**

In `CMakeLists.txt`, inside `add_executable(spky_tests ...)`, add after `tests/smoke.cpp`:

```cmake
    tests/test_quantizer.cpp
```

- [ ] **Step 4: Run tests to verify they fail**

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake -B build && cmake --build build --target spky_tests`
Expected: compile error — `pitch/quantizer.h` not found.

- [ ] **Step 5: Write the implementation**

Create `engine/pitch/quantizer.h` (hysteresis and slew arrive in Task 2; this version quantizes statelessly):

```cpp
#pragma once
#include <cstdint>
#include <cmath>
#include "util/math.h"

namespace spky {

enum class QuantMode { Scale, Chrom, Free };

// Global scale list, ordered dark -> bright (spec: the selection knob sweep
// is a brightness axis). Bit i set = semitone i relative to root is allowed.
enum ScaleId {
    SCALE_MIN_PENT = 0,
    SCALE_AEOLIAN,
    SCALE_DORIAN,      // boot default
    SCALE_MAJ_PENT,
    SCALE_LYDIAN,
    SCALE_WHOLE,
    SCALE_LIST_COUNT
};

constexpr uint16_t SCALE_MASKS[SCALE_LIST_COUNT] = {
    0x04A9,  // minor pentatonic  0 3 5 7 10
    0x05AD,  // aeolian           0 2 3 5 7 8 10
    0x06AD,  // dorian            0 2 3 5 7 9 10
    0x0295,  // major pentatonic  0 2 4 7 9
    0x0AD5,  // lydian            0 2 4 6 7 9 11
    0x0555,  // whole tone        0 2 4 6 8 10
};

constexpr uint16_t CHROM_MASK = 0x0FFF;

// Scale quantizer on the pitch contract: normalized 0..1 = 36 semitones
// (3 octaves). Part applies it as the last stage of the PITCH target; voices
// later apply it to the target + V/Oct sum. FREE returns the input untouched.
class Quantizer {
public:
    static constexpr float SPAN_SEMIS = 36.f;

    void init(float sample_rate) { (void)sample_rate; }

    void set_scale(uint16_t mask12) { _scale = mask12; }
    void set_mode(QuantMode m)      { _mode = m; }
    void set_root(int semis)        { _root = semis; }

    QuantMode mode() const { return _mode; }

    float process(float norm) {
        if (_mode == QuantMode::Free) return norm;
        const uint16_t mask = (_mode == QuantMode::Chrom) ? CHROM_MASK : _scale;
        const float semis = clampf(norm, 0.f, 1.f) * SPAN_SEMIS;
        return static_cast<float>(nearest_note(semis, mask)) / SPAN_SEMIS;
    }

private:
    bool allowed(int k, uint16_t mask) const {
        int deg = (k - _root) % 12;
        if (deg < 0) deg += 12;
        return (mask >> deg) & 1;
    }

    // Outward search from the rounded center: the first allowed note at
    // integer distance d is the float-nearest up to the lo/hi tie, which is
    // resolved by comparing real distances (equal -> lower note wins).
    int nearest_note(float semis, uint16_t mask) const {
        const int center = static_cast<int>(semis + 0.5f);
        for (int d = 0; d <= 12; ++d) {
            const int lo = center - d, hi = center + d;
            const bool lo_ok = lo >= 0 && allowed(lo, mask);
            const bool hi_ok = hi <= 36 && allowed(hi, mask);
            if (lo_ok && hi_ok && lo != hi)
                return std::fabs(semis - static_cast<float>(hi))
                     < std::fabs(semis - static_cast<float>(lo)) ? hi : lo;
            if (lo_ok) return lo;
            if (hi_ok) return hi;
        }
        return center;  // unreachable with a non-empty mask
    }

    QuantMode _mode  = QuantMode::Scale;
    uint16_t  _scale = SCALE_MASKS[SCALE_DORIAN];
    int       _root  = 0;
};

} // namespace spky
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `cmake --build build --target spky_tests && ./build/spky_tests.exe -tc="quantizer*"`
Expected: all quantizer cases PASS. Then run the full suite once: `./build/spky_tests.exe` — everything still green (nothing else consumes the quantizer yet).

- [ ] **Step 7: Commit**

```bash
git add engine/pitch/quantizer.h tests/test_quantizer.cpp CMakeLists.txt
git commit -m "feat(engine): scale table + Quantizer core (SCALE/CHROM/FREE, root, nearest-note)"
```

---

### Task 2: Quantizer hysteresis + change slew

**Files:**
- Modify: `engine/pitch/quantizer.h`
- Test: `tests/test_quantizer.cpp` (append)

**Interfaces:**
- Consumes: Task 1's `Quantizer`.
- Produces: same public API, now stateful — `init(sample_rate)` sizes the ~40 ms slew; `process()` applies ±15-cent hysteresis and slews across config changes. Adds public `static constexpr float HYST_SEMIS = 0.30f;`. Part (Task 3) relies on: steady-state output is always exactly `note/36`; only the ~40 ms after a config change may be off-grid.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_quantizer.cpp`:

```cpp
TEST_CASE("quantizer: hysteresis holds the note ~15 cents past the midpoint") {
    Quantizer q;
    q.init(48000.f);
    q.set_mode(QuantMode::Chrom);
    CHECK(q.process(17.0f / 36.f) == doctest::Approx(17.f / 36.f));
    // 17.55 is past the 17.5 midpoint but inside the hysteresis band -> hold
    CHECK(q.process(17.55f / 36.f) == doctest::Approx(17.f / 36.f));
    // 17.7 is clearly past -> switch
    CHECK(q.process(17.7f / 36.f) == doctest::Approx(18.f / 36.f));
    // coming back: 17.55 from above stays on 18 (symmetric band)
    CHECK(q.process(17.55f / 36.f) == doctest::Approx(18.f / 36.f));
}

TEST_CASE("quantizer: config change slews ~40 ms instead of clicking") {
    Quantizer q;
    q.init(48000.f);
    for (int i = 0; i < 100; ++i) q.process(0.5f);          // settled on 17/36
    q.set_root(1);                                          // target becomes 18/36
    float first = q.process(0.5f);
    CHECK(first > 17.f / 36.f);
    CHECK(first < 18.f / 36.f);                             // mid-slew, no jump
    float prev = first, last = first;
    for (int i = 0; i < 1920; ++i) {                        // 40 ms @ 48k
        last = q.process(0.5f);
        CHECK(last >= prev - 1e-6f);                        // monotonic ramp up
        prev = last;
    }
    CHECK(last == doctest::Approx(18.f / 36.f));
}

TEST_CASE("quantizer: switching to FREE is an instant passthrough") {
    Quantizer q;
    q.init(48000.f);
    for (int i = 0; i < 100; ++i) q.process(0.5f);
    q.set_mode(QuantMode::Free);
    CHECK(q.process(0.512345f) == 0.512345f);               // exact, no slew
}

TEST_CASE("quantizer: leaving FREE slews from the last raw output") {
    Quantizer q;
    q.init(48000.f);
    q.set_mode(QuantMode::Free);
    for (int i = 0; i < 100; ++i) q.process(0.5f);
    q.set_mode(QuantMode::Scale);                           // dorian -> 17/36
    float first = q.process(0.5f);
    CHECK(first < 0.5f);
    CHECK(first > 17.f / 36.f);                             // gliding down
    for (int i = 0; i < 1920; ++i) q.process(0.5f);
    CHECK(q.process(0.5f) == doctest::Approx(17.f / 36.f));
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build --target spky_tests && ./build/spky_tests.exe -tc="quantizer*"`
Expected: the four new cases FAIL (hysteresis case gets 18 where 17 is expected; slew cases see instant jumps).

- [ ] **Step 3: Implement hysteresis + slew**

Replace the `Quantizer` class body in `engine/pitch/quantizer.h` with (enums, masks, and file header stay as in Task 1):

```cpp
class Quantizer {
public:
    static constexpr float SPAN_SEMIS = 36.f;
    static constexpr float HYST_SEMIS = 0.30f;   // switch ~15 cents past midpoint

    void init(float sample_rate) {
        _slew_len = static_cast<int>(sample_rate * 0.04f);   // ~40 ms change slew
        _slew_ctr = 0;
        _have_note = false;
        _have_out = false;
    }

    void set_scale(uint16_t mask12) { if (mask12 != _scale) { _scale = mask12; on_change(); } }
    void set_mode(QuantMode m)      { if (m != _mode)       { _mode = m;       on_change(); } }
    void set_root(int semis)        { if (semis != _root)   { _root = semis;   on_change(); } }

    QuantMode mode() const { return _mode; }

    float process(float norm) {
        if (_mode == QuantMode::Free) {
            _last_out = norm;
            _have_out = true;
            _have_note = false;
            return norm;
        }
        const uint16_t mask = (_mode == QuantMode::Chrom) ? CHROM_MASK : _scale;
        const float semis = clampf(norm, 0.f, 1.f) * SPAN_SEMIS;
        int note = nearest_note(semis, mask);
        if (_have_note && note != _last_note && allowed(_last_note, mask)) {
            const float d_last = std::fabs(semis - static_cast<float>(_last_note));
            const float d_note = std::fabs(semis - static_cast<float>(note));
            if (d_last - d_note < HYST_SEMIS) note = _last_note;   // hold
        }
        _last_note = note;
        _have_note = true;

        float out = static_cast<float>(note) / SPAN_SEMIS;
        if (_slew_ctr > 0) {
            --_slew_ctr;
            const float t = 1.f - static_cast<float>(_slew_ctr) / static_cast<float>(_slew_len);
            out = lerpf(_slew_from, out, t);
        }
        _last_out = out;
        _have_out = true;
        return out;
    }

private:
    void on_change() {
        _have_note = false;                       // re-pick without hysteresis
        if (_have_out && _mode != QuantMode::Free) {
            _slew_from = _last_out;               // soften the jump (~40 ms)
            _slew_ctr = _slew_len;
        } else {
            _slew_ctr = 0;                        // into FREE: instant passthrough
        }
    }

    bool allowed(int k, uint16_t mask) const {
        int deg = (k - _root) % 12;
        if (deg < 0) deg += 12;
        return (mask >> deg) & 1;
    }

    int nearest_note(float semis, uint16_t mask) const {
        const int center = static_cast<int>(semis + 0.5f);
        for (int d = 0; d <= 12; ++d) {
            const int lo = center - d, hi = center + d;
            const bool lo_ok = lo >= 0 && allowed(lo, mask);
            const bool hi_ok = hi <= 36 && allowed(hi, mask);
            if (lo_ok && hi_ok && lo != hi)
                return std::fabs(semis - static_cast<float>(hi))
                     < std::fabs(semis - static_cast<float>(lo)) ? hi : lo;
            if (lo_ok) return lo;
            if (hi_ok) return hi;
        }
        return center;  // unreachable with a non-empty mask
    }

    QuantMode _mode  = QuantMode::Scale;
    uint16_t  _scale = SCALE_MASKS[SCALE_DORIAN];
    int       _root  = 0;
    int       _last_note = 0;
    bool      _have_note = false;
    float     _last_out  = 0.f;
    bool      _have_out  = false;
    float     _slew_from = 0.f;
    int       _slew_ctr  = 0;
    int       _slew_len  = 1920;
};
```

- [ ] **Step 4: Run all tests to verify they pass**

Run: `cmake --build build --target spky_tests && ./build/spky_tests.exe`
Expected: full suite PASS, including all Task-1 quantizer cases (fresh instances there never trigger slew before their first `process`).

- [ ] **Step 5: Commit**

```bash
git add engine/pitch/quantizer.h tests/test_quantizer.cpp
git commit -m "feat(engine): quantizer hysteresis (15 cents) + 40 ms change slew"
```

---

### Task 3: Wire the Quantizer into Part + Instrument API

**Files:**
- Modify: `engine/parts/part.h`, `engine/parts/part.cpp`, `engine/instrument.h`
- Test: `tests/test_part.cpp` (one deliberate fix + new cases), `tests/test_instrument.cpp` (new case)

**Interfaces:**
- Consumes: Task 2's `Quantizer`, `QuantMode`, `SCALE_MASKS`, `ScaleId`.
- Produces:
  - `Part`: `Quantizer& quant()`, `float target_raw(int slot) const` (the old unquantized `target_value`); `target_value(LANE_PITCH)` and `pitch_cv()` now return the quantized value from the last `process()` call.
  - `Instrument`: `void set_scale(int scale_idx)` (global, clamped to the list), `void set_quant_mode(int p, QuantMode m)`, `void set_root(int p, int semis)` — Task 4's host dispatcher calls exactly these.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_part.cpp` (file already includes `parts/part.h`; add `#include <cmath>` at the top):

```cpp
TEST_CASE("part: SCALE mode lands pitch only on allowed dorian degrees") {
    Part p;
    p.init(48000.f, 5);
    p.set_target_active(LANE_PITCH, true);
    p.set_target_base(LANE_PITCH, 0.5f);
    p.mod().set_range(1.f);
    p.mod().set_sync_mode(SyncMode::Free);
    p.mod().set_rate(0.6f);
    float l, r;
    for (int i = 0; i < 48000; ++i) {
        p.process(l, r);
        float semis = p.pitch_cv() * 36.f;
        int k = static_cast<int>(semis + 0.5f);
        CHECK(std::fabs(semis - k) < 1e-4f);                       // on the grid
        CHECK(((SCALE_MASKS[SCALE_DORIAN] >> (k % 12)) & 1) == 1); // in dorian
    }
}

TEST_CASE("part: FREE mode restores the raw continuous pitch path") {
    Part p;
    p.init(48000.f, 5);
    p.quant().set_mode(QuantMode::Free);
    p.set_target_base(LANE_PITCH, 0.5f);
    p.set_depth(0.f);
    float l, r;
    p.process(l, r);
    CHECK(p.pitch_cv() == doctest::Approx(0.5f));   // off-grid value passes through
}
```

Append to `tests/test_instrument.cpp`:

```cpp
TEST_CASE("instrument: set_scale is global and reaches both parts") {
    Instrument inst;
    inst.init(48000.f);
    inst.set_depth(PART_A, 0.f);
    inst.set_depth(PART_B, 0.f);
    inst.set_target_base(PART_A, LANE_PITCH, 0.5f);
    inst.set_target_base(PART_B, LANE_PITCH, 0.5f);
    inst.set_scale(SCALE_WHOLE);   // 18 semis is a whole-tone degree
    std::vector<float> l(1), r(1);
    for (int i = 0; i < 4000; ++i)   // ride out the 40 ms change slew
        inst.process(nullptr, nullptr, l.data(), r.data(), 1);
    CHECK(inst.pitch_cv(PART_A) == doctest::Approx(18.f / 36.f));
    CHECK(inst.pitch_cv(PART_B) == doctest::Approx(18.f / 36.f));
}
```

- [ ] **Step 2: Fix the one existing test whose assumption changes**

In `tests/test_part.cpp`, the case `"part: DEPTH 0 pins targets to base"` checks `target_value(LANE_PITCH) == 0.5` — with Dorian as boot default that value is now quantized to 17/36. The test's intent is the raw pinning path, so pin it there explicitly. Add one line after `p.init(48000.f, 5);`:

```cpp
    p.quant().set_mode(QuantMode::Free);   // this test asserts the raw path
```

- [ ] **Step 3: Run tests to verify the new ones fail**

Run: `cmake --build build --target spky_tests && ./build/spky_tests.exe`
Expected: compile error — `Part::quant()` does not exist yet.

- [ ] **Step 4: Implement Part changes**

`engine/parts/part.h` — add the include after `"mod/super_modulator.h"`:

```cpp
#include "pitch/quantizer.h"
```

Add to the public section (after `SuperModulator& mod() ...`):

```cpp
    Quantizer& quant() { return _quant; }
```

Add below the `target_value` declaration:

```cpp
    float target_raw(int slot) const;          // base + mod*depth, unquantized
```

Change `target_value` from an inline-documented declaration to plain declaration (it stays `float target_value(int slot) const;`). Add to the private section:

```cpp
    Quantizer _quant;
    float     _pitch_q = 0.f;
```

`engine/parts/part.cpp` — replace the whole file body:

```cpp
#include "parts/part.h"

using namespace spky;

void Part::init(float sample_rate, uint32_t seed_base) {
    _sr = sample_rate;
    _mod.init(sample_rate, seed_base);
    _tone.init(sample_rate);
    _engine = &_tone;
    _gate_len = static_cast<int>(sample_rate * 0.005f);
    _gate_ctr = 0;
    _quant.init(sample_rate);                  // boots Dorian / SCALE / root 0
    _pitch_q = _quant.process(target_raw(LANE_PITCH));
}

float Part::target_raw(int slot) const {
    float mod = _active[slot] ? _mod.lane_output(slot) * _depth * _tdepth[slot] : 0.f;
    return clampf(_base[slot] + mod, 0.f, 1.f);
}

float Part::target_value(int slot) const {
    return slot == LANE_PITCH ? _pitch_q : target_raw(slot);
}

void Part::process(float& outL, float& outR) {
    _mod.process();

    if (_mod.lane_fired(LANE_PITCH)) _gate_ctr = _gate_len;
    if (_gate_ctr > 0) --_gate_ctr;

    float targets[LANE_COUNT];
    for (int i = 0; i < LANE_COUNT; ++i) targets[i] = target_raw(i);
    targets[LANE_PITCH] = _quant.process(targets[LANE_PITCH]);
    _pitch_q = targets[LANE_PITCH];

    _engine->set_targets(targets, _tune);
    if (_mod.lane_fired(LANE_PITCH)) _engine->trigger(targets[LANE_PITCH]);
    _engine->process(outL, outR);
}
```

- [ ] **Step 5: Implement Instrument API**

`engine/instrument.h` — add after `set_target_depth(...)`:

```cpp
    void set_scale(int scale_idx) {
        if (scale_idx < 0) scale_idx = 0;
        if (scale_idx >= SCALE_LIST_COUNT) scale_idx = SCALE_LIST_COUNT - 1;
        for (auto& part : _parts) part.quant().set_scale(SCALE_MASKS[scale_idx]);
    }
    void set_quant_mode(int p, QuantMode m) { _parts[p].quant().set_mode(m); }
    void set_root(int p, int semis)         { _parts[p].quant().set_root(semis); }
```

- [ ] **Step 6: Run all tests to verify they pass**

Run: `cmake --build build --target spky_tests && ./build/spky_tests.exe`
Expected: full suite PASS — including the untouched lane/step/evolve determinism tests (the quantizer sits after the lanes and cannot affect them).

- [ ] **Step 7: Commit**

```bash
git add engine/parts/part.h engine/parts/part.cpp engine/instrument.h tests/test_part.cpp tests/test_instrument.cpp
git commit -m "feat(engine): quantize PITCH target in Part; Instrument scale/mode/root API"
```

---

### Task 4: Host scenario actions + demo scene

**Files:**
- Modify: `host/render/scenario.cpp`, `host/render/scenarios/melody_then_drift.json`
- Create: `host/render/scenarios/dorian_vs_drift.json`
- Test: `tests/test_scenario.cpp` (append)

**Interfaces:**
- Consumes: `Instrument::set_scale(int)`, `set_quant_mode(int, QuantMode)`, `set_root(int, int)` from Task 3; `Event` fields `svalue`/`ivalue` (already parsed generically by `parse_event`).
- Produces: scenario actions `set_scale` (string value: `min_pent|aeolian|dorian|maj_pent|lydian|whole`), `set_quant_mode` (string value: `scale|chrom|free`), `set_root` (int `ivalue`, semitones).

- [ ] **Step 1: Write the failing test**

Append to `tests/test_scenario.cpp` (it already includes `render/scenario.h`; ensure `#include "mod/lane_id.h"` is available via `instrument.h`):

```cpp
TEST_CASE("scenario: quantizer actions reach the instrument") {
    Instrument inst;
    inst.init(48000.f);
    Event depth;  depth.action = "set_depth"; depth.part = 0; depth.value = 0.f;
    apply_event(inst, depth);
    Event base;   base.action = "set_target_base"; base.part = 0;
    base.slot = LANE_PITCH; base.value = 0.5f;
    apply_event(inst, base);

    float l = 0.f, r = 0.f;
    for (int i = 0; i < 4000; ++i) inst.process(nullptr, nullptr, &l, &r, 1);
    CHECK(inst.pitch_cv(0) == doctest::Approx(17.f / 36.f));   // boot dorian

    Event scale;  scale.action = "set_scale"; scale.svalue = "whole";
    apply_event(inst, scale);
    for (int i = 0; i < 4000; ++i) inst.process(nullptr, nullptr, &l, &r, 1);
    CHECK(inst.pitch_cv(0) == doctest::Approx(18.f / 36.f));   // whole tone

    Event mode;   mode.action = "set_quant_mode"; mode.part = 0; mode.svalue = "free";
    apply_event(inst, mode);
    inst.process(nullptr, nullptr, &l, &r, 1);
    CHECK(inst.pitch_cv(0) == doctest::Approx(0.5f));          // raw passthrough
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target spky_tests && ./build/spky_tests.exe -tc="scenario*"`
Expected: the new case FAILS on the `set_scale` check — unknown actions are ignored by design, so pitch stays 17/36.

- [ ] **Step 3: Implement the dispatcher actions**

In `host/render/scenario.cpp`, add below `parse_sync`:

```cpp
static QuantMode parse_qmode(const std::string& s) {
    if (s == "chrom") return QuantMode::Chrom;
    if (s == "free")  return QuantMode::Free;
    return QuantMode::Scale;
}

static int parse_scale_name(const std::string& s) {
    if (s == "min_pent") return SCALE_MIN_PENT;
    if (s == "aeolian")  return SCALE_AEOLIAN;
    if (s == "maj_pent") return SCALE_MAJ_PENT;
    if (s == "lydian")   return SCALE_LYDIAN;
    if (s == "whole")    return SCALE_WHOLE;
    return SCALE_DORIAN;   // "dorian" and anything unknown -> the default
}
```

In `apply_event`, add before the closing comment:

```cpp
    else if (a == "set_scale")      inst.set_scale(parse_scale_name(e.svalue));
    else if (a == "set_quant_mode") inst.set_quant_mode(e.part, parse_qmode(e.svalue));
    else if (a == "set_root")       inst.set_root(e.part, e.ivalue);
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake --build build --target spky_tests && ./build/spky_tests.exe`
Expected: full suite PASS.

- [ ] **Step 5: Keep the old demo identical (explicit FREE opt-out)**

In `host/render/scenarios/melody_then_drift.json`, add to the top of the `"init"` array:

```json
    {"_comment":"Pre-scale-era scene: opt out of the Dorian boot default explicitly."},
    {"action":"set_quant_mode","part":0,"value":"free"},
    {"action":"set_quant_mode","part":1,"value":"free"},
```

- [ ] **Step 6: Create the demo scene**

Create `host/render/scenarios/dorian_vs_drift.json`:

```json
{
  "sample_rate": 48000,
  "bpm": 120,
  "duration_s": 24,
  "init": [
    {"_comment":"PART A - fixed 16-step melody, quantized to the global Dorian scale (boot default, no action needed)."},
    {"action":"set_sync_mode","part":0,"value":"sync"},
    {"action":"set_rate","part":0,"value":0.375},
    {"action":"set_step","part":0,"flag":true,"ivalue":16},
    {"action":"set_shape","part":0,"value":0.25},
    {"action":"set_range","part":0,"value":0.65},
    {"action":"set_smooth","part":0,"value":0.03},
    {"action":"set_probability","part":0,"value":1.0},
    {"action":"set_evolve","part":0,"value":0.0},
    {"action":"set_depth","part":0,"value":1.0},
    {"action":"set_target_active","part":0,"slot":2,"flag":true},
    {"action":"set_target_base","part":0,"slot":2,"value":0.45},
    {"action":"set_target_active","part":0,"slot":4,"flag":false},
    {"action":"set_target_base","part":0,"slot":4,"value":0.6},

    {"_comment":"PART B - slow flowing voice, starts consonant in the same Dorian."},
    {"action":"set_sync_mode","part":1,"value":"free"},
    {"action":"set_rate","part":1,"value":0.25},
    {"action":"set_shape","part":1,"value":0.6},
    {"action":"set_range","part":1,"value":0.5},
    {"action":"set_smooth","part":1,"value":0.4},
    {"action":"set_depth","part":1,"value":1.0},
    {"action":"set_target_active","part":1,"slot":2,"flag":true},
    {"action":"set_target_base","part":1,"slot":2,"value":0.35},
    {"action":"set_target_active","part":1,"slot":4,"flag":false},
    {"action":"set_target_base","part":1,"slot":4,"value":0.4}
  ],
  "events": [
    {"_comment":"Bars 1-4: both parts sit in Dorian - consonant."},
    {"t":8.0,"action":"set_quant_mode","part":1,"value":"chrom"},
    {"_comment":"8s: part B goes chromatic - drift now steps in semitones."},
    {"t":12.0,"action":"set_evolve","part":1,"value":0.4},
    {"t":16.0,"action":"set_quant_mode","part":1,"value":"free"},
    {"_comment":"16s: part B unquantized - continuous drift against the Dorian melody."},
    {"t":20.0,"action":"set_evolve","part":1,"value":0.7}
  ]
}
```

- [ ] **Step 7: Render both scenes and verify**

```bash
cmake --build build --target render
./build/render.exe host/render/scenarios/dorian_vs_drift.json renders/dorian_vs_drift.wav renders/dorian_vs_drift.csv
./build/render.exe host/render/scenarios/melody_then_drift.json renders/melody_then_drift.wav renders/melody_then_drift.csv
```

Expected: both print `wrote ... frames`. Spot-check the CSV: in `renders/dorian_vs_drift.csv` the `a_pcv` column times 36 must always be within 0.0018 of an integer whose degree mod 12 is in {0,2,3,5,7,9,10} (the CSV writes pitch CV with `%.4f`, so ×36 carries up to ±0.0018 of rounding — a tighter 0.001 bound reports false off-grid hits at exact degrees; the passing `scenario` TEST_CASE is the authoritative on-grid check); in the last third (t > 16 s) `b_pcv` shows off-grid values. Listen to the WAV — part A is a Dorian melody, part B drifts against it after 16 s.

- [ ] **Step 8: Commit**

```bash
git add host/render/scenario.cpp host/render/scenarios/melody_then_drift.json host/render/scenarios/dorian_vs_drift.json tests/test_scenario.cpp
git commit -m "feat(host): set_scale/set_quant_mode/set_root actions + dorian_vs_drift demo"
```

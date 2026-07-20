# Spotykach FX (M1.6) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port the original firmware's FLUX (tape echo) and GRIT (drive/reduce) as per-part, fully modulatable effects, add a shared ambient reverb with per-part modulatable sends, and expose 5 curated FX modulation targets per part — all verified in the desktop renderer.

**Architecture:** New `engine/fx/` directory holding portable ports of the original `Drive`/`Reduce`/`EchoDelay` plus a `ReverbSc`-based `AmbientReverb`; `Part` gains a second target row (5 FX targets tapped from the existing lanes, fixed 1:1 lane→target mapping); `Instrument` sums per-part post-FX sends into the shared reverb after the part mix. DaisySP becomes an `engine/` dependency (portable C++, still **no libDaisy**).

**Tech Stack:** C++17, CMake + Ninja + clang (via `env.sh`), doctest, DaisySP (vendored submodule at `lib/DaisySP`), nlohmann/json (host only).

**Spec:** `docs/superpowers/specs/2026-07-11-spotykach-fx-design.md` (residency repo). UX/gesture sections of the spec are M6 scope — this plan implements engine + host only.

## Global Constraints

- **Repo:** all code changes in `C:\Users\bernd\Documents\AI\Spotykach` (the firmware fork, branch `main`). This plan file lives in the residency repo.
- **Build/test command** (run from the fork root, every task):
  `source env.sh && cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure`
- **Engine purity:** `engine/` must compile with **no libDaisy include**. DaisySP includes are allowed in `engine/fx/` only (spec acceptance criterion).
- **No heap in `engine/`:** delay buffers and the reverb object are injected by the host (`FxMem` pattern). Desktop: static arrays; Daisy (M6): SDRAM.
- **`AmbientReverb` is ~530 KB** (ReverbSc's `aux_[98936]` floats + PitchShifter's two 16384-sample delay lines are inline members). **Never stack-allocate it** — always `static` in tests and host.
- Echo buffer length: `240000` samples per channel (5 s @ 48 kHz), 4 buffers total (2 parts × stereo) ≈ 3.7 MB — static arrays on desktop.
- All normalized parameters are `0..1` floats, clamped at the API boundary (existing convention).
- Existing tests must stay green after every task.
- Determinism invariant: identical scenario ⇒ bit-identical WAV.
- Commit messages follow repo style (`feat(engine):`, `feat(host):`, `docs:`, `test(engine):`) and end with:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`
- DaisySP submodule must be checked out (`lib/DaisySP/Source/` exists — it does on this machine; if missing: `git submodule update --init lib/DaisySP`).

## Locked design decisions (verified against the codebase 2026-07-11)

These resolve points the spec left open; do not re-litigate during implementation:

1. **FX MIX is a linear (equal-gain) crossfade** `out = dry + (wet − dry) × m`, **not** the original square-law `XFade`. Dry and wet are correlated here (wet ≈ dry at low intensities), and the square-law sums to > 1 for correlated signals (at m=0.5 it would boost 1.5×) which breaks the bit-exact-bypass acceptance criterion. The square-law `XFade` **is** kept inside Drive/Reduce (per-effect GRIT MIX), exactly as in the original.
2. **`PartFx` is skipped entirely when both blocks are off** (both SoftSwitches idle and not switched on) → bypass is trivially bit-exact. The reverb send tap still runs (it taps the dry signal then).
3. **FX target boot bases** (index = `FxTargetId`): `{GRIT_INT: 0.3, FLUX_TIME: 0.4, FX_MIX: 1.0, REV_SEND: 0.25, FLUX_FB: 0.45}`. FX MIX boots at **1.0** (spec doesn't name it; the per-effect mixes FLUX MIX/GRIT MIX 0.5 control the blend, and an off block is bypass anyway — a chain mix < 1 would just halve every effect). All FX target modulations boot **inactive**, depths 1.0.
4. **GRIT MIX applies to both modes** (Drive and Reduce). The original kept a separate mix per mode; the spec defines a single static GRIT MIX layer param, so one value drives both.
5. **`Instrument::init(float)` keeps working without FX memory** (no reverb, no echo buffers → FX chain absent, flux can't engage, reverb setters no-op). Existing tests stay untouched; the full chain comes via a new `init(float, const FxMem&)` overload.
6. **FX target values are NOT quantized** — `fx_target_value()` uses the raw `base + lane × depth × master_depth` formula (like `target_raw`), no pitch quantizer involvement.
7. **PartFx smoothers prime on first process call** (snap to the first computed target values instead of slewing from boot constants) — prevents a phantom reverb swell at t=0 and makes the bypass test exact.
8. **Parameter mappings:**
   - FLUX TIME: `daisysp::fmap(v, 0.05f, 5.f, Mapping::LOG)` → 50 ms…5 s exponential (v=0.5 → exactly 0.5 s)
   - FLUX FEEDBACK: `v * 1.1f` (soft clip catches > 1, as original)
   - FLUX MIX (static): `dbfs2lin(fmap(v, -40.f, 0.f))` (original curve)
   - GRIT INTENSITY: original Drive/Reduce curves unchanged
   - REVERB SEND: `sinf(v * PI/2)` equal-power onto the wet path
   - REVERB SIZE: `_rev.SetFeedback(fmap(v, 0.4f, 0.99f))`
   - REVERB TONE: `_rev.SetLpFreq(fmap(v, 500.f, 16000.f, Mapping::LOG))`
   - SHIMMER: pitch shifter (+12 st) on the mono-summed previous wet frame, fed back into the reverb input at `shimmer × 0.5` (0.5 = fixed headroom); **skipped entirely at shimmer == 0**.
9. **`daisysp::PitchShifter` uses a global static RNG** (`myrand()` in `pitchshifter.h`). One shared reverb instance → fine per process run; tests must never assert golden shimmer values, only within-test comparisons.

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `CMakeLists.txt` | modify | `daisysp_min` static lib (5 DaisySP sources), new engine/fx + test sources |
| `engine/fx/fx_util.h` | create | `XFade`, `SoftSwitch` + Hann table — ports of `src/core/{xfade,softswitch,hann}.h`, libDaisy-free |
| `engine/fx/grit.h/.cpp` | create | `GritDrive`, `GritReduce` (ports of `src/core/fx.drive.*`, `fx.reduce.*`), `Grit` mode switch + SoftSwitch |
| `engine/fx/flux.h/.cpp` | create | `DeLine`, `TapeBpf` (BPF12 @ 800 Hz/Q 0.1), `EchoDelay` (ports of `src/core/{deline,biquad,echo}.h`), `Flux` wrapper |
| `engine/fx/reverb.h/.cpp` | create | `AmbientReverb` = ReverbSc + shimmer PitchShifter |
| `engine/fx/part_fx.h/.cpp` | create | `FxTargetId`, `FxBlock`, `PartFx` chain GRIT→FLUX→FX MIX + send tap |
| `engine/parts/part.h/.cpp` | modify | second target row, `fx_target_value()`, 4-output `process()` |
| `engine/instrument.h/.cpp` | modify | `FxMem`, init overload, FX API setters, send summing + reverb |
| `host/render/scenario.cpp` | modify | 10 new scenario actions |
| `host/render/main.cpp` | modify | static FX memory, `FxMem` wiring, 5 CSV columns per part |
| `host/render/scenarios/dub_delay.json` | create | demo: STEP lane on FLUX TIME + feedback swells |
| `host/render/scenarios/ambient_wash.json` | create | demo: breathing REVERB SEND + shimmer |
| `tests/test_fx_deps.cpp` | create | DaisySP-on-desktop sanity |
| `tests/test_fx_util.cpp` | create | XFade / SoftSwitch |
| `tests/test_grit.cpp` | create | Drive/Reduce/bypass/click-free |
| `tests/test_flux.cpp` | create | delay accuracy, feedback decay, bypass |
| `tests/test_reverb.cpp` | create | silence, stereo tail, shimmer skip |
| `tests/test_part_fx.cpp` | create | chain bypass, FX MIX law, send tap |
| `tests/test_part.cpp` | modify | fx target routing tests |
| `tests/test_instrument.cpp` | modify | bypass acceptance, reverb audible |
| `tests/test_scenario.cpp` | modify | new actions reach the instrument |
| `docs/roadmap.md`, `README.md`, `THIRD_PARTY.md` | modify | M1.6 status, DaisySP-on-desktop licensing note |

---

### Task 1: DaisySP desktop build (`daisysp_min`)

**Files:**
- Modify: `CMakeLists.txt`
- Test: `tests/test_fx_deps.cpp`

**Interfaces:**
- Produces: CMake target `daisysp_min` (PUBLIC include dirs `lib/DaisySP/Source`, `lib/DaisySP/DaisySP-LGPL/Source`) linked into `spky_tests` and `render`. Later tasks include DaisySP headers as `#include "Effects/overdrive.h"`, `#include "Effects/reverbsc.h"`, `#include "Utility/dsp.h"`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_fx_deps.cpp`:

```cpp
#include <doctest/doctest.h>
#include "Effects/overdrive.h"
#include "Effects/decimator.h"
#include "Effects/sampleratereducer.h"
#include "Effects/pitchshifter.h"
#include "Effects/reverbsc.h"

// Sanity: the DaisySP modules the FX chain needs compile and run on desktop
// (clang, no ARM). ReverbSc/PitchShifter are huge objects -> static, never stack.
TEST_CASE("daisysp: fx modules init and process on desktop") {
    daisysp::Overdrive od;
    od.Init();
    od.SetDrive(0.4f);
    CHECK(od.Process(0.5f) == od.Process(0.5f));   // stateless

    daisysp::Decimator dec;
    dec.Init();
    dec.SetDownsampleFactor(0.5f);
    (void)dec.Process(0.3f);

    daisysp::SampleRateReducer srr;
    srr.Init();
    srr.SetFreq(0.3f);
    (void)srr.Process(0.3f);

    static daisysp::ReverbSc rev;
    REQUIRE(rev.Init(48000.f) == 0);
    rev.SetFeedback(0.85f);
    rev.SetLpFreq(10000.f);
    float wl = 0.f, wr = 0.f;
    rev.Process(1.f, 1.f, &wl, &wr);
    float energy = 0.f;
    for (int i = 0; i < 9600; ++i) {
        rev.Process(0.f, 0.f, &wl, &wr);
        energy += wl * wl + wr * wr;
    }
    CHECK(energy > 0.f);                            // impulse leaves a tail

    static daisysp::PitchShifter ps;
    ps.Init(48000.f);
    ps.SetTransposition(12.f);
    float in = 0.5f;
    (void)ps.Process(in);
}
```

- [ ] **Step 2: Add the CMake pieces and confirm the test fails first**

Run before editing CMake: `source env.sh && cmake --build build 2>&1 | tail -3` — the new test file is not referenced yet, so nothing changes (this is the "fail" state: test not built). Then edit `CMakeLists.txt`. After the `nlohmann_json` block, insert:

```cmake
# --- DaisySP (vendored submodule; only the modules the FX chain needs) ---
add_library(daisysp_min STATIC
    lib/DaisySP/Source/Effects/overdrive.cpp
    lib/DaisySP/Source/Effects/decimator.cpp
    lib/DaisySP/Source/Effects/sampleratereducer.cpp
    lib/DaisySP/Source/Control/phasor.cpp
    lib/DaisySP/DaisySP-LGPL/Source/Effects/reverbsc.cpp
)
target_include_directories(daisysp_min PUBLIC
    lib/DaisySP/Source
    lib/DaisySP/DaisySP-LGPL/Source
    PRIVATE
    lib/DaisySP/Source/Utility
    lib/DaisySP/Source/Control
    lib/DaisySP/Source/Effects
    lib/DaisySP/DaisySP-LGPL/Source/Effects
)
```

(The PRIVATE dirs exist because DaisySP's own `.cpp` files use flat includes like `#include "dsp.h"`.)

Add `tests/test_fx_deps.cpp` to the `spky_tests` source list, and extend both link lines:

```cmake
target_link_libraries(spky_tests PRIVATE spky_engine doctest nlohmann_json daisysp_min)
```
```cmake
target_link_libraries(render PRIVATE nlohmann_json daisysp_min)
```

- [ ] **Step 3: Build and run**

Run: `source env.sh && cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS (all existing tests + the new one).

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt tests/test_fx_deps.cpp
git commit -m "build: compile minimal DaisySP module set for the desktop host (M1.6)"
```

### Task 2: `engine/fx/fx_util.h` — XFade + SoftSwitch ports

**Files:**
- Create: `engine/fx/fx_util.h`
- Modify: `CMakeLists.txt` (add `tests/test_fx_util.cpp` to `spky_tests`)
- Test: `tests/test_fx_util.cpp`

**Interfaces:**
- Produces: `spky::XFade` (`SetStage(float)`, `Stage()`, `Process(l0,l1,r0,r1,out0,out1)`) and `spky::SoftSwitch` (`init(float sr)`, `set_on(bool, bool immediate=false)`, `is_on()`, `is_idle()`, `float process(bool inverse=false)`). Used by Tasks 3, 4, 6.
- Consumes: nothing (header-only, no DaisySP).

- [ ] **Step 1: Write the failing test**

Create `tests/test_fx_util.cpp`:

```cpp
#include <doctest/doctest.h>
#include "fx/fx_util.h"
using namespace spky;

TEST_CASE("xfade: stage 0 is exactly lhs, stage 1 is exactly rhs") {
    XFade x;
    float o0, o1;
    x.SetStage(0.f);
    x.Process(0.25f, -0.5f, 0.9f, 0.9f, o0, o1);
    CHECK(o0 == 0.25f);
    CHECK(o1 == -0.5f);
    x.SetStage(1.f);
    x.Process(0.25f, -0.5f, 0.9f, 0.7f, o0, o1);
    CHECK(o0 == 0.9f);
    CHECK(o1 == 0.7f);
}

TEST_CASE("softswitch: rises to 1 within ~4 ms, falls back to idle") {
    SoftSwitch s;
    s.init(48000.f);
    CHECK(s.process() == 0.f);
    CHECK(s.is_idle());
    s.set_on(true);
    for (int i = 0; i < 300; ++i) s.process();   // 4 ms = 192 samples
    CHECK(s.process() == 1.f);
    CHECK(!s.is_idle());
    s.set_on(false);
    for (int i = 0; i < 300; ++i) s.process();
    CHECK(s.process() == 0.f);
    CHECK(s.is_idle());
}

TEST_CASE("softswitch: immediate flag jumps straight to hold") {
    SoftSwitch s;
    s.init(48000.f);
    s.set_on(true, true);
    CHECK(s.process() == 1.f);
}
```

- [ ] **Step 2: Run to verify it fails**

Add `tests/test_fx_util.cpp` to the `spky_tests` sources in `CMakeLists.txt`, then:
Run: `source env.sh && cmake -S . -B build && cmake --build build 2>&1 | tail -5`
Expected: FAIL — `fx/fx_util.h: No such file or directory`.

- [ ] **Step 3: Write the implementation**

Create `engine/fx/fx_util.h` — verbatim ports of `src/core/xfade.h`, `src/core/hann.h`, `src/core/softswitch.h` into `namespace spky`, with the `NOCOPY` macro replaced by explicit deletes, the file-scope Hann globals replaced by a function-local static, and `_iterator` initialized:

```cpp
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// Ports of the original firmware's src/core/{xfade,softswitch,hann}.h with the
// libDaisy-adjacent includes stripped. Sound-identical: same square-law
// crossfade, same 4 ms Hann micro-ASR for click-free FX switching.

namespace spky {

inline const std::array<float, 192>& hann_curve() {
    static const std::array<float, 192> table = [] {
        std::array<float, 192> t{};
        constexpr float half_pi = 1.5707963705f;
        for (size_t i = 0; i < t.size(); ++i) {
            float s = std::sin(half_pi * static_cast<float>(i)
                                       / static_cast<float>(t.size() - 1));
            t[i] = std::clamp(s * s, 0.f, 1.f);
        }
        return t;
    }();
    return table;
}

inline float hann_value_at(float norm_pos) {
    const auto& curve = hann_curve();
    float pos = static_cast<float>(curve.size() - 1) * norm_pos;
    auto ipos = static_cast<size_t>(pos);
    float frac = pos - static_cast<float>(ipos);
    size_t npos = ipos + 1 >= curve.size() ? curve.size() - 1 : ipos + 1;
    return curve[ipos] + (curve[npos] - curve[ipos]) * frac;
}

// The square law crossfade
// Adopted from Will. C. Pirkle "Designing Software Synthesizer Plugins in C++".
class XFade {
public:
    XFade() = default;
    XFade(const XFade&) = delete;
    XFade& operator=(const XFade&) = delete;

    void Process(float lhs0, float lhs1, float rhs0, float rhs1,
                 float& out0, float& out1) const {
        out0 = lhs0 * _lhs + rhs0 * _rhs;
        out1 = lhs1 * _lhs + rhs1 * _rhs;
    }

    void SetStage(float value) {
        _stage = std::clamp(value, 0.f, 1.f);
        float sq = _stage * _stage;
        _lhs = 1.f - sq;
        _rhs = 2.f * _stage - sq;
    }

    float Stage() const { return _stage; }

private:
    float _stage = 0.f;
    float _lhs = 1.f;
    float _rhs = 0.f;
};

// Four-millisecond micro ASR envelope for click-free on/off switching.
class SoftSwitch {
public:
    SoftSwitch() = default;
    SoftSwitch(const SoftSwitch&) = delete;
    SoftSwitch& operator=(const SoftSwitch&) = delete;

    void init(float sample_rate) { _kof = 1.f / (0.004f * sample_rate); }

    void set_on(bool on, bool immediate = false) {
        _on = on;
        if (immediate) _stage = _on ? Stage::hold : Stage::idle;
    }

    bool is_on() const { return _on; }
    bool is_idle() const { return _stage == Stage::idle; }

    float process(bool inverse = false) {
        switch (_stage) {
            case Stage::idle:
                _out = 0.f;
                _iterator = 0;
                if (_on) _stage = Stage::rise;
                break;
            case Stage::rise:
                if (!_on) _stage = Stage::fall;
                else _out = hann_value_at(_iterator * _kof);
                if (++_iterator >= 191) _stage = Stage::hold;
                break;
            case Stage::hold:
                _out = 1.f;
                _iterator = 191;
                if (!_on) _stage = Stage::fall;
                break;
            case Stage::fall:
                if (_on) _stage = Stage::rise;
                else _out = hann_value_at(_iterator * _kof);
                if (--_iterator <= 0) _stage = Stage::idle;
                break;
        }
        return std::clamp(inverse ? 1.f - _out : _out, 0.f, 1.f);
    }

private:
    enum class Stage { idle, rise, hold, fall };

    int32_t _iterator = 0;
    float _kof = 1.f;
    float _out = 0.f;
    Stage _stage = Stage::idle;
    bool _on = false;
};

} // namespace spky
```

- [ ] **Step 4: Build and run all tests**

Run: `source env.sh && cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add engine/fx/fx_util.h tests/test_fx_util.cpp CMakeLists.txt
git commit -m "feat(engine): port XFade + SoftSwitch as portable fx utilities"
```

### Task 3: `engine/fx/grit.h/.cpp` — Drive/Reduce port + mode switch

**Files:**
- Create: `engine/fx/grit.h`, `engine/fx/grit.cpp`
- Modify: `CMakeLists.txt` (add `engine/fx/grit.cpp` to `spky_tests` AND `render` source lists; add `tests/test_grit.cpp` to `spky_tests`)
- Test: `tests/test_grit.cpp`

**Interfaces:**
- Consumes: `spky::XFade`, `spky::SoftSwitch` from `fx/fx_util.h`; `daisysp::Overdrive/Decimator/SampleRateReducer`.
- Produces: `enum class spky::GritMode : uint8_t { Drive, Reduce }` and `spky::Grit` with `init(float sr)`, `set_on(bool, bool immediate=false)`, `is_on()`, `engaged()`, `set_mode(GritMode)`, `mode()`, `set_intensity(float)`, `set_mix(float)`, `process(float& l, float& r)`. Task 6 builds the chain on `engaged()`/`process()`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_grit.cpp`:

```cpp
#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include "fx/grit.h"
using namespace spky;

static std::vector<float> sine(int n, float amp = 0.5f) {
    std::vector<float> v(n);
    for (int i = 0; i < n; ++i)
        v[i] = amp * std::sin(6.2831853f * 220.f * i / 48000.f);
    return v;
}

TEST_CASE("grit: off leaves the signal bit-exact") {
    Grit g;
    g.init(48000.f);
    auto in = sine(2000);
    for (float s : in) {
        float l = s, r = s;
        g.process(l, r);
        CHECK(l == s);
        CHECK(r == s);
    }
    CHECK(!g.engaged());
}

TEST_CASE("grit: drive distorts, reduce decimates, and they differ") {
    auto in = sine(4800);
    auto run = [&](GritMode m) {
        Grit g;
        g.init(48000.f);
        g.set_mode(m);
        g.set_intensity(0.8f);
        g.set_mix(1.f);
        g.set_on(true, true);
        std::vector<float> out;
        out.reserve(in.size());
        for (float s : in) {
            float l = s, r = s;
            g.process(l, r);
            out.push_back(l);
        }
        return out;
    };
    auto drive = run(GritMode::Drive);
    auto reduce = run(GritMode::Reduce);
    int drive_diff = 0, mode_diff = 0;
    for (size_t i = 0; i < in.size(); ++i) {
        if (std::fabs(drive[i] - in[i]) > 1e-4f) ++drive_diff;
        if (std::fabs(drive[i] - reduce[i]) > 1e-4f) ++mode_diff;
        CHECK(std::fabs(drive[i]) <= 1.5f);
        CHECK(std::fabs(reduce[i]) <= 1.5f);
    }
    CHECK(drive_diff > 1000);
    CHECK(mode_diff > 1000);
}

TEST_CASE("grit: mix 0 returns the dry signal") {
    Grit g;
    g.init(48000.f);
    g.set_intensity(0.9f);
    g.set_mix(0.f);
    g.set_on(true, true);
    auto in = sine(2000);
    for (float s : in) {
        float l = s, r = s;
        g.process(l, r);
        CHECK(l == doctest::Approx(s).epsilon(1e-6));
    }
}

TEST_CASE("grit: switching on mid-signal is click-free") {
    Grit g;
    g.init(48000.f);
    g.set_intensity(0.7f);
    g.set_mix(1.f);
    auto in = sine(9600);
    float prev = 0.f, max_delta = 0.f;
    for (int i = 0; i < (int)in.size(); ++i) {
        if (i == 4800) g.set_on(true);   // ramped, not immediate
        float l = in[i], r = in[i];
        g.process(l, r);
        if (i > 0) max_delta = std::max(max_delta, std::fabs(l - prev));
        prev = l;
    }
    // 220 Hz sine sample-to-sample delta is ~0.014; a hard switch would jump
    // by the dry/wet difference (~0.3). The 4 ms Hann ramp keeps it small.
    CHECK(max_delta < 0.1f);
}
```

- [ ] **Step 2: Run to verify it fails**

Add `engine/fx/grit.cpp` to BOTH the `spky_tests` and `render` source lists and `tests/test_grit.cpp` to `spky_tests` in `CMakeLists.txt`, then build.
Expected: FAIL — `fx/grit.h: No such file or directory`.

- [ ] **Step 3: Write the implementation**

Create `engine/fx/grit.h`:

```cpp
#pragma once
#include <cstdint>
#include "Effects/overdrive.h"
#include "Effects/decimator.h"
#include "Effects/sampleratereducer.h"
#include "fx/fx_util.h"

namespace spky {

enum class GritMode : uint8_t { Drive, Reduce };

// Port of the original firmware's Drive (src/core/fx.drive.*): Overdrive with
// intensity-coupled attenuation, square-law dry/wet mix. Original curves kept.
class GritDrive {
public:
    void init(float sample_rate);
    float intensity() const { return _intensity; }
    void set_intensity(float norm);
    float mix() const { return _mix.Stage(); }
    void set_mix(float norm) { _mix.SetStage(norm); }
    void process(float& l, float& r);

private:
    void apply();

    daisysp::Overdrive _drive;
    XFade _mix;
    float _intensity = 0.2f;
    float _attenuation = 1.f;
};

// Port of the original firmware's Reduce (src/core/fx.reduce.*): Decimator +
// SampleRateReducer. NOTE: like the original, the mono DSP objects are shared
// across L/R — part of the original sound identity, kept as-is.
class GritReduce {
public:
    void init(float sample_rate);
    float intensity() const { return _intensity; }
    void set_intensity(float norm);
    float mix() const { return _mix.Stage(); }
    void set_mix(float norm) { _mix.SetStage(norm); }
    void process(float& l, float& r);

private:
    void apply();

    daisysp::Decimator _decimator;
    daisysp::SampleRateReducer _reducer;
    XFade _mix;
    float _intensity = 0.55f;
};

// GRIT block: Drive <-> Reduce behind one click-free SoftSwitch. Replaces the
// original Fx class's grit half; the mode is set explicitly (the original's
// switch_grit_mode() toggle becomes an M6 gesture on top of set_mode()).
class Grit {
public:
    void init(float sample_rate);
    void set_on(bool on, bool immediate = false) { _sw.set_on(on, immediate); }
    bool is_on() const { return _sw.is_on(); }
    // true while audible: on, or still ramping out after switch-off
    bool engaged() const { return _sw.is_on() || !_sw.is_idle(); }
    void set_mode(GritMode m);
    GritMode mode() const { return _mode; }
    void set_intensity(float norm);   // applies to the active mode (original behavior)
    float intensity() const;
    void set_mix(float norm);         // single GRIT MIX layer param -> both modes
    void process(float& l, float& r);

private:
    GritDrive _drive;
    GritReduce _reduce;
    SoftSwitch _sw;
    GritMode _mode = GritMode::Drive;
    float _intensity = 0.3f;
    float _mix_norm = 0.5f;
};

} // namespace spky
```

Create `engine/fx/grit.cpp`:

```cpp
#include "fx/grit.h"
#include "Utility/dsp.h"
#include "util/math.h"

using namespace spky;

namespace {
inline float dbfs2lin(float db) { return daisysp::pow10f(db * 0.05f); }
inline float attenuation_for_intensity(float intensity) {
    return dbfs2lin(daisysp::fmap(intensity, -3.f, -24.f));
}
} // namespace

void GritDrive::init(float /*sample_rate*/) {
    _drive.Init();
    _drive.SetDrive(0.f);
    _mix.SetStage(0.33f);
    apply();
}

void GritDrive::set_intensity(float norm) {
    _intensity = clampf(norm, 0.f, 1.f);
    apply();
}

void GritDrive::apply() {
    _attenuation = attenuation_for_intensity(_intensity);
    _drive.SetDrive(dbfs2lin(daisysp::fmap(_intensity, -6.f, 0.f)));
}

void GritDrive::process(float& l, float& r) {
    float d0 = _drive.Process(l) * _attenuation;
    float d1 = _drive.Process(r) * _attenuation;
    _mix.Process(l, r, d0, d1, l, r);
}

void GritReduce::init(float /*sample_rate*/) {
    _decimator.Init();
    _decimator.SetBitsToCrush(16);
    _reducer.Init();
    _reducer.SetFreq(0.6f);
    _mix.SetStage(0.5f);
    apply();
}

void GritReduce::set_intensity(float norm) {
    _intensity = clampf(norm, 0.f, 1.f);
    apply();
}

void GritReduce::apply() {
    _decimator.SetDownsampleFactor(_intensity);
    _decimator.SetBitcrushFactor(
        daisysp::fmap(1.f - _intensity, 0.5f, 0.7f, daisysp::Mapping::EXP));
}

void GritReduce::process(float& l, float& r) {
    float r0 = _reducer.Process(_decimator.Process(l));
    float r1 = _reducer.Process(_decimator.Process(r));
    _mix.Process(l, r, r0, r1, l, r);
}

void Grit::init(float sample_rate) {
    _sw.init(sample_rate);
    _drive.init(sample_rate);
    _reduce.init(sample_rate);
    set_intensity(_intensity);
    set_mix(_mix_norm);
}

void Grit::set_mode(GritMode m) {
    _mode = m;
    set_intensity(_intensity);   // re-apply current values to the new mode
    set_mix(_mix_norm);
}

void Grit::set_intensity(float norm) {
    _intensity = clampf(norm, 0.f, 1.f);
    if (_mode == GritMode::Drive) _drive.set_intensity(_intensity);
    else                          _reduce.set_intensity(_intensity);
}

float Grit::intensity() const {
    return _mode == GritMode::Drive ? _drive.intensity() : _reduce.intensity();
}

void Grit::set_mix(float norm) {
    _mix_norm = clampf(norm, 0.f, 1.f);
    _drive.set_mix(_mix_norm);
    _reduce.set_mix(_mix_norm);
}

void Grit::process(float& l, float& r) {
    float k = _sw.process();
    if (_sw.is_idle()) return;   // fully off: bit-exact dry
    float gl = l, gr = r;
    if (_mode == GritMode::Drive) _drive.process(gl, gr);
    else                          _reduce.process(gl, gr);
    l = gl * k + l * (1.f - k);
    r = gr * k + r * (1.f - k);
}
```

- [ ] **Step 4: Build and run all tests**

Run: `source env.sh && cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add engine/fx/grit.h engine/fx/grit.cpp tests/test_grit.cpp CMakeLists.txt
git commit -m "feat(engine): port GRIT (Drive/Reduce) as portable per-part fx block"
```

### Task 4: `engine/fx/flux.h/.cpp` — tape echo port

**Files:**
- Create: `engine/fx/flux.h`, `engine/fx/flux.cpp`
- Modify: `CMakeLists.txt` (add `engine/fx/flux.cpp` to `spky_tests` AND `render`; add `tests/test_flux.cpp` to `spky_tests`)
- Test: `tests/test_flux.cpp`

**Interfaces:**
- Consumes: `spky::SoftSwitch` from `fx/fx_util.h`; `daisysp::fonepole/SoftClip/fmap/pow10f` from `Utility/dsp.h`.
- Produces: `spky::Flux` with `static constexpr size_t kMaxSamples = 240000`, `init(float sr, float* bufL, float* bufR)` (null bufs → permanently bypassed), `set_on(bool, bool immediate=false)`, `is_on()`, `engaged()`, `has_buffers()`, `set_time(float norm, bool immediate=false)`, `set_feedback(float norm)`, `set_mix(float norm)`, `process(float& l, float& r)`. Task 6 consumes `engaged()`/`set_time`/`set_feedback`/`process`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_flux.cpp`:

```cpp
#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include "fx/flux.h"
using namespace spky;

// 5 s stereo of echo memory, shared by all cases in this file; Flux::init
// resets the lines, so every TEST_CASE starts from silence.
static float s_buf_l[Flux::kMaxSamples];
static float s_buf_r[Flux::kMaxSamples];

// Feed a unit impulse, return the index of the first echo arrival.
static int first_echo_index(Flux& f, int n) {
    for (int i = 0; i < n; ++i) {
        float l = (i == 0) ? 1.f : 0.f;
        float r = l;
        f.process(l, r);
        if (i > 100 && std::fabs(l) > 1e-3f) return i;
    }
    return -1;
}

TEST_CASE("flux: echo arrives at the mapped delay time (0.5 -> 0.5 s)") {
    Flux f;
    f.init(48000.f, s_buf_l, s_buf_r);
    f.set_on(true, true);
    f.set_time(0.5f, true);          // fmap LOG 0.05..5 -> exactly 0.5 s
    f.set_feedback(0.f);
    f.set_mix(1.f);                  // 0 dB
    int idx = first_echo_index(f, 30000);
    CHECK(idx >= 23990);
    CHECK(idx <= 24100);
}

TEST_CASE("flux: time 0 maps to 50 ms") {
    Flux f;
    f.init(48000.f, s_buf_l, s_buf_r);
    f.set_on(true, true);
    f.set_time(0.f, true);
    f.set_feedback(0.f);
    f.set_mix(1.f);
    int idx = first_echo_index(f, 5000);
    CHECK(idx >= 2390);              // 0.05 s = 2400 samples
    CHECK(idx <= 2500);
}

TEST_CASE("flux: feedback produces decaying repeats") {
    Flux f;
    f.init(48000.f, s_buf_l, s_buf_r);
    f.set_on(true, true);
    f.set_time(0.5f, true);
    f.set_feedback(0.45f);           // -> 0.495 linear
    f.set_mix(1.f);
    std::vector<float> out(80000);
    for (int i = 0; i < (int)out.size(); ++i) {
        float l = (i == 0) ? 1.f : 0.f;
        float r = l;
        f.process(l, r);
        out[i] = l;
    }
    auto peak_around = [&](int center) {
        float p = 0.f;
        for (int i = center - 600; i < center + 600; ++i)
            p = std::max(p, std::fabs(out[i]));
        return p;
    };
    float p1 = peak_around(24000);
    float p2 = peak_around(48000);
    float p3 = peak_around(72000);
    CHECK(p1 > 1e-3f);
    CHECK(p2 < p1);
    CHECK(p3 < p2);
}

TEST_CASE("flux: off is bit-exact dry") {
    Flux f;
    f.init(48000.f, s_buf_l, s_buf_r);
    for (int i = 0; i < 2000; ++i) {
        float s = std::sin(0.01f * i) * 0.4f;
        float l = s, r = s;
        f.process(l, r);
        CHECK(l == s);
        CHECK(r == s);
    }
}

TEST_CASE("flux: null buffers never engage") {
    Flux f;
    f.init(48000.f, nullptr, nullptr);
    f.set_on(true, true);
    CHECK(!f.has_buffers());
    CHECK(!f.engaged());
    float l = 0.5f, r = 0.5f;
    f.process(l, r);
    CHECK(l == 0.5f);
}
```

- [ ] **Step 2: Run to verify it fails**

Add `engine/fx/flux.cpp` to `spky_tests` and `render`, `tests/test_flux.cpp` to `spky_tests`, then build.
Expected: FAIL — `fx/flux.h: No such file or directory`.

- [ ] **Step 3: Write the implementation**

Create `engine/fx/flux.h`. `DeLine` and `EchoDelay` are verbatim ports of `src/core/deline.h` / `src/core/echo.h`; `TapeBpf` replaces the general `BiquadCascade` with the one configuration the echo actually uses (single band-pass section, 800 Hz, the original's effective Q of 0.1 — `SetParams(800.f, 0.f)` clamps q to 0.1). Coefficient math copied from `src/core/biquad.cpp`:

```cpp
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include "Utility/dsp.h"
#include "fx/fx_util.h"

namespace spky {

// Port of src/core/deline.h (interpolating delay line over an injected buffer).
template <typename T, size_t max_size>
class DeLine {
public:
    DeLine() = default;
    DeLine(const DeLine&) = delete;
    DeLine& operator=(const DeLine&) = delete;

    void Init(float* buf) {
        line_ = buf;
        Reset();
    }

    void Reset() {
        std::memset(line_, 0, max_size * sizeof(T));
        write_ptr_ = 0;
        delay_ = 1;
        frac_ = 0.f;
    }

    void SetDelay(float delay) {
        int32_t int_delay = static_cast<int32_t>(delay);
        frac_ = delay - static_cast<float>(int_delay);
        delay_ = int_delay % static_cast<int32_t>(max_size);
    }

    void Write(T sample) {
        line_[write_ptr_] = sample;
        write_ptr_ = (write_ptr_ - 1 + max_size) % max_size;
    }

    T Read() const {
        T a = line_[(write_ptr_ + delay_) % max_size];
        T b = line_[(write_ptr_ + delay_ + 1) % max_size];
        return a + (b - a) * frac_;
    }

private:
    float frac_ = 0.f;
    int32_t write_ptr_ = 0;
    int32_t delay_ = 1;
    T* line_ = nullptr;
};

// The echo's tape band-limit: the one biquad the original chain uses —
// a single band-pass section at 800 Hz with the effective Q of 0.1 that
// src/core/echo.h's SetParams(800, 0) produced. Transposed direct form 2;
// coefficient math from src/core/biquad.cpp.
class TapeBpf {
public:
    void Init(float sample_rate) {
        constexpr float pi = 3.14159265358979f;
        constexpr float cutoff_hz = 800.f;
        constexpr float q = 0.1f;
        const float k = std::tan(pi * cutoff_hz / sample_rate);
        const float ksq = k * k;
        const float norm = 1.f / (1.f + (k / q) + ksq);
        b0_ = (k / q) * norm;
        a1_ = 2.f * (ksq - 1.f) * norm;
        a2_ = (1.f - (k / q) + ksq) * norm;
        s1_ = s2_ = 0.f;
    }

    float Process(float in) {
        // b1 = 0, b2 = -b0 for the band-pass case
        float y = b0_ * in + s1_;
        s1_ = s2_ - a1_ * y;
        s2_ = -b0_ * in - a2_ * y;
        return y;
    }

private:
    float b0_ = 0.f, a1_ = 0.f, a2_ = 0.f;
    float s1_ = 0.f, s2_ = 0.f;
};

// Port of src/core/echo.h (Nick Donaldson / Infrasonic Audio): tape-ish echo.
// Feedback unbounded but soft-clipped, output full-wet, band-passed, delay
// time changes slewed by a one-pole (the "tape" pitch behavior — under
// modulation this slew IS the feature: FLOW = wobble, STEP = dub pitch jumps).
template <size_t max_size>
class EchoDelay {
public:
    EchoDelay() = default;
    EchoDelay(const EchoDelay&) = delete;
    EchoDelay& operator=(const EchoDelay&) = delete;

    void Init(float sample_rate, float* buf) {
        sample_rate_ = sample_rate;
        delay_line_.Init(buf);
        bpf_.Init(sample_rate);
        delay_time_current_ = delay_time_target_ = 0.05f;
        feedback_ = 0.f;
    }

    // Approximate lag (smoothing) for delay-time changes, in seconds.
    void SetLagTime(float time_s) {
        delay_smooth_coef_ = (time_s <= 0.f || sample_rate_ <= 0.f)
            ? 1.f
            : daisysp::fmin(1.f / (time_s * sample_rate_), 1.f);
    }

    void SetDelayTime(float time_s, bool immediately = false) {
        delay_time_target_ = time_s;
        if (immediately) delay_time_current_ = time_s;
    }

    void SetFeedback(float feedback) { feedback_ = feedback; }
    float Feedback() const { return feedback_; }

    float Process(float in) {
        daisysp::fonepole(delay_time_current_, delay_time_target_,
                          delay_smooth_coef_);
        delay_line_.SetDelay(delay_time_current_ * sample_rate_);
        float out = delay_line_.Read();
        out = bpf_.Process(out);
        out = daisysp::SoftClip(out);
        delay_line_.Write(out * feedback_ + in);
        return out;
    }

private:
    float sample_rate_ = 48000.f;
    float delay_time_current_ = 0.05f;
    float delay_time_target_ = 0.05f;
    float delay_smooth_coef_ = 1.f;
    float feedback_ = 0.f;

    DeLine<float, max_size> delay_line_;
    TapeBpf bpf_;
};

// FLUX block: the stereo tape echo behind a click-free SoftSwitch, echo added
// onto the signal at FLUX MIX (original topology: send-style, full-wet echo).
class Flux {
public:
    static constexpr size_t kMaxSamples = 240000;   // 5 s @ 48 kHz

    void init(float sample_rate, float* buf_l, float* buf_r);
    void set_on(bool on, bool immediate = false) { _sw.set_on(on, immediate); }
    bool is_on() const { return _sw.is_on(); }
    bool engaged() const {
        return _buf_ok && (_sw.is_on() || !_sw.is_idle());
    }
    bool has_buffers() const { return _buf_ok; }
    void set_time(float norm, bool immediate = false);   // 50 ms .. 5 s, exp
    void set_feedback(float norm);                       // 0 .. 1.1
    void set_mix(float norm);                            // -40 .. 0 dBFS
    void process(float& l, float& r);

private:
    EchoDelay<kMaxSamples> _echo_l;
    EchoDelay<kMaxSamples> _echo_r;
    SoftSwitch _sw;
    float _mix_lin = 0.f;
    bool _buf_ok = false;
};

} // namespace spky
```

Create `engine/fx/flux.cpp`:

```cpp
#include "fx/flux.h"
#include "util/math.h"

using namespace spky;

namespace {
inline float dbfs2lin(float db) { return daisysp::pow10f(db * 0.05f); }
}

void Flux::init(float sample_rate, float* buf_l, float* buf_r) {
    _sw.init(sample_rate);
    _buf_ok = (buf_l != nullptr && buf_r != nullptr);
    if (!_buf_ok) return;
    _echo_l.Init(sample_rate, buf_l);
    _echo_r.Init(sample_rate, buf_r);
    _echo_l.SetLagTime(0.5f);   // the original tape slew
    _echo_r.SetLagTime(0.5f);
    set_time(0.4f, true);       // boot defaults; PartFx drives them afterwards
    set_feedback(0.45f);
    set_mix(0.5f);
}

void Flux::set_time(float norm, bool immediate) {
    if (!_buf_ok) return;
    float t = daisysp::fmap(clampf(norm, 0.f, 1.f), 0.05f, 5.f,
                            daisysp::Mapping::LOG);
    _echo_l.SetDelayTime(t, immediate);
    _echo_r.SetDelayTime(t, immediate);
}

void Flux::set_feedback(float norm) {
    if (!_buf_ok) return;
    float fb = clampf(norm, 0.f, 1.f) * 1.1f;   // >1 allowed; SoftClip catches it
    _echo_l.SetFeedback(fb);
    _echo_r.SetFeedback(fb);
}

void Flux::set_mix(float norm) {
    _mix_lin = dbfs2lin(daisysp::fmap(clampf(norm, 0.f, 1.f), -40.f, 0.f));
}

void Flux::process(float& l, float& r) {
    if (!_buf_ok) return;
    float send = _sw.process();
    if (_sw.is_idle()) return;   // fully off: bit-exact dry
    l += _echo_l.Process(l * send) * _mix_lin;
    r += _echo_r.Process(r * send) * _mix_lin;
}
```

Note on `SoftClip`/`fmin`: both live in `Utility/dsp.h` (already included by `flux.h`).

- [ ] **Step 4: Build and run all tests**

Run: `source env.sh && cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add engine/fx/flux.h engine/fx/flux.cpp tests/test_flux.cpp CMakeLists.txt
git commit -m "feat(engine): port FLUX tape echo (EchoDelay/DeLine/BPF) as portable fx block"
```

### Task 5: `engine/fx/reverb.h/.cpp` — shared ambient reverb + shimmer

**Files:**
- Create: `engine/fx/reverb.h`, `engine/fx/reverb.cpp`
- Modify: `CMakeLists.txt` (add `engine/fx/reverb.cpp` to `spky_tests` AND `render`; add `tests/test_reverb.cpp` to `spky_tests`)
- Test: `tests/test_reverb.cpp`

**Interfaces:**
- Consumes: `daisysp::ReverbSc` (`Effects/reverbsc.h`, LGPL module), `daisysp::PitchShifter` (`Effects/pitchshifter.h`).
- Produces: `spky::AmbientReverb` with `init(float sr)`, `set_size(float)`, `set_tone(float)`, `set_shimmer(float)`, `process(float inL, float inR, float& outL, float& outR)` (input = summed sends, output = wet only). Task 8 owns one instance via `FxMem::reverb`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_reverb.cpp`:

```cpp
#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include "fx/reverb.h"
using namespace spky;

// ~530 KB object: static, never on the stack. init() re-seeds all state, so
// sharing one instance across cases is safe.
static AmbientReverb s_rev;

static std::vector<float> impulse_response(AmbientReverb& rv, int n,
                                           bool left_channel) {
    std::vector<float> out(n);
    for (int i = 0; i < n; ++i) {
        float wl = 0.f, wr = 0.f;
        float in = (i == 0) ? 1.f : 0.f;
        rv.process(in, in, wl, wr);
        out[i] = left_channel ? wl : wr;
    }
    return out;
}

TEST_CASE("reverb: silence in, exact silence out") {
    s_rev.init(48000.f);
    for (int i = 0; i < 2000; ++i) {
        float wl = 1.f, wr = 1.f;
        s_rev.process(0.f, 0.f, wl, wr);
        CHECK(wl == 0.f);
        CHECK(wr == 0.f);
    }
}

TEST_CASE("reverb: mono impulse produces a persistent stereo tail") {
    s_rev.init(48000.f);
    auto l = impulse_response(s_rev, 48000, true);
    s_rev.init(48000.f);
    auto r = impulse_response(s_rev, 48000, false);
    float tail = 0.f, decorr = 0.f;
    for (int i = 24000; i < 48000; ++i) tail += l[i] * l[i];
    for (int i = 0; i < 48000; ++i) decorr = std::max(decorr, std::fabs(l[i] - r[i]));
    CHECK(tail > 1e-6f);     // still ringing after 0.5 s at size 0.7
    CHECK(decorr > 1e-4f);   // L and R differ
}

TEST_CASE("reverb: shimmer 0 leaves the pitch shifter untouched") {
    s_rev.init(48000.f);
    auto plain = impulse_response(s_rev, 9600, true);
    s_rev.init(48000.f);
    s_rev.set_shimmer(0.7f);   // momentarily on...
    s_rev.set_shimmer(0.f);    // ...but 0 when processing starts
    auto toggled = impulse_response(s_rev, 9600, true);
    for (int i = 0; i < 9600; ++i) CHECK(plain[i] == toggled[i]);
}

TEST_CASE("reverb: shimmer changes the tail") {
    s_rev.init(48000.f);
    auto plain = impulse_response(s_rev, 48000, true);
    s_rev.init(48000.f);
    s_rev.set_shimmer(0.8f);
    auto shim = impulse_response(s_rev, 48000, true);
    int diff = 0;
    for (int i = 4800; i < 48000; ++i)
        if (std::fabs(plain[i] - shim[i]) > 1e-6f) ++diff;
    CHECK(diff > 1000);
}
```

- [ ] **Step 2: Run to verify it fails**

Add `engine/fx/reverb.cpp` to `spky_tests` and `render`, `tests/test_reverb.cpp` to `spky_tests`, then build.
Expected: FAIL — `fx/reverb.h: No such file or directory`.

- [ ] **Step 3: Write the implementation**

Create `engine/fx/reverb.h`:

```cpp
#pragma once
#include "Effects/reverbsc.h"
#include "Effects/pitchshifter.h"

namespace spky {

// The one shared room behind both parts. Input is the summed per-part sends
// (post-FX, pre-morph); output is wet-only and joins the master AFTER the
// part mix. Optional shimmer: a +12 st pitch shifter on the previous wet
// frame, fed back into the reverb input — fully skipped at shimmer == 0.
//
// BIG object (~530 KB — ReverbSc's aux buffer and the shifter's delay lines
// are inline members). Never stack-allocate: the desktop host owns it as a
// static; the M6 firmware shell places it in SDRAM. Injected via FxMem.
class AmbientReverb {
public:
    void init(float sample_rate);
    void set_size(float norm);      // decay: ReverbSc feedback 0.4 .. 0.99
    void set_tone(float norm);      // damping LP 500 Hz .. 16 kHz, exp
    void set_shimmer(float norm);   // 0 = shifter skipped entirely (CPU)
    float shimmer() const { return _shim; }
    void process(float in_l, float in_r, float& out_l, float& out_r);

private:
    daisysp::ReverbSc _rev;
    daisysp::PitchShifter _shift;
    float _shim = 0.f;
    float _last_l = 0.f;
    float _last_r = 0.f;
};

} // namespace spky
```

Create `engine/fx/reverb.cpp`:

```cpp
#include "fx/reverb.h"
#include "Utility/dsp.h"
#include "util/math.h"

using namespace spky;

void AmbientReverb::init(float sample_rate) {
    _rev.Init(sample_rate);
    _shift.Init(sample_rate);
    _shift.SetTransposition(12.f);   // fixed +1 octave (spec)
    _shift.SetFun(0.f);
    _shim = 0.f;
    _last_l = _last_r = 0.f;
    set_size(0.7f);                  // boot defaults (spec)
    set_tone(0.5f);
}

void AmbientReverb::set_size(float norm) {
    _rev.SetFeedback(daisysp::fmap(clampf(norm, 0.f, 1.f), 0.4f, 0.99f));
}

void AmbientReverb::set_tone(float norm) {
    _rev.SetLpFreq(daisysp::fmap(clampf(norm, 0.f, 1.f), 500.f, 16000.f,
                                 daisysp::Mapping::LOG));
}

void AmbientReverb::set_shimmer(float norm) {
    _shim = clampf(norm, 0.f, 1.f);
}

void AmbientReverb::process(float in_l, float in_r, float& out_l, float& out_r) {
    if (_shim > 0.f) {
        float mono = 0.5f * (_last_l + _last_r);
        float shifted = _shift.Process(mono);
        float g = _shim * 0.5f;      // fixed headroom on the feedback path
        in_l += shifted * g;
        in_r += shifted * g;
    }
    _rev.Process(in_l, in_r, &out_l, &out_r);
    _last_l = out_l;
    _last_r = out_r;
}
```

(`daisysp::PitchShifter::Process` takes `float&` — `mono` is an lvalue for that reason. The shifter's internal RNG is a global static — see locked decision 9; only within-test comparisons, never golden values.)

- [ ] **Step 4: Build and run all tests**

Run: `source env.sh && cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add engine/fx/reverb.h engine/fx/reverb.cpp tests/test_reverb.cpp CMakeLists.txt
git commit -m "feat(engine): shared ambient reverb (ReverbSc) with optional +12st shimmer"
```

### Task 6: `engine/fx/part_fx.h/.cpp` — per-part chain + FX target ids

**Files:**
- Create: `engine/fx/part_fx.h`, `engine/fx/part_fx.cpp`
- Modify: `CMakeLists.txt` (add `engine/fx/part_fx.cpp` to `spky_tests` AND `render`; add `tests/test_part_fx.cpp` to `spky_tests`)
- Test: `tests/test_part_fx.cpp`

**Interfaces:**
- Consumes: `spky::Grit`, `spky::Flux`, `spky::OnePole` (`util/onepole.h`).
- Produces (Tasks 7–9 depend on these exact names):
  - `enum spky::FxTargetId { FXT_GRIT_INT = 0, FXT_FLUX_TIME = 1, FXT_FX_MIX = 2, FXT_REV_SEND = 3, FXT_FLUX_FB = 4, FXT_COUNT = 5 }`
  - `enum class spky::FxBlock { Flux, Grit }`
  - `spky::PartFx` with `init(float sr, float* echo_l, float* echo_r)`, `grit()`, `flux()`, `set_fx_on(FxBlock, bool, bool immediate = false)`, `set_grit_mode(GritMode)`, `set_flux_mix(float)`, `set_grit_mix(float)`, `process(float& l, float& r, float& send_l, float& send_r, const float* fxv)` where `fxv` is the 5 already-modulated target values from `Part`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_part_fx.cpp`:

```cpp
#include <doctest/doctest.h>
#include <cmath>
#include "fx/part_fx.h"
using namespace spky;

static float s_pf_l[Flux::kMaxSamples];
static float s_pf_r[Flux::kMaxSamples];

// fxv helper: boot bases with individual overrides
static void fill(float* v, float grit, float time, float mix, float send, float fb) {
    v[FXT_GRIT_INT] = grit;
    v[FXT_FLUX_TIME] = time;
    v[FXT_FX_MIX] = mix;
    v[FXT_REV_SEND] = send;
    v[FXT_FLUX_FB] = fb;
}

TEST_CASE("part_fx: both blocks off is bit-exact dry, send 0 is exact zero") {
    PartFx fx;
    fx.init(48000.f, s_pf_l, s_pf_r);
    float v[FXT_COUNT];
    fill(v, 0.3f, 0.4f, 1.f, 0.f, 0.45f);
    for (int i = 0; i < 2000; ++i) {
        float s = 0.4f * std::sin(0.013f * i);
        float l = s, r = s, sl = 1.f, sr = 1.f;
        fx.process(l, r, sl, sr, v);
        CHECK(l == s);
        CHECK(r == s);
        CHECK(sl == 0.f);
        CHECK(sr == 0.f);
    }
}

TEST_CASE("part_fx: FX MIX 0 keeps the dry signal even with grit engaged") {
    PartFx fx;
    fx.init(48000.f, s_pf_l, s_pf_r);
    fx.set_fx_on(FxBlock::Grit, true, true);
    float v[FXT_COUNT];
    fill(v, 0.9f, 0.4f, 0.f, 0.f, 0.f);
    for (int i = 0; i < 2000; ++i) {
        float s = 0.4f * std::sin(0.013f * i);
        float l = s, r = s, sl, sr;
        fx.process(l, r, sl, sr, v);
        CHECK(l == doctest::Approx(s).epsilon(1e-6));
    }
}

TEST_CASE("part_fx: FX MIX 1 with grit on changes the signal") {
    PartFx fx;
    fx.init(48000.f, s_pf_l, s_pf_r);
    fx.set_fx_on(FxBlock::Grit, true, true);
    float v[FXT_COUNT];
    fill(v, 0.9f, 0.4f, 1.f, 0.f, 0.f);
    int diff = 0;
    for (int i = 0; i < 4800; ++i) {
        float s = 0.4f * std::sin(0.028f * i);
        float l = s, r = s, sl, sr;
        fx.process(l, r, sl, sr, v);
        if (std::fabs(l - s) > 1e-4f) ++diff;
    }
    CHECK(diff > 1000);
}

TEST_CASE("part_fx: send taps post-FX at the equal-power gain") {
    PartFx fx;
    fx.init(48000.f, s_pf_l, s_pf_r);
    float v[FXT_COUNT];
    fill(v, 0.3f, 0.4f, 1.f, 1.f, 0.45f);   // send fully open
    // prime the smoothers (first process snaps), then measure
    float l = 0.f, r = 0.f, sl, sr;
    fx.process(l, r, sl, sr, v);
    for (int i = 1; i < 200; ++i) {
        float s = 0.4f * std::sin(0.013f * i);
        l = s; r = s;
        fx.process(l, r, sl, sr, v);
        CHECK(sl == doctest::Approx(l));    // sin(pi/2) = 1: send == post-fx out
    }
}
```

- [ ] **Step 2: Run to verify it fails**

Add `engine/fx/part_fx.cpp` to `spky_tests` and `render`, `tests/test_part_fx.cpp` to `spky_tests`, then build.
Expected: FAIL — `fx/part_fx.h: No such file or directory`.

- [ ] **Step 3: Write the implementation**

Create `engine/fx/part_fx.h`:

```cpp
#pragma once
#include "fx/flux.h"
#include "fx/grit.h"
#include "util/onepole.h"

namespace spky {

// The second target row: pad slot in the FX layer == lane index, mirroring the
// engine targets' "lane index == pad slot == target slot" principle (spec).
enum FxTargetId {
    FXT_GRIT_INT  = 0,   // lane 0 (x2)    fast rhythmic texture
    FXT_FLUX_TIME = 1,   // lane 1 (x1/2)  slow tape drift / dub steps
    FXT_FX_MIX    = 2,   // lane 2 (x1)    accents locked to the melody cycle
    FXT_REV_SEND  = 3,   // lane 3 (x3/4)  polyrhythmic breathing
    FXT_FLUX_FB   = 4,   // lane 4 (x3/2)  swells
    FXT_COUNT     = 5
};

enum class FxBlock { Flux, Grit };

// Per-part chain: GRIT -> FLUX -> FX MIX, plus the post-FX reverb send tap.
// FX MIX is a linear (equal-gain) dry/wet — dry and wet are correlated, and
// bypass must be bit-exact (wet == dry => out == dry). The square-law XFade
// stays inside Drive/Reduce where it belongs. When both blocks are off the
// whole chain is skipped, so "FX off" costs nothing and changes nothing.
class PartFx {
public:
    void init(float sample_rate, float* echo_l, float* echo_r);

    Grit& grit() { return _grit; }
    Flux& flux() { return _flux; }
    const Grit& grit() const { return _grit; }
    const Flux& flux() const { return _flux; }

    void set_fx_on(FxBlock b, bool on, bool immediate = false);
    void set_grit_mode(GritMode m) { _grit.set_mode(m); }
    void set_flux_mix(float n) { _flux.set_mix(n); }
    void set_grit_mix(float n) { _grit.set_mix(n); }

    // fxv[FXT_COUNT]: already-modulated values from Part::fx_target_value().
    void process(float& l, float& r, float& send_l, float& send_r,
                 const float* fxv);

private:
    Grit _grit;
    Flux _flux;
    OnePole _smooth[FXT_COUNT];
    float _grit_applied = -1.f;   // change guard: Overdrive::SetDrive costs
    bool _primed = false;         // first process() snaps the smoothers
};

} // namespace spky
```

Create `engine/fx/part_fx.cpp`:

```cpp
#include "fx/part_fx.h"
#include <cmath>

using namespace spky;

void PartFx::init(float sample_rate, float* echo_l, float* echo_r) {
    _grit.init(sample_rate);
    _flux.init(sample_rate, echo_l, echo_r);
    for (auto& s : _smooth) s.init(sample_rate, 0.002f);
    _grit_applied = -1.f;
    _primed = false;
}

void PartFx::set_fx_on(FxBlock b, bool on, bool immediate) {
    if (b == FxBlock::Flux) _flux.set_on(on, immediate);
    else                    _grit.set_on(on, immediate);
}

void PartFx::process(float& l, float& r, float& send_l, float& send_r,
                     const float* fxv) {
    if (!_primed) {   // snap to the first real values: no phantom boot slew
        for (int i = 0; i < FXT_COUNT; ++i) _smooth[i].reset(fxv[i]);
        _primed = true;
    }
    float v[FXT_COUNT];
    for (int i = 0; i < FXT_COUNT; ++i) v[i] = _smooth[i].process(fxv[i]);

    if (_grit.engaged() || _flux.engaged()) {
        if (v[FXT_GRIT_INT] != _grit_applied) {
            _grit.set_intensity(v[FXT_GRIT_INT]);
            _grit_applied = v[FXT_GRIT_INT];
        }
        _flux.set_time(v[FXT_FLUX_TIME]);      // slewed inside EchoDelay (tape)
        _flux.set_feedback(v[FXT_FLUX_FB]);
        const float dry_l = l, dry_r = r;
        _grit.process(l, r);
        _flux.process(l, r);
        const float m = v[FXT_FX_MIX];
        l = dry_l + (l - dry_l) * m;
        r = dry_r + (r - dry_r) * m;
    }

    const float g = std::sin(v[FXT_REV_SEND] * 1.5707963f);   // equal-power
    send_l = l * g;
    send_r = r * g;
}
```

- [ ] **Step 4: Build and run all tests**

Run: `source env.sh && cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add engine/fx/part_fx.h engine/fx/part_fx.cpp tests/test_part_fx.cpp CMakeLists.txt
git commit -m "feat(engine): per-part FX chain GRIT->FLUX->FX MIX with reverb send tap"
```

### Task 7: `Part` — second target row + FX in the audio path

**Files:**
- Modify: `engine/parts/part.h`, `engine/parts/part.cpp`
- Test: `tests/test_part.cpp` (append cases)

**Interfaces:**
- Consumes: `spky::PartFx`, `FXT_*` ids from Task 6.
- Produces (Task 8 depends on these):
  - `void Part::init(float sample_rate, uint32_t seed_base, float* echo_l = nullptr, float* echo_r = nullptr)` (existing 2-arg calls keep compiling)
  - `PartFx& Part::fx()`
  - `void set_fx_target_active(int slot, bool on)` / `set_fx_target_base(int slot, float)` / `set_fx_target_depth(int slot, float)`
  - `float fx_target_value(int slot) const` — `clamp01(base + lane_output × master_depth × target_depth)`, NO quantization
  - `void process(float& outL, float& outR, float& sendL, float& sendR)` — the old 2-arg `process(l, r)` stays as a wrapper that discards sends.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_part.cpp`:

```cpp
TEST_CASE("part: inactive fx target contributes only its base value") {
    Part p;
    p.init(48000.f, 5);
    p.set_fx_target_base(FXT_FLUX_TIME, 0.37f);
    p.mod().set_range(1.f);
    float l, r;
    for (int i = 0; i < 1000; ++i) p.process(l, r);
    CHECK(p.fx_target_value(FXT_FLUX_TIME) == doctest::Approx(0.37f));
}

TEST_CASE("part: active fx target modulates around its base, clamped") {
    Part p;
    p.init(48000.f, 5);
    p.set_fx_target_active(FXT_FLUX_TIME, true);
    p.set_fx_target_base(FXT_FLUX_TIME, 0.5f);
    p.set_fx_target_depth(FXT_FLUX_TIME, 1.f);
    p.set_depth(1.f);
    p.mod().set_range(1.f);
    p.mod().set_sync_mode(SyncMode::Free);
    p.mod().set_rate(0.6f);
    float minv = 1.f, maxv = 0.f, l, r;
    for (int i = 0; i < 48000; ++i) {
        p.process(l, r);
        float t = p.fx_target_value(FXT_FLUX_TIME);
        if (t < minv) minv = t;
        if (t > maxv) maxv = t;
    }
    CHECK(maxv > minv);
    CHECK(minv >= 0.f);
    CHECK(maxv <= 1.f);
}

TEST_CASE("part: master DEPTH 0 pins fx targets to base") {
    Part p;
    p.init(48000.f, 5);
    p.set_fx_target_active(FXT_REV_SEND, true);
    p.set_fx_target_base(FXT_REV_SEND, 0.4f);
    p.set_depth(0.f);
    p.mod().set_range(1.f);
    float l, r;
    for (int i = 0; i < 5000; ++i) {
        p.process(l, r);
        CHECK(p.fx_target_value(FXT_REV_SEND) == doctest::Approx(0.4f));
    }
}

TEST_CASE("part: fx targets are never quantized (unlike the PITCH lane)") {
    Part p;
    p.init(48000.f, 5);   // boots in SCALE mode
    p.set_fx_target_base(FXT_FX_MIX, 0.437f);   // off any scale grid
    float l, r;
    p.process(l, r);
    CHECK(p.fx_target_value(FXT_FX_MIX) == doctest::Approx(0.437f));
}

TEST_CASE("part: 4-output process yields sends that follow REV SEND") {
    Part p;
    p.init(48000.f, 5);
    p.set_fx_target_base(FXT_REV_SEND, 1.f);    // send fully open
    float l, r, sl, sr;
    for (int i = 0; i < 2000; ++i) p.process(l, r, sl, sr);
    CHECK(sl == doctest::Approx(l));            // sin(pi/2) = 1
    p.set_fx_target_base(FXT_REV_SEND, 0.f);
    for (int i = 0; i < 2000; ++i) p.process(l, r, sl, sr);   // ride out smoother
    CHECK(sl == doctest::Approx(0.f));
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `source env.sh && cmake --build build 2>&1 | tail -5`
Expected: FAIL — `set_fx_target_base` / `FXT_FLUX_TIME` undeclared.

- [ ] **Step 3: Implement**

`engine/parts/part.h` — add `#include "fx/part_fx.h"` to the include block, then replace the `init`/`process` declarations and add the FX members. The class becomes:

```cpp
class Part {
public:
    void init(float sample_rate, uint32_t seed_base,
              float* echo_l = nullptr, float* echo_r = nullptr);

    SuperModulator& mod() { return _mod; }
    Quantizer& quant() { return _quant; }
    PartFx& fx() { return _fx; }

    void set_depth(float d) { _depth = clampf(d, 0.f, 1.f); }
    void set_tune(float t)  { _tune = clampf(t, 0.f, 1.f); }
    void set_target_active(int slot, bool on) { _active[slot] = on; }
    void set_target_base(int slot, float b)   { _base[slot] = clampf(b, 0.f, 1.f); }
    void set_target_depth(int slot, float d)  { _tdepth[slot] = clampf(d, 0.f, 1.f); }

    void set_fx_target_active(int slot, bool on) { _fx_active[slot] = on; }
    void set_fx_target_base(int slot, float b)   { _fx_base[slot] = clampf(b, 0.f, 1.f); }
    void set_fx_target_depth(int slot, float d)  { _fx_depth[slot] = clampf(d, 0.f, 1.f); }
    float fx_target_value(int slot) const;

    float target_value(int slot) const;
    float target_raw(int slot) const;
    float pitch_pre_quant() const;
    float lane_output(int slot) const { return _mod.lane_output(slot); }
    bool  lane_fired(int slot) const  { return _mod.lane_fired(slot); }
    bool  gate() const { return _gate_ctr > 0; }
    float pitch_cv() const { return target_value(LANE_PITCH); }

    // advance mod one sample + engine + part FX; sends = post-FX x REVERB SEND
    void process(float& outL, float& outR, float& sendL, float& sendR);
    void process(float& outL, float& outR) {
        float sl, sr;
        process(outL, outR, sl, sr);
    }

private:
    SuperModulator _mod;
    TestToneEngine _tone;
    IPartEngine*   _engine = nullptr;
    PartFx         _fx;

    std::array<bool,  LANE_COUNT> _active { { false, false, true, false, true } };
    std::array<float, LANE_COUNT> _base   { { 0.5f, 0.5f, 0.5f, 0.5f, 0.8f } };
    std::array<float, LANE_COUNT> _tdepth { { 1.f, 1.f, 1.f, 1.f, 1.f } };

    // FX target row (boot: all modulation inactive, spec "Boot defaults").
    // Bases, by FxTargetId: GRIT_INT .3 | FLUX_TIME .4 | FX_MIX 1 | REV_SEND .25 | FLUX_FB .45
    std::array<bool,  FXT_COUNT> _fx_active { { false, false, false, false, false } };
    std::array<float, FXT_COUNT> _fx_base   { { 0.3f, 0.4f, 1.f, 0.25f, 0.45f } };
    std::array<float, FXT_COUNT> _fx_depth  { { 1.f, 1.f, 1.f, 1.f, 1.f } };

    float _depth = 1.f;
    float _tune = 0.5f;
    int   _gate_ctr = 0;
    int   _gate_len = 240;
    float _sr = 48000.f;

    Quantizer _quant;
    float     _pitch_q = 0.f;
};
```

`engine/parts/part.cpp` — extend `init`, add `fx_target_value`, rework `process`:

```cpp
void Part::init(float sample_rate, uint32_t seed_base,
                float* echo_l, float* echo_r) {
    _sr = sample_rate;
    _mod.init(sample_rate, seed_base);
    _tone.init(sample_rate);
    _engine = &_tone;
    _fx.init(sample_rate, echo_l, echo_r);
    _gate_len = static_cast<int>(sample_rate * 0.005f);
    _gate_ctr = 0;
    _quant.init(sample_rate);
    _pitch_q = _quant.process(pitch_pre_quant());
}
```

```cpp
// Same combine rule as target_raw, tapped from the SAME lanes — the FX breathe
// in the part's own character. Never quantized (that is a PITCH-lane concern).
float Part::fx_target_value(int slot) const {
    float mod = _fx_active[slot]
        ? _mod.lane_output(slot) * _depth * _fx_depth[slot] : 0.f;
    return clampf(_fx_base[slot] + mod, 0.f, 1.f);
}
```

```cpp
void Part::process(float& outL, float& outR, float& sendL, float& sendR) {
    _mod.process();

    if (_mod.lane_fired(LANE_PITCH)) _gate_ctr = _gate_len;
    if (_gate_ctr > 0) --_gate_ctr;

    float targets[LANE_COUNT];
    for (int i = 0; i < LANE_COUNT; ++i) targets[i] = target_raw(i);
    targets[LANE_PITCH] = _quant.process(pitch_pre_quant());
    _pitch_q = targets[LANE_PITCH];

    _engine->set_targets(targets, _tune);
    if (_mod.lane_fired(LANE_PITCH)) _engine->trigger(targets[LANE_PITCH]);
    _engine->process(outL, outR);

    float fxv[FXT_COUNT];
    for (int i = 0; i < FXT_COUNT; ++i) fxv[i] = fx_target_value(i);
    _fx.process(outL, outR, sendL, sendR, fxv);
}
```

(The old 2-arg `process` body is deleted from part.cpp — it lives inline in the header now.)

- [ ] **Step 4: Build and run all tests**

Run: `source env.sh && cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS — including every pre-existing part/instrument/scenario test (the FX chain defaults to bypassed).

- [ ] **Step 5: Commit**

```bash
git add engine/parts/part.h engine/parts/part.cpp tests/test_part.cpp
git commit -m "feat(engine): second fx target row on Part; fx chain in the part audio path"
```

### Task 8: `Instrument` — FxMem, FX API, shared reverb in the mix

**Files:**
- Modify: `engine/instrument.h`, `engine/instrument.cpp`
- Test: `tests/test_instrument.cpp` (append cases)

**Interfaces:**
- Consumes: `Part` 4-output `process`, `PartFx` setters, `AmbientReverb`.
- Produces (Task 9 depends on these exact signatures — they are the spec's "Instrument API" verbatim):

```cpp
struct FxMem {
    float* echo[PART_COUNT][2] = { { nullptr, nullptr }, { nullptr, nullptr } };
    AmbientReverb* reverb = nullptr;
};
void init(float sample_rate);                    // unchanged: no FX chain
void init(float sample_rate, const FxMem& mem);  // full chain
void set_fx_on(int p, FxBlock which, bool on);
void set_grit_mode(int p, GritMode m);
void set_fx_target_active(int p, int i, bool on);
void set_fx_target_base(int p, int i, float n);
void set_fx_target_depth(int p, int i, float n);
void set_flux_mix(int p, float n);
void set_grit_mix(int p, float n);
void set_reverb_size(float n);                   // no-op without reverb
void set_reverb_tone(float n);
void set_reverb_shimmer(float n);
float fx_target_value(int p, int i) const;
```

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_instrument.cpp`:

```cpp
#include "fx/reverb.h"

static float s_ti_echo[PART_COUNT][2][spky::Flux::kMaxSamples];
static spky::AmbientReverb s_ti_reverb;

static spky::FxMem test_fx_mem() {
    spky::FxMem m;
    for (int p = 0; p < PART_COUNT; ++p)
        for (int c = 0; c < 2; ++c) m.echo[p][c] = s_ti_echo[p][c];
    m.reverb = &s_ti_reverb;
    return m;
}

TEST_CASE("instrument: all FX off + send 0 is bit-identical to the no-FX build") {
    Instrument plain;
    plain.init(48000.f);
    Instrument fx;
    fx.init(48000.f, test_fx_mem());
    for (int p = 0; p < PART_COUNT; ++p)
        fx.set_fx_target_base(p, FXT_REV_SEND, 0.f);   // before any process()
    float pl, pr, fl, fr;
    for (int i = 0; i < 48000; ++i) {
        plain.process(nullptr, nullptr, &pl, &pr, 1);
        fx.process(nullptr, nullptr, &fl, &fr, 1);
        CHECK(fl == pl);
        CHECK(fr == pr);
    }
}

TEST_CASE("instrument: boot reverb send is audible") {
    Instrument plain;
    plain.init(48000.f);
    Instrument fx;
    fx.init(48000.f, test_fx_mem());   // boot REV_SEND base = 0.25
    float pl, pr, fl, fr;
    int diff = 0;
    for (int i = 0; i < 48000; ++i) {
        plain.process(nullptr, nullptr, &pl, &pr, 1);
        fx.process(nullptr, nullptr, &fl, &fr, 1);
        if (std::fabs(fl - pl) > 1e-5f) ++diff;
    }
    CHECK(diff > 1000);
}

TEST_CASE("instrument: fx setters reach the parts and reverb setters are null-safe") {
    Instrument inst;
    inst.init(48000.f);                 // NO reverb, NO buffers
    inst.set_fx_on(PART_A, FxBlock::Grit, true);
    inst.set_grit_mode(PART_A, GritMode::Reduce);
    inst.set_fx_target_active(PART_A, FXT_GRIT_INT, true);
    inst.set_fx_target_base(PART_A, FXT_GRIT_INT, 0.6f);
    inst.set_fx_target_depth(PART_A, FXT_GRIT_INT, 0.5f);
    inst.set_flux_mix(PART_A, 0.4f);
    inst.set_grit_mix(PART_A, 0.7f);
    inst.set_reverb_size(0.9f);         // must not crash without a reverb
    inst.set_reverb_tone(0.2f);
    inst.set_reverb_shimmer(0.5f);
    float l, r;
    inst.process(nullptr, nullptr, &l, &r, 1);
    CHECK(inst.fx_target_value(PART_A, FXT_GRIT_INT) >= 0.f);
    CHECK(l == l);   // not NaN
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `source env.sh && cmake --build build 2>&1 | tail -5`
Expected: FAIL — `FxMem` / `set_fx_on` undeclared.

- [ ] **Step 3: Implement**

`engine/instrument.h` — add `#include "fx/reverb.h"`, then after the `PartId` enum:

```cpp
// FX memory injected by the host (spec "No heap"): echo buffers of
// Flux::kMaxSamples floats each, and storage for the one shared reverb.
// Desktop: static arrays / static object. Daisy (M6): SDRAM.
struct FxMem {
    float* echo[PART_COUNT][2] = { { nullptr, nullptr }, { nullptr, nullptr } };
    AmbientReverb* reverb = nullptr;
};
```

Inside the class, replace `void init(float sample_rate);` with both overloads and add the FX API (inline, following the existing setter style):

```cpp
    void init(float sample_rate);                    // engine only, no FX chain
    void init(float sample_rate, const FxMem& mem);  // full FX chain
```

```cpp
    void set_fx_on(int p, FxBlock which, bool on)  { _parts[p].fx().set_fx_on(which, on); }
    void set_grit_mode(int p, GritMode m)          { _parts[p].fx().set_grit_mode(m); }
    void set_fx_target_active(int p, int i, bool on) { _parts[p].set_fx_target_active(i, on); }
    void set_fx_target_base(int p, int i, float n) { _parts[p].set_fx_target_base(i, n); }
    void set_fx_target_depth(int p, int i, float n){ _parts[p].set_fx_target_depth(i, n); }
    void set_flux_mix(int p, float n)              { _parts[p].fx().set_flux_mix(n); }
    void set_grit_mix(int p, float n)              { _parts[p].fx().set_grit_mix(n); }
    void set_reverb_size(float n)    { if (_reverb) _reverb->set_size(n); }
    void set_reverb_tone(float n)    { if (_reverb) _reverb->set_tone(n); }
    void set_reverb_shimmer(float n) { if (_reverb) _reverb->set_shimmer(n); }
    float fx_target_value(int p, int i) const { return _parts[p].fx_target_value(i); }
```

Add the member: `AmbientReverb* _reverb = nullptr;`

`engine/instrument.cpp` — replace `init` and `process`:

```cpp
void Instrument::init(float sample_rate) { init(sample_rate, FxMem{}); }

void Instrument::init(float sample_rate, const FxMem& mem) {
    _sr = sample_rate;
    _reverb = mem.reverb;
    _parts[PART_A].init(sample_rate, 0x1234abcdu,
                        mem.echo[PART_A][0], mem.echo[PART_A][1]);
    _parts[PART_B].init(sample_rate, 0x9e3779b9u,
                        mem.echo[PART_B][0], mem.echo[PART_B][1]);
    if (_reverb) _reverb->init(sample_rate);
    set_tempo_bpm(_bpm);
}

void Instrument::process(const float* /*inL*/, const float* /*inR*/,
                         float* outL, float* outR, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        float al, ar, bl, br;
        float asl, asr, bsl, bsr;
        _parts[PART_A].process(al, ar, asl, asr);
        _parts[PART_B].process(bl, br, bsl, bsr);
        float l = (al + bl) * 0.5f;   // MORPH/center mixing arrives in M4
        float r = (ar + br) * 0.5f;
        if (_reverb) {
            // sends tapped pre-morph; the shared room joins AFTER the part
            // mix — a part morphed away can still haunt the room (spec).
            float wl, wr;
            _reverb->process(asl + bsl, asr + bsr, wl, wr);
            l += wl;
            r += wr;
        }
        outL[i] = l;
        outR[i] = r;
    }
}
```

- [ ] **Step 4: Build and run all tests**

Run: `source env.sh && cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS. The bypass test is the spec's bit-exactness acceptance criterion — if it fails, check that `PartFx::process` skips the chain when both blocks are idle and that the smoother priming (locked decision 7) landed.

- [ ] **Step 5: Commit**

```bash
git add engine/instrument.h engine/instrument.cpp tests/test_instrument.cpp
git commit -m "feat(engine): Instrument FX API, injected FxMem, shared reverb in the mix"
```

### Task 9: Render host — scenario actions, FxMem wiring, CSV columns

**Files:**
- Modify: `host/render/scenario.cpp`, `host/render/main.cpp`
- Test: `tests/test_scenario.cpp` (append cases)

**Interfaces:**
- Consumes: the Task 8 Instrument API.
- Produces: scenario actions `set_fx_on` (svalue `"flux"`/`"grit"` + flag), `set_grit_mode` (svalue `"drive"`/`"reduce"`), `set_fx_target_active`/`set_fx_target_base`/`set_fx_target_depth` (part+slot), `set_flux_mix`, `set_grit_mix`, `set_reverb_size`/`set_reverb_tone`/`set_reverb_shimmer` (global, no part). `mods.csv` gains `a_fx0..a_fx4` and `b_fx0..b_fx4` columns (actual FX target values).

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_scenario.cpp`:

```cpp
TEST_CASE("scenario: fx actions reach the instrument") {
    Instrument inst;
    inst.init(48000.f);

    Event base;
    base.action = "set_fx_target_base";
    base.part = 0;
    base.slot = FXT_FLUX_TIME;
    base.value = 0.8f;
    apply_event(inst, base);
    CHECK(inst.fx_target_value(0, FXT_FLUX_TIME) == doctest::Approx(0.8f));

    Event on;      // must not crash even without FX memory
    on.action = "set_fx_on";
    on.part = 0;
    on.svalue = "flux";
    on.flag = true;
    apply_event(inst, on);

    Event mode;
    mode.action = "set_grit_mode";
    mode.part = 1;
    mode.svalue = "reduce";
    apply_event(inst, mode);

    Event shim;    // global reverb action: no part, null-safe
    shim.action = "set_reverb_shimmer";
    shim.value = 0.5f;
    apply_event(inst, shim);

    float l = 0.f, r = 0.f;
    inst.process(nullptr, nullptr, &l, &r, 1);
    CHECK(l == l);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `source env.sh && cmake --build build && ctest --test-dir build --output-on-failure 2>&1 | tail -8`
Expected: FAIL — `fx_target_value` stays at the boot base (0.4) because `apply_event` ignores unknown actions (forward-compatible by design), so the `Approx(0.8)` check fails.

- [ ] **Step 3: Implement**

`host/render/scenario.cpp` — add two parse helpers next to `parse_qmode`:

```cpp
static FxBlock parse_fx_block(const std::string& s) {
    return s == "grit" ? FxBlock::Grit : FxBlock::Flux;
}

static GritMode parse_grit_mode(const std::string& s) {
    return s == "reduce" ? GritMode::Reduce : GritMode::Drive;
}
```

In `apply_event`, before the closing "unknown actions" comment, add:

```cpp
    else if (a == "set_fx_on")            inst.set_fx_on(e.part, parse_fx_block(e.svalue), e.flag);
    else if (a == "set_grit_mode")        inst.set_grit_mode(e.part, parse_grit_mode(e.svalue));
    else if (a == "set_fx_target_active") inst.set_fx_target_active(e.part, e.slot, e.flag);
    else if (a == "set_fx_target_base")   inst.set_fx_target_base(e.part, e.slot, e.value);
    else if (a == "set_fx_target_depth")  inst.set_fx_target_depth(e.part, e.slot, e.value);
    else if (a == "set_flux_mix")         inst.set_flux_mix(e.part, e.value);
    else if (a == "set_grit_mix")         inst.set_grit_mix(e.part, e.value);
    else if (a == "set_reverb_size")      inst.set_reverb_size(e.value);
    else if (a == "set_reverb_tone")      inst.set_reverb_tone(e.value);
    else if (a == "set_reverb_shimmer")   inst.set_reverb_shimmer(e.value);
```

`host/render/main.cpp` — three changes:

1. After the includes, the static FX memory (file scope — 4 × 240000 floats ≈ 3.7 MB plus the ~530 KB reverb, deliberately NOT on the stack):

```cpp
// FX memory, injected per the engine's no-heap contract (FxMem pattern).
static float s_echo[spky::PART_COUNT][2][spky::Flux::kMaxSamples];
static spky::AmbientReverb s_reverb;
```

2. Replace `inst.init(static_cast<float>(scen.sample_rate));` with:

```cpp
    FxMem fx_mem;
    for (int p = 0; p < PART_COUNT; ++p)
        for (int c = 0; c < 2; ++c) fx_mem.echo[p][c] = s_echo[p][c];
    fx_mem.reverb = &s_reverb;
    inst.init(static_cast<float>(scen.sample_rate), fx_mem);
```

3. Extend the CSV header and row. Header becomes:

```cpp
    if (csv) {
        std::fprintf(csv, "t,"
            "a_src,a_size,a_pitch,a_motion,a_level,a_pcv,a_gate,"
            "a_fx0,a_fx1,a_fx2,a_fx3,a_fx4,"
            "b_src,b_size,b_pitch,b_motion,b_level,b_pcv,b_gate,"
            "b_fx0,b_fx1,b_fx2,b_fx3,b_fx4\n");
    }
```

Row loop (inside the existing `for (int p = 0; ...)`), after the pcv/gate print, add:

```cpp
                for (int s = 0; s < FXT_COUNT; ++s)
                    std::fprintf(csv, ",%.4f", inst.fx_target_value(p, s));
```

- [ ] **Step 4: Build, run all tests, spot-check a render**

Run: `source env.sh && cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS.

Run: `./build/render host/render/scenarios/dorian_melody.json /tmp/t.wav /tmp/t.csv && head -2 /tmp/t.csv`
Expected: the header shows 25 columns (`t` + 12 per part); `a_fx0..a_fx4` hold the boot bases `0.3, 0.4, 1.0, 0.25, 0.45`.

- [ ] **Step 5: Commit**

```bash
git add host/render/scenario.cpp host/render/main.cpp tests/test_scenario.cpp
git commit -m "feat(host): fx scenario actions, injected fx memory, fx columns in mods.csv"
```

### Task 10: Demo scenarios, acceptance renders, docs

**Files:**
- Create: `host/render/scenarios/dub_delay.json`, `host/render/scenarios/ambient_wash.json`
- Modify: `docs/roadmap.md`, `README.md`, `THIRD_PARTY.md`

**Interfaces:**
- Consumes: everything above. This task is the spec's acceptance-criteria gate.

- [ ] **Step 1: Create `host/render/scenarios/dub_delay.json`**

```json
{
  "sample_rate": 48000,
  "bpm": 75,
  "duration_s": 40,
  "init": [
    {"_comment":"PART A - dorian STEP melody into a dub tape echo. Lane 1 (x1/2, STEP) kicks FLUX TIME around -> audible pitch jumps; lane 4 (x3/2) swells FLUX FEEDBACK."},
    {"action":"set_sync_mode","part":0,"value":"sync"},
    {"action":"set_rate","part":0,"value":0.35},
    {"action":"set_step","part":0,"flag":true,"ivalue":8},
    {"action":"set_range","part":0,"value":0.5},
    {"action":"set_smooth","part":0,"value":0.2},
    {"action":"set_probability","part":0,"value":0.9},
    {"action":"set_depth","part":0,"value":1.0},
    {"action":"set_target_active","part":0,"slot":2,"flag":true},
    {"action":"set_target_base","part":0,"slot":2,"value":0.5},
    {"action":"set_target_active","part":0,"slot":4,"flag":true},
    {"action":"set_target_base","part":0,"slot":4,"value":0.7},

    {"_comment":"FLUX on; TIME modulated by the STEP lane, FEEDBACK breathing on lane 4."},
    {"action":"set_fx_on","part":0,"value":"flux","flag":true},
    {"action":"set_flux_mix","part":0,"value":0.6},
    {"action":"set_fx_target_active","part":0,"slot":1,"flag":true},
    {"action":"set_fx_target_base","part":0,"slot":1,"value":0.35},
    {"action":"set_fx_target_depth","part":0,"slot":1,"value":0.6},
    {"action":"set_fx_target_active","part":0,"slot":4,"flag":true},
    {"action":"set_fx_target_base","part":0,"slot":4,"value":0.5},
    {"action":"set_fx_target_depth","part":0,"slot":4,"value":0.5},
    {"action":"set_fx_target_base","part":0,"slot":3,"value":0.15},

    {"_comment":"PART B silent for clarity."},
    {"action":"set_target_active","part":1,"slot":4,"flag":false},
    {"action":"set_target_base","part":1,"slot":4,"value":0.0}
  ],
  "events": [
    {"t":20.0,"action":"set_fx_target_base","part":0,"slot":4,"value":0.75},
    {"_comment":"20s: feedback base up -> long self-feeding dub swells."},
    {"t":30.0,"action":"set_fx_on","part":0,"value":"grit","flag":true},
    {"_comment":"30s: GRIT (Drive) joins for texture."}
  ]
}
```

- [ ] **Step 2: Create `host/render/scenarios/ambient_wash.json`**

```json
{
  "sample_rate": 48000,
  "bpm": 60,
  "duration_s": 48,
  "init": [
    {"_comment":"Dorian melody over a breathing shared room. Lane 3 (x3/4) breathes REVERB SEND; shimmer glitters +12 st. A touch of slow tape echo underneath."},
    {"action":"set_sync_mode","part":0,"value":"sync"},
    {"action":"set_rate","part":0,"value":0.12},
    {"action":"set_step","part":0,"flag":true,"ivalue":8},
    {"action":"set_shape","part":0,"value":0.3},
    {"action":"set_range","part":0,"value":0.45},
    {"action":"set_smooth","part":0,"value":0.5},
    {"action":"set_depth","part":0,"value":1.0},
    {"action":"set_target_active","part":0,"slot":2,"flag":true},
    {"action":"set_target_base","part":0,"slot":2,"value":0.55},
    {"action":"set_target_active","part":0,"slot":4,"flag":true},
    {"action":"set_target_base","part":0,"slot":4,"value":0.65},

    {"action":"set_reverb_size","value":0.85},
    {"action":"set_reverb_tone","value":0.55},
    {"action":"set_reverb_shimmer","value":0.6},
    {"action":"set_fx_target_active","part":0,"slot":3,"flag":true},
    {"action":"set_fx_target_base","part":0,"slot":3,"value":0.35},
    {"action":"set_fx_target_depth","part":0,"slot":3,"value":0.6},

    {"action":"set_fx_on","part":0,"value":"flux","flag":true},
    {"action":"set_flux_mix","part":0,"value":0.35},
    {"action":"set_fx_target_base","part":0,"slot":1,"value":0.55},
    {"action":"set_fx_target_base","part":0,"slot":4,"value":0.4},

    {"_comment":"PART B - low slow pad, also breathing into the room on its own lane 3."},
    {"action":"set_sync_mode","part":1,"value":"free"},
    {"action":"set_rate","part":1,"value":0.05},
    {"action":"set_smooth","part":1,"value":0.7},
    {"action":"set_range","part":1,"value":0.2},
    {"action":"set_depth","part":1,"value":1.0},
    {"action":"set_target_active","part":1,"slot":2,"flag":true},
    {"action":"set_target_base","part":1,"slot":2,"value":0.2},
    {"action":"set_target_active","part":1,"slot":4,"flag":true},
    {"action":"set_target_base","part":1,"slot":4,"value":0.5},
    {"action":"set_fx_target_active","part":1,"slot":3,"flag":true},
    {"action":"set_fx_target_base","part":1,"slot":3,"value":0.3},
    {"action":"set_fx_target_depth","part":1,"slot":3,"value":0.5}
  ],
  "events": [
    {"t":32.0,"action":"set_reverb_shimmer","value":0.85},
    {"_comment":"32s: more glitter as the wash peaks."}
  ]
}
```

- [ ] **Step 3: Render both demos + verify the CSV acceptance criteria**

```bash
./build/render host/render/scenarios/dub_delay.json renders/dub_delay.wav renders/dub_delay.csv
./build/render host/render/scenarios/ambient_wash.json renders/ambient_wash.wav renders/ambient_wash.csv
python - <<'EOF'
import csv
rows = list(csv.DictReader(open('renders/dub_delay.csv')))
steps = sorted({round(float(r['a_fx1']), 3) for r in rows})
assert len(steps) >= 4, f"FLUX TIME should step, got {steps}"
fb = [float(r['a_fx4']) for r in rows]
assert max(fb) - min(fb) > 0.15, "FLUX FEEDBACK should swell"
rows = list(csv.DictReader(open('renders/ambient_wash.csv')))
send = [float(r['a_fx3']) for r in rows]
assert max(send) - min(send) > 0.2, "REVERB SEND should breathe"
print("csv acceptance ok")
EOF
```

Expected: `csv acceptance ok`.

- [ ] **Step 4: Verify determinism and the CPU early indicator**

```bash
./build/render host/render/scenarios/dub_delay.json /tmp/d1.wav /tmp/d1.csv
./build/render host/render/scenarios/dub_delay.json /tmp/d2.wav /tmp/d2.csv
cmp /tmp/d1.wav /tmp/d2.wav && echo "deterministic"
time ./build/render host/render/scenarios/dorian_melody.json /tmp/base.wav /tmp/base.csv
time ./build/render host/render/scenarios/ambient_wash.json /tmp/fx.wav /tmp/fx.csv
```

Expected: `deterministic`; ambient_wash (48 s, all FX + shimmer) real time ÷ dorian_melody (48 s, no FX) real time **< 2×** (spec acceptance). Record both numbers in the commit message body.

- [ ] **Step 5: Listen**

Open `renders/dub_delay.wav` and `renders/ambient_wash.wav`. By-ear acceptance (spec): dub delay pitch-jumps when the STEP lane fires and swells after 20 s; ambient wash's room breathes and the tail glitters an octave up, more after 32 s. If GRIT INTENSITY or the mappings feel wrong, adjust base/depth values in the JSONs (not the engine curves) first; engine curve changes are a spec "assumption to verify" and belong in a follow-up conversation with the user.

- [ ] **Step 6: Update docs**

`docs/roadmap.md`:
- Status table: `| **M1.6** | ... | ✅ **done** (engine + host; UI wiring deferred to M6) |`
- Add to the **Done** section (after the scales entry), following its style:

```markdown
### M1.6 — FX ✅

Per-part FLUX (tape echo) + GRIT (drive/reduce) ported from the original
firmware, a shared ambient reverb (DaisySP `ReverbSc` + optional +12 st
shimmer), and 5 curated FX parameters per part as first-class modulation
targets — a second tap on the same five lanes (fixed 1:1 lane → target
mapping, `engine/fx/part_fx.h`).

- **`engine/fx/`** — `fx_util.h` (XFade/SoftSwitch ports), `grit.*`, `flux.*`,
  `reverb.*`, `part_fx.*`. DaisySP is now an `engine/` dependency (portable
  C++; still no libDaisy). Memory is injected (`FxMem`): echo buffers +
  reverb object — static on desktop, SDRAM on Daisy (M6).
- **Signal flow** — per part: engine → GRIT → FLUX → FX MIX (equal-gain
  dry/wet); post-FX send × REVERB SEND (equal-power) into the shared room,
  which joins the master after the part mix. Bypass is bit-exact.
- **Host** — 10 new scenario actions, 5 FX columns per part in `mods.csv`,
  demo scenarios `dub_delay.json` / `ambient_wash.json`.
- **UI (M6)** — FLUX/GRIT pads, hold-layers, ALT gestures per the FX spec.
```

`README.md` line 103 — change the M1.6 status cell from `planned` to `**done** (engine + host)`:

```markdown
| **M1.6** | FX: per-part FLUX (tape echo) + GRIT (drive/reduce), shared ambient reverb, FX params as modulation targets | **done** (engine + host) |
```

`THIRD_PARTY.md`: in the "Note on DaisySP-LGPL" section, replace the last sentence (`The portable engine/ core ... is unaffected.`) with:

```markdown
As of M1.6 the portable `engine/` core depends on DaisySP (MIT) for its FX
blocks — still no libDaisy — and `engine/fx/reverb.*` links the LGPL-2.1
`ReverbSc` module from `DaisySP-LGPL`. Desktop test/render binaries and any
distributed firmware binary therefore link LGPL code, with the relinking/
attribution obligations noted above. Distributing this source repository
imposes no such obligation.
```

Also update the fork-authored port provenance: `engine/fx/{fx_util,flux}.h` contain ports of `src/core/{xfade,softswitch,hann,deline,biquad,echo}.h` (MIT, © Infrasonic Audio LLC / shensley) — add one line noting this if the file lists per-file provenance.

- [ ] **Step 7: Full build + tests one last time, then commit**

Run: `source env.sh && cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS.

```bash
git add host/render/scenarios/dub_delay.json host/render/scenarios/ambient_wash.json renders/dub_delay.wav renders/dub_delay.csv renders/ambient_wash.wav renders/ambient_wash.csv docs/roadmap.md README.md THIRD_PARTY.md
git commit -m "feat(host): dub_delay + ambient_wash demos; docs: M1.6 done"
```

(Only commit the renders if the repo already tracks `renders/` — it does: `git ls-files renders/` shows the M1 WAVs. Follow that precedent.)

---

## Acceptance criteria traceability (spec → plan)

| Spec criterion | Where verified |
|---|---|
| `engine/fx/` compiles desktop, no libDaisy, DaisySP only | Tasks 2–6 builds; include audit in self-review |
| `dub_delay.json`: FLUX TIME steps in csv + audible pitch jumps, feedback swells | Task 10 steps 3+5 |
| `ambient_wash.json`: send breathes with lane 3; shimmer glitters +12 | Task 10 steps 3+5 |
| FLUX+GRIT off, send 0 → bit-identical to no-FX | Task 8 test "bit-identical to the no-FX build" |
| All new unit tests pass; existing stay green | every task's ctest step |
| CPU early indicator < 2× M1 baseline | Task 10 step 4 |

## Notes for the implementer

- The `engine` include dir is the root for engine includes (`#include "fx/grit.h"`), and `daisysp_min`'s PUBLIC dirs make `#include "Effects/..."`/`"Utility/dsp.h"` work — no other include plumbing needed.
- If the bit-exact checks (`CHECK(l == s)`) fail with denormal-looking noise, the chain-skip condition in `PartFx::process` is the first suspect — both blocks must report `engaged() == false`.
- `test_flux.cpp`'s static buffers are shared between cases: `Flux::init` calls `DeLine::Reset()` (memset) — every case must call `init` first (they all do).
- Windows stack is 1 MB: keep `ReverbSc`/`PitchShifter`/`AmbientReverb`/echo buffers static everywhere, including any new test you add later.






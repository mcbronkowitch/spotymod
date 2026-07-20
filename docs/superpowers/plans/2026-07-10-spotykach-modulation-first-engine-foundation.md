# Spotykach Modulation-First — Milestone 1: Engine Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the portable `engine/` core, the five-lane SuperModulator bank (the spec's declared core deliverable — "make it audible first"), a desktop render host (scenario JSON → WAV + CSV), and the unit-test harness — all fully testable on the desktop with no hardware.

**Architecture:** One platform-independent C++ core under `engine/` (no libDaisy/DaisySP includes, no heap in the audio path) exposed through a single `Instrument` class. Each of two `Part`s owns a `SuperModulator` (five independent `ModLane`s behind one macro surface) feeding a placeholder `TestToneEngine` (replaced by the real synth/sampler in later milestones). A CMake desktop build produces the `render` CLI and a doctest test binary. All randomness comes from a deterministic xorshift PRNG so renders and tests are bit-reproducible.

**Tech Stack:** C++17, CMake (Windows), doctest (unit tests) and nlohmann/json (scenario parsing) as **vendored single headers** under `third_party/`. No third-party code enters `engine/`.

**Milestone context:** This is **Plan 1 of 6**. It implements build-order step 1 from the spec (`## Build order`): *Modulator bank (5 lanes) + unit tests + render host*. Milestones 2–6 (polyphonic synth voice, capture sequencer, center section, sampler adapter, firmware shell) each get their own plan and build on the interfaces produced here. The full acceptance demo ("overlapping voices") needs the M2 synth voice; M1 delivers the audible test-tone plus the modulation CSV that later milestones extend.

**Repository:** All code lands in the firmware fork at `c:\Users\bernd\Documents\AI\Spotykach`. M1 adds only new directories (`engine/`, `host/`, `tests/`) and a root `CMakeLists.txt`. It does **not** touch `src/`, the `Makefile`, or any existing firmware file, so the original firmware stays buildable unchanged.

## Global Constraints

Every task's requirements implicitly include this section. Copy values verbatim.

- **Language / standard:** C++17 (`set(CMAKE_CXX_STANDARD 17)`).
- **Namespace:** all new engine and host code is in `namespace spky` (distinct from the existing firmware `namespace spotykach`, which the shell reconciles in M6).
- **No hardware types in `engine/`:** no `#include <daisy.h>`, `<daisysp.h>`, or anything under `src/`. The engine compiles standalone.
- **No heap / no allocation in the audio path:** engine members are fixed-size (`std::array`, plain fields). No `new`, `malloc`, `std::vector`, or `std::function` inside `engine/`. `std::vector`/file I/O are allowed only in `host/` and `tests/`.
- **Determinism:** all randomness uses the engine's `spky::Rng` (xorshift32). Never `std::rand`, `rand()`, `<random>` distributions, or time-based seeds anywhere in `engine/`.
- **Parameter smoothing:** knob→engine slews go through `spky::OnePole` (the portable copy of the firmware's one-pole smoother).
- **Sample rate / block:** engine is sample-rate agnostic via `init(sample_rate)`. Firmware target is 96 samples @ 48 kHz; the offline render host processes sample-by-sample (block size is a firmware CPU concern, not an offline one).
- **Do not modify** `src/`, `Makefile`, `main.cpp`, `app.cpp`, or `app.h`.

## File Structure

New files, each with one responsibility:

```
Spotykach/  (fork root)
├── CMakeLists.txt                    desktop build: render CLI + doctest tests
├── env.sh                            build-env preamble (clang+ninja PATH; gitignored)
├── third_party/                      vendored single headers (desktop only)
│   ├── doctest/doctest.h
│   └── nlohmann/json.hpp
├── engine/                           portable core (no libDaisy)
│   ├── util/math.h                   TWO_PI, clampf, lerpf
│   ├── util/onepole.h                OnePole smoother (portable)
│   ├── mod/lane_id.h                 LaneId enum, SyncMode enum
│   ├── mod/rng.h                     Rng — deterministic xorshift32
│   ├── mod/waveforms.h               waveform bank + continuous morph
│   ├── mod/range.h                   RANGE mapping (off → unipolar → bipolar)
│   ├── mod/lane.{h,cpp}              ModLane — one modulation lane
│   ├── mod/super_modulator.{h,cpp}   SuperModulator — 5 lanes + macros
│   ├── parts/engine_iface.h          IPartEngine interface
│   ├── parts/test_tone_engine.h      TestToneEngine (M1 placeholder engine)
│   ├── parts/part.{h,cpp}            Part — SuperModulator + engine + targets
│   └── instrument.{h,cpp}            Instrument — the single public API
├── host/
│   └── render/
│       ├── wav_writer.h              16-bit PCM stereo WAV writer
│       ├── scenario.{h,cpp}          scenario JSON parsing + event apply
│       ├── main.cpp                  render CLI
│       └── scenarios/demo_step_melody.json
└── tests/
    ├── test_main.cpp                 doctest main
    ├── smoke.cpp
    ├── test_rng.cpp
    ├── test_waveforms.cpp
    ├── test_range.cpp
    ├── test_lane.cpp
    ├── test_step.cpp
    ├── test_evolve.cpp
    ├── test_super_modulator.cpp
    ├── test_part.cpp
    ├── test_instrument.cpp
    ├── test_wav.cpp
    └── test_scenario.cpp
```

**How the build grows:** `CMakeLists.txt` keeps `spky_engine` as an INTERFACE target (it only carries the `engine` include directory — header-only until the first `.cpp` exists). Engine `.cpp` files and test `.cpp` files are compiled **directly into the `spky_tests` executable**, and each task appends its new sources to that target's source list. The `render` executable (Task 13) lists the engine `.cpp` files it needs. This avoids an empty-static-library problem in the early header-only tasks.

### Build environment (this machine — validated 2026-07-10)

There is **no MSVC/Visual Studio and no native GNU toolchain** on PATH — only an ARM cross-compiler (DaisyToolchain) for the firmware. The desktop host build uses **LLVM/Clang + Ninja**, both present but not on the default Git Bash PATH. Before any `cmake`/`ctest` call, source this preamble (put it in a `env.sh` at the fork root, or paste it):

```bash
# Desktop build environment for the Spotykach engine host (Git Bash)
export PATH="/c/Program Files/LLVM/bin:/c/Users/bernd/AppData/Roaming/Python/Python314/Scripts:$PATH"
export CC=clang CXX=clang++ CMAKE_GENERATOR=Ninja
```

- `clang++` = LLVM 22, target `x86_64-pc-windows-msvc` (a working host STL is present — verified compiling `<vector>`/`<cmath>` and nlohmann/json).
- `ninja` was installed via `python -m pip install ninja` (Python 3.14 present); its `ninja.exe` lives in `…/Python314/Scripts`.
- doctest/json are **vendored single headers** (no FetchContent) — avoids the Windows `MAX_PATH` failure that a full doctest git-clone hits under the deep session path, and needs no network at configure time.

**Build & test commands** (used throughout; run from the fork root `c:\Users\bernd\Documents\AI\Spotykach` after sourcing the preamble):

```bash
cmake -S . -B build                          # configure (first time / after CMakeLists edits)
cmake --build build                          # compile (Ninja)
ctest --test-dir build --output-on-failure   # run the whole doctest suite
```

With the Ninja single-config generator the `spky_tests` / `render` binaries are directly under `build/`. `ctest --test-dir build` finds them regardless.

---

## Task 1: Project scaffold — CMake, util headers, doctest smoke test

**Files:**
- Create: `CMakeLists.txt`
- Create: `engine/util/math.h`
- Create: `engine/util/onepole.h`
- Create: `engine/mod/lane_id.h`
- Create: `tests/test_main.cpp`
- Create: `tests/smoke.cpp`

**Interfaces:**
- Produces: `spky::TWO_PI`, `spky::clampf(x,lo,hi)`, `spky::lerpf(a,b,t)`; `spky::OnePole` with `init(sr,time_s)`, `process(target)→float`, `reset(value)`, `value()`; `spky::LaneId {LANE_SOURCE=0,LANE_SIZE=1,LANE_PITCH=2,LANE_MOTION=3,LANE_LEVEL=4,LANE_COUNT=5}`; `spky::SyncMode {Sync,SyncTriplet,Free}`.

- [ ] **Step 1: Write the smoke test**

`tests/test_main.cpp`:
```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
```

`tests/smoke.cpp`:
```cpp
#include <doctest/doctest.h>

TEST_CASE("smoke: toolchain builds and runs") {
    CHECK(1 + 1 == 2);
}
```

- [ ] **Step 2: Create the util headers**

`engine/util/math.h`:
```cpp
#pragma once

namespace spky {

static constexpr float TWO_PI = 6.28318530717958647692f;

inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

inline float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

} // namespace spky
```

`engine/util/onepole.h`:
```cpp
#pragma once
#include <cmath>

namespace spky {

// Portable one-pole smoother (same math as the firmware's OnePoleSmoother,
// but with no libDaisy dependency).
class OnePole {
public:
    void init(float sample_rate, float time_s = 0.001f) {
        if (time_s <= 0.f || sample_rate <= 0.f) { _kof = 1.f; return; }
        float k = 1.f / (time_s * sample_rate);
        _kof = k > 1.f ? 1.f : k;
    }

    float process(float target) {
        float diff = target - _value;
        if (!_smoothing && std::fabs(diff) < 0.0005f) return _value;
        _smoothing = true;
        _value += _kof * diff;
        if (std::fabs(target - _value) < 0.0005f) {
            _value = target;
            _smoothing = false;
        }
        return _value;
    }

    void reset(float value = 0.f) { _value = value; _smoothing = false; }
    float value() const { return _value; }

private:
    float _kof = 1.f;
    float _value = 0.f;
    bool  _smoothing = false;
};

} // namespace spky
```

`engine/mod/lane_id.h`:
```cpp
#pragma once

namespace spky {

// Fixed target slots. The five pads keep the same function class in both
// engines (spec "Fixed target slots"). Lane index == pad slot == target slot.
enum LaneId {
    LANE_SOURCE = 0,   // Pad 1: POSITION / TIMBRE   (rate x2)
    LANE_SIZE   = 1,   // Pad 2: SIZE / FILTER       (rate x1/2)
    LANE_PITCH  = 2,   // Pad 3: PITCH (master lane) (rate x1)
    LANE_MOTION = 3,   // Pad 4: SHAPE / MOTION      (rate x3/4)
    LANE_LEVEL  = 4,   // Pad 5: LEVEL               (rate x3/2)
    LANE_COUNT  = 5
};

enum class SyncMode { Sync, SyncTriplet, Free };

} // namespace spky
```

- [ ] **Step 3: Vendor the doctest single header and create `env.sh`**

Download the single-header doctest into `third_party/` and write the build-environment preamble to `env.sh` at the fork root:

```bash
mkdir -p third_party/doctest
curl -fsSL -o third_party/doctest/doctest.h \
  https://raw.githubusercontent.com/doctest/doctest/v2.4.11/doctest/doctest.h

cat > env.sh <<'SH'
# Desktop build environment for the Spotykach engine host (source before cmake/ctest)
export PATH="/c/Program Files/LLVM/bin:/c/Users/bernd/AppData/Roaming/Python/Python314/Scripts:$PATH"
export CC=clang CXX=clang++ CMAKE_GENERATOR=Ninja
SH
```

Verify: `wc -c third_party/doctest/doctest.h` reports ~321 KB.

- [ ] **Step 4: Create `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.16)
project(spky LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Portable engine: header-only for now; carries the include dir.
add_library(spky_engine INTERFACE)
target_include_directories(spky_engine INTERFACE engine)

# --- third-party (vendored single headers under third_party/, desktop only) ---
add_library(doctest INTERFACE)
target_include_directories(doctest INTERFACE third_party)

# --- tests ---
enable_testing()
add_executable(spky_tests
    tests/test_main.cpp
    tests/smoke.cpp
)
target_link_libraries(spky_tests PRIVATE spky_engine doctest)
add_test(NAME spky_tests COMMAND spky_tests)
```

- [ ] **Step 5: Configure, build, run**

Run (source the preamble first):
```bash
source env.sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```
Expected: `cmake` selects the Ninja generator and `clang++`; build succeeds; `ctest` reports `100% tests passed, 0 tests failed out of 1` (the smoke test).

- [ ] **Step 6: Add `.gitignore` entries for the build dir**

Append to the existing `.gitignore` (do not remove existing lines):
```
/build/
/env.sh
/out.wav
/mods.csv
```

- [ ] **Step 7: Commit**

The vendored `third_party/doctest/doctest.h` is committed (small, keeps the build offline and reproducible); `env.sh` is machine-specific and gitignored.
```bash
git add CMakeLists.txt engine/util/math.h engine/util/onepole.h engine/mod/lane_id.h \
        tests/test_main.cpp tests/smoke.cpp third_party/doctest/doctest.h .gitignore
git commit -m "build: desktop CMake scaffold (clang+ninja) + vendored doctest + util headers"
```

---

## Task 2: Deterministic PRNG (`Rng`)

**Files:**
- Create: `engine/mod/rng.h`
- Test: `tests/test_rng.cpp`
- Modify: `CMakeLists.txt` (add `tests/test_rng.cpp` to `spky_tests`)

**Interfaces:**
- Produces: `spky::Rng` with `seed(uint32_t)`, `next_u32()→uint32_t`, `next_unipolar()→float [0,1)`, `next_bipolar()→float [-1,1)`.

- [ ] **Step 1: Write the failing tests**

`tests/test_rng.cpp`:
```cpp
#include <doctest/doctest.h>
#include "mod/rng.h"
using namespace spky;

TEST_CASE("rng: deterministic for a given seed") {
    Rng a, b;
    a.seed(12345);
    b.seed(12345);
    for (int i = 0; i < 1000; ++i) CHECK(a.next_u32() == b.next_u32());
}

TEST_CASE("rng: different seeds diverge") {
    Rng a, b;
    a.seed(1); b.seed(2);
    bool differ = false;
    for (int i = 0; i < 10; ++i) if (a.next_u32() != b.next_u32()) differ = true;
    CHECK(differ);
}

TEST_CASE("rng: unipolar in [0,1) and roughly uniform") {
    Rng r; r.seed(99);
    for (int i = 0; i < 100000; ++i) {
        float u = r.next_unipolar();
        CHECK(u >= 0.f);
        CHECK(u < 1.f);
    }
    Rng r2; r2.seed(7);
    double sum = 0; int n = 200000;
    for (int i = 0; i < n; ++i) sum += r2.next_unipolar();
    CHECK(sum / n == doctest::Approx(0.5).epsilon(0.02));
}
```

- [ ] **Step 2: Add the test to the build and run — expect RED**

In `CMakeLists.txt`, add `tests/test_rng.cpp` to the `spky_tests` source list:
```cmake
add_executable(spky_tests
    tests/test_main.cpp
    tests/smoke.cpp
    tests/test_rng.cpp
)
```
Run:
```bash
cmake -S . -B build
cmake --build build
```
Expected: build FAILS — `fatal error: mod/rng.h: No such file or directory` (test references a header that does not exist yet).

- [ ] **Step 3: Implement `Rng`**

`engine/mod/rng.h`:
```cpp
#pragma once
#include <cstdint>

namespace spky {

// Deterministic xorshift32 PRNG. No global state, no time seeding — the
// engine must be bit-reproducible across desktop and firmware (capture
// determinism, testable probability statistics).
class Rng {
public:
    void seed(uint32_t s) { _state = s ? s : 0x1u; }

    uint32_t next_u32() {
        uint32_t x = _state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        _state = x;
        return x;
    }

    float next_unipolar() { return next_u32() * (1.f / 4294967296.f); } // [0,1)
    float next_bipolar()  { return next_unipolar() * 2.f - 1.f; }        // [-1,1)

private:
    uint32_t _state = 0x1u;
};

} // namespace spky
```

- [ ] **Step 4: Build and run — expect GREEN**

Run:
```bash
cmake --build build
ctest --test-dir build --output-on-failure
```
Expected: PASS (all rng test cases green).

- [ ] **Step 5: Commit**

```bash
git add engine/mod/rng.h tests/test_rng.cpp CMakeLists.txt
git commit -m "feat(engine): deterministic xorshift Rng"
```

---

## Task 3: Waveform bank + continuous morph

**Files:**
- Create: `engine/mod/waveforms.h`
- Test: `tests/test_waveforms.cpp`
- Modify: `CMakeLists.txt` (add `tests/test_waveforms.cpp`)

**Interfaces:**
- Consumes: `spky::TWO_PI`, `clampf`, `lerpf` (Task 1).
- Produces: `spky::wave_sine/triangle/ramp/pulse(float ph)→float`; `spky::shape_value(float ph, float shape, float sh_hold)→float bipolar [-1,1]`, morphing sine→triangle→ramp→pulse→S&H(random) as `shape` sweeps 0..1.

- [ ] **Step 1: Write the failing tests**

`tests/test_waveforms.cpp`:
```cpp
#include <doctest/doctest.h>
#include "mod/waveforms.h"
using namespace spky;

TEST_CASE("waveforms: canonical shapes at bank anchor points") {
    CHECK(shape_value(0.25f, 0.00f, 0.f) == doctest::Approx(1.0f));   // sine peak
    CHECK(shape_value(0.50f, 0.25f, 0.f) == doctest::Approx(1.0f));   // triangle peak
    CHECK(shape_value(0.50f, 0.50f, 0.f) == doctest::Approx(0.0f));   // ramp mid
    CHECK(shape_value(0.25f, 0.75f, 0.f) == doctest::Approx(1.0f));   // pulse high
    CHECK(shape_value(0.75f, 0.75f, 0.f) == doctest::Approx(-1.0f));  // pulse low
}

TEST_CASE("waveforms: S&H end returns the held random value") {
    CHECK(shape_value(0.1f, 1.0f,  0.42f) == doctest::Approx( 0.42f).epsilon(0.01));
    CHECK(shape_value(0.9f, 1.0f, -0.31f) == doctest::Approx(-0.31f).epsilon(0.01));
}

TEST_CASE("waveforms: output stays bipolar for all shapes") {
    for (int i = 0; i <= 20; ++i) {
        float ph = i / 20.f;
        for (int s = 0; s <= 10; ++s) {
            float v = shape_value(ph, s / 10.f, 0.5f);
            CHECK(v >= -1.001f);
            CHECK(v <=  1.001f);
        }
    }
}
```

- [ ] **Step 2: Add to build and run — expect RED**

Add `tests/test_waveforms.cpp` to `spky_tests` in `CMakeLists.txt`, then:
```bash
cmake -S . -B build && cmake --build build
```
Expected: build FAILS — `mod/waveforms.h: No such file or directory`.

- [ ] **Step 3: Implement the waveform bank**

`engine/mod/waveforms.h`:
```cpp
#pragma once
#include <cmath>
#include "util/math.h"

namespace spky {

inline float wave_sine(float ph)     { return std::sin(ph * TWO_PI); }
inline float wave_triangle(float ph) { return ph < 0.5f ? (-1.f + 4.f * ph) : (3.f - 4.f * ph); }
inline float wave_ramp(float ph)     { return 2.f * ph - 1.f; }
inline float wave_pulse(float ph)    { return ph < 0.5f ? 1.f : -1.f; }

// Continuous morph across the bank as `shape` sweeps 0..1:
//   sine -> triangle -> ramp -> pulse -> sample&hold(random).
// `ph` is lane phase in [0,1); `sh_hold` is the per-cycle random value used at
// the S&H end. Returns bipolar [-1, 1].
inline float shape_value(float ph, float shape, float sh_hold) {
    shape = clampf(shape, 0.f, 1.f);
    float seg = shape * 4.f;
    if (seg >= 3.9999f) seg = 3.9999f;   // keep S&H reachable at shape == 1
    int   i = static_cast<int>(seg);
    float f = seg - i;
    switch (i) {
        case 0:  return lerpf(wave_sine(ph),     wave_triangle(ph), f);
        case 1:  return lerpf(wave_triangle(ph), wave_ramp(ph),     f);
        case 2:  return lerpf(wave_ramp(ph),     wave_pulse(ph),    f);
        default: return lerpf(wave_pulse(ph),    sh_hold,           f);
    }
}

} // namespace spky
```

- [ ] **Step 4: Build and run — expect GREEN**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add engine/mod/waveforms.h tests/test_waveforms.cpp CMakeLists.txt
git commit -m "feat(engine): waveform bank with continuous shape morph"
```

---

## Task 4: RANGE mapping

**Files:**
- Create: `engine/mod/range.h`
- Test: `tests/test_range.cpp`
- Modify: `CMakeLists.txt` (add `tests/test_range.cpp`)

**Interfaces:**
- Consumes: `spky::clampf`, `lerpf` (Task 1).
- Produces: `spky::apply_range(float v, float r)→float`. Monotonic: `r=0` off, `r=0.5` full unipolar, `r=1` full bipolar.

- [ ] **Step 1: Write the failing tests**

`tests/test_range.cpp`:
```cpp
#include <doctest/doctest.h>
#include "mod/range.h"
using namespace spky;

TEST_CASE("range: minimum is off") {
    CHECK(apply_range( 1.f, 0.f) == doctest::Approx(0.f));
    CHECK(apply_range(-1.f, 0.f) == doctest::Approx(0.f));
    CHECK(apply_range( 0.f, 0.f) == doctest::Approx(0.f));
}

TEST_CASE("range: full unipolar at mid travel") {
    CHECK(apply_range( 1.f, 0.5f) == doctest::Approx(1.f));
    CHECK(apply_range(-1.f, 0.5f) == doctest::Approx(0.f));
    CHECK(apply_range( 0.f, 0.5f) == doctest::Approx(0.5f));
}

TEST_CASE("range: full bipolar at maximum") {
    CHECK(apply_range( 1.f, 1.f) == doctest::Approx( 1.f));
    CHECK(apply_range(-1.f, 1.f) == doctest::Approx(-1.f));
    CHECK(apply_range( 0.f, 1.f) == doctest::Approx( 0.f));
}

TEST_CASE("range: monotonic in v for every fixed r") {
    for (int ri = 0; ri <= 10; ++ri) {
        float r = ri / 10.f;
        float prev = apply_range(-1.f, r);
        for (int vi = -9; vi <= 10; ++vi) {
            float cur = apply_range(vi / 10.f, r);
            CHECK(cur >= prev - 1e-5f);
            prev = cur;
        }
    }
}
```

- [ ] **Step 2: Add to build and run — expect RED**

Add `tests/test_range.cpp` to `spky_tests`, then `cmake -S . -B build && cmake --build build`.
Expected: build FAILS — `mod/range.h: No such file or directory`.

- [ ] **Step 3: Implement `apply_range`**

`engine/mod/range.h`:
```cpp
#pragma once
#include "util/math.h"

namespace spky {

// RANGE mapping (spec): monotonic, minimum = off, opening unipolar through the
// first half, widening to full bipolar at maximum.
//   r <= 0      : off (0)
//   r in 0..0.5 : unipolar, amplitude 0..1
//   r in 0.5..1 : blend unipolar -> full bipolar
// `v` is the bipolar lane value [-1,1].
inline float apply_range(float v, float r) {
    r = clampf(r, 0.f, 1.f);
    if (r <= 0.f) return 0.f;
    float uni = v * 0.5f + 0.5f;          // [0,1]
    if (r <= 0.5f) return uni * (r * 2.f); // unipolar 0..amp
    float t = (r - 0.5f) * 2.f;           // 0..1
    return lerpf(uni, v, t);              // unipolar -> bipolar
}

} // namespace spky
```

- [ ] **Step 4: Build and run — expect GREEN**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add engine/mod/range.h tests/test_range.cpp CMakeLists.txt
git commit -m "feat(engine): RANGE mapping (off -> unipolar -> bipolar)"
```

---

## Task 5: `ModLane` — FLOW mode (phasor, probability freeze, smooth, range)

One lane's full signal path in FLOW (continuous) mode. STEP and EVOLVE are added in Tasks 6–7.

**Files:**
- Create: `engine/mod/lane.h`
- Create: `engine/mod/lane.cpp`
- Test: `tests/test_lane.cpp`
- Modify: `CMakeLists.txt` (add `engine/mod/lane.cpp` and `tests/test_lane.cpp` to `spky_tests`)

**Interfaces:**
- Consumes: `spky::OnePole`, `Rng`, `shape_value`, `apply_range`, `clampf`.
- Produces: `spky::ModLane` with `init(float sr, uint32_t seed)`, setters `set_rate_hz(float)`, `set_shape(float)`, `set_probability(float)`, `set_step(bool,int)`, `set_fixed_slew(bool)`, `set_smooth(float)`, `set_range(float)`, `set_evolve(float)`; `process()→float` (post-range bipolar); queries `fired()→bool`, `frozen()→bool`, `phase()→float`, `target()→float` (pre-smooth held value); `reset(float phase=0)`. The pipeline is: wavetable core → probability (dice per cycle in FLOW) → step/flow → smooth (`OnePole`) → range. Later tasks add STEP-boundary dicing (Task 6) and EVOLVE random walk (Task 7).

- [ ] **Step 1: Write the failing FLOW tests**

`tests/test_lane.cpp`:
```cpp
#include <doctest/doctest.h>
#include <cmath>
#include "mod/lane.h"
using namespace spky;

static void configure_flow(ModLane& l, float hz, float prob = 1.f) {
    l.init(48000.f, 1234);
    l.set_range(1.f);
    l.set_shape(0.5f);        // ramp
    l.set_smooth(0.f);
    l.set_probability(prob);
    l.set_rate_hz(hz);
}

TEST_CASE("lane FLOW: rate accuracy — ~2 fires per second") {
    ModLane l; configure_flow(l, 2.f);
    const int seconds = 5;
    int fires = 0;
    for (int i = 0; i < 48000 * seconds; ++i) { l.process(); if (l.fired()) ++fires; }
    // A free-running float phasor never closes a cycle in EXACTLY N samples, so
    // assert the fire RATE over a multi-second window (+/-1), not exact closure.
    CHECK(fires >= 2 * seconds - 1);   // ~10
    CHECK(fires <= 2 * seconds + 1);
}

TEST_CASE("lane FLOW: output stays in range") {
    ModLane l; configure_flow(l, 3.f);
    for (int i = 0; i < 48000; ++i) {
        float v = l.process();
        CHECK(v >= -1.001f);
        CHECK(v <=  1.001f);
    }
}

TEST_CASE("lane FLOW: probability 0 freezes after the first cycle") {
    ModLane l; configure_flow(l, 4.f, 0.f);
    for (int i = 0; i < 48000; ++i) l.process();   // >= one full cycle
    CHECK(l.frozen());
}

TEST_CASE("lane: SMOOTH turns a step into a glide") {
    ModLane l;
    l.init(48000.f, 55);
    l.set_range(1.f);
    l.set_shape(0.5f);        // ramp: consecutive step values differ
    l.set_step(true, 2);      // 2 steps/cycle; boundary at phase 0.5
    l.set_probability(1.f);
    l.set_smooth(0.5f);       // glide ~3 ms: settles well within a step, still gliding ~1 ms past a boundary
    l.set_rate_hz(1.f);       // 48000 samples/cycle -> step is 24000 samples

    for (int i = 0; i < 20000; ++i) l.process();   // settle in step 0
    float settled0 = l.process();
    float target0  = l.target();
    for (int i = 20002; i < 24050; ++i) l.process(); // cross into step 1
    float out_after = l.process();                   // ~1 ms past boundary
    float target1   = l.target();

    CHECK(target1 != doctest::Approx(target0));        // new value latched
    CHECK(std::fabs(out_after - target1) > 0.01f);     // output still gliding
    CHECK(std::fabs(settled0  - target0) < 0.01f);     // was settled before
}
```

- [ ] **Step 2: Add to build and run — expect RED**

Add both files to `spky_tests` in `CMakeLists.txt`:
```cmake
add_executable(spky_tests
    tests/test_main.cpp
    tests/smoke.cpp
    tests/test_rng.cpp
    tests/test_waveforms.cpp
    tests/test_range.cpp
    engine/mod/lane.cpp
    tests/test_lane.cpp
)
```
Run `cmake -S . -B build && cmake --build build`.
Expected: build FAILS — `mod/lane.h: No such file or directory`.

- [ ] **Step 3: Declare `ModLane`**

`engine/mod/lane.h`:
```cpp
#pragma once
#include <cstdint>
#include "util/onepole.h"
#include "mod/rng.h"

namespace spky {

// One modulation lane: wavetable core -> probability -> step/flow -> smooth
// -> range. Bipolar output in [-1,1]. Deterministic given its seed.
class ModLane {
public:
    void init(float sample_rate, uint32_t seed);

    void set_rate_hz(float hz);
    void set_shape(float s);          // 0..1
    void set_probability(float p);    // 0..1
    void set_step(bool on, int steps_per_cycle);
    void set_fixed_slew(bool on);     // panel switch 3 middle position
    void set_smooth(float s);         // 0..1
    void set_range(float r);          // 0..1
    void set_evolve(float amount);    // 0 = LOOP (deterministic)

    float process();                  // advance one sample, return post-range value

    bool  fired()  const { return _fired; }   // true on the sample a boundary fired
    bool  frozen() const { return _frozen; }  // last dice failed -> holding
    float phase()  const { return _phase; }
    float target() const { return _target; }  // pre-smooth, pre-range held value

    void reset(float phase = 0.f);

private:
    void  _update_slew();
    void  _on_boundary();
    float _compute_raw() const;

    Rng     _rng;
    OnePole _slew;

    float _sr = 48000.f;
    float _phase = 0.f;
    float _phase_inc = 0.f;
    float _shape = 0.f;
    float _prob = 1.f;
    float _range = 1.f;
    float _smooth = 0.f;
    float _evolve = 0.f;

    bool  _step_mode = false;
    int   _steps = 8;
    bool  _fixed_slew = false;

    int   _cur_step = -1;
    float _sh_cycle = 0.f;   // per-cycle random for the S&H end of the bank
    float _target = 0.f;     // pre-smooth held value
    bool  _fired = false;
    bool  _frozen = false;

    float _ev_phase = 0.f;   // EVOLVE random-walk offsets: shape / phase / rate (Task 7)
    float _ev_shape = 0.f;
    float _ev_rate  = 0.f;
};

} // namespace spky
```

- [ ] **Step 4: Implement `ModLane` (FLOW path complete; STEP branch present but exercised in Task 6)**

`engine/mod/lane.cpp`:
```cpp
#include "mod/lane.h"
#include "mod/waveforms.h"
#include "mod/range.h"
#include "util/math.h"
#include <cmath>

using namespace spky;

void ModLane::init(float sample_rate, uint32_t seed) {
    _sr = sample_rate;
    _rng.seed(seed);
    _phase = 0.f;
    _cur_step = -1;
    _sh_cycle = _rng.next_bipolar();
    _target = 0.f;
    _fired = false;
    _frozen = false;
    _ev_phase = 0.f;
    _ev_shape = 0.f;
    _ev_rate  = 0.f;
    _update_slew();
    _slew.reset(0.f);
}

void ModLane::set_rate_hz(float hz)   { _phase_inc = (hz > 0.f ? hz : 0.f) / _sr; }
void ModLane::set_shape(float s)      { _shape = clampf(s, 0.f, 1.f); }
void ModLane::set_probability(float p){ _prob = clampf(p, 0.f, 1.f); }
void ModLane::set_range(float r)      { _range = clampf(r, 0.f, 1.f); }
void ModLane::set_evolve(float a)     { _evolve = clampf(a, 0.f, 1.f); }

void ModLane::set_step(bool on, int steps) {
    _step_mode = on;
    _steps = steps < 1 ? 1 : steps;
}

void ModLane::set_smooth(float s) {
    _smooth = clampf(s, 0.f, 1.f);
    _update_slew();
}

void ModLane::set_fixed_slew(bool on) {
    _fixed_slew = on;
    _update_slew();
}

void ModLane::_update_slew() {
    // smooth 0 -> ~1 sample (near passthrough), smooth 1 -> ~0.5 s.
    float t = _fixed_slew ? 0.02f : (0.00002f * std::pow(25000.f, _smooth));
    _slew.init(_sr, t);
}

void ModLane::reset(float phase) {
    _phase = clampf(phase, 0.f, 0.999999f);
    _cur_step = -1;
    _slew.reset(_target);
}

float ModLane::_compute_raw() const {
    float ph = _phase + _ev_phase;
    ph -= std::floor(ph);
    float sh = clampf(_shape + _ev_shape, 0.f, 1.f);
    return shape_value(ph, sh, _sh_cycle);
}

void ModLane::_on_boundary() {
    bool fire = _rng.next_unipolar() < _prob;
    _frozen = !fire;
    if (fire) {
        _fired = true;
        _target = _compute_raw();   // latch the value at this boundary
    }
    // if !fire: hold the previous _target (frozen)
}

float ModLane::process() {
    _fired = false;

    _phase += _phase_inc * (1.f + _ev_rate);           // EVOLVE also wanders the rate
    bool wrapped = false;
    while (_phase >= 1.f) { _phase -= 1.f; wrapped = true; }

    if (wrapped) {
        _sh_cycle = _rng.next_bipolar();               // new S&H value per cycle
        if (_evolve > 0.f) {                           // EVOLVE: shape / phase / rate random walk (Task 7)
            _ev_phase = clampf(_ev_phase + _rng.next_bipolar() * 0.01f * _evolve, -0.5f, 0.5f);
            _ev_shape = clampf(_ev_shape + _rng.next_bipolar() * 0.02f * _evolve, -0.25f, 0.25f);
            _ev_rate  = clampf(_ev_rate  + _rng.next_bipolar() * 0.01f * _evolve, -0.2f, 0.2f);
        }
    }

    if (_step_mode) {
        int step = static_cast<int>(_phase * _steps);
        if (step >= _steps) step = _steps - 1;
        if (step != _cur_step) {
            _cur_step = step;
            _on_boundary();
        }
    } else {
        if (wrapped) _on_boundary();
        if (!_frozen) _target = _compute_raw();        // continuous in FLOW
    }

    float smoothed = _slew.process(_target);
    return apply_range(smoothed, _range);
}
```

- [ ] **Step 5: Build and run — expect GREEN**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```
Expected: PASS (rate ~10 fires over 5 s, probability-0 freeze, range bounds, and the glide settles within a step yet is still moving ~1 ms past a boundary).

- [ ] **Step 6: Commit**

```bash
git add engine/mod/lane.h engine/mod/lane.cpp tests/test_lane.cpp CMakeLists.txt
git commit -m "feat(engine): ModLane FLOW mode — phasor, probability freeze, smooth, range"
```

---

## Task 6: `ModLane` — STEP mode (quantization, per-step dice, fixed slew)

The STEP branch already exists in `lane.cpp` from Task 5; this task proves and pins its behavior with tests. If a test reveals a defect, fix `lane.cpp` — but no new interface is added.

**Files:**
- Test: `tests/test_step.cpp`
- Modify: `CMakeLists.txt` (add `tests/test_step.cpp`)
- Possibly modify: `engine/mod/lane.cpp` (only if a test fails)

**Interfaces:**
- Consumes: `spky::ModLane` (Task 5). No new symbols produced.

- [ ] **Step 1: Write the failing STEP tests**

`tests/test_step.cpp`:
```cpp
#include <doctest/doctest.h>
#include "mod/lane.h"
using namespace spky;

TEST_CASE("lane STEP: fires once per step") {
    ModLane l;
    l.init(48000.f, 7);
    l.set_range(1.f); l.set_shape(0.5f); l.set_smooth(0.f);
    l.set_probability(1.f);
    l.set_step(true, 4);
    l.set_rate_hz(1.f);
    int fires = 0;
    // Count over LESS than one cycle (47000 < ~48000) so the free-running float
    // phasor does not wrap and re-enter step 0 with a spurious 5th fire.
    for (int i = 0; i < 47000; ++i) { l.process(); if (l.fired()) ++fires; }
    CHECK(fires == 4);
}

TEST_CASE("lane STEP: target held constant within a step") {
    ModLane l;
    l.init(48000.f, 7);
    l.set_range(1.f); l.set_shape(0.5f); l.set_smooth(0.f);
    l.set_probability(1.f);
    l.set_step(true, 4);      // step 0 spans samples [0, 12000)
    l.set_rate_hz(1.f);
    for (int i = 0; i < 3000; ++i) l.process();
    float a = l.target();
    for (int i = 0; i < 5000; ++i) l.process();   // still step 0 (~sample 8000)
    float b = l.target();
    CHECK(a == doctest::Approx(b));
}

TEST_CASE("lane STEP: probability ~0.5 thins steps, deterministically") {
    auto count_fires = [](uint32_t seed) {
        ModLane l;
        l.init(48000.f, seed);
        l.set_range(1.f); l.set_shape(0.5f); l.set_smooth(0.f);
        l.set_probability(0.5f);
        l.set_step(true, 32);   // 32 steps/cycle
        l.set_rate_hz(1.f);     // 1 cycle/sec
        int fires = 0;
        for (int i = 0; i < 48000 * 20; ++i) { l.process(); if (l.fired()) ++fires; }
        return fires;
    };
    const int total_steps = 32 * 20;   // 640
    int f1 = count_fires(101);
    int f2 = count_fires(101);
    CHECK(f1 == f2);                                 // deterministic
    CHECK(f1 > total_steps * 0.4);                   // ~half fire
    CHECK(f1 < total_steps * 0.6);
}

TEST_CASE("lane STEP: fixed slew ignores the SMOOTH knob") {
    // Panel switch 3 middle = STEP + fixed slew: the glide time must be constant
    // regardless of SMOOTH. Sample the output a fixed offset past a step boundary
    // for two very different SMOOTH settings; with fixed slew they must match.
    auto glide_after_boundary = [](float smooth) {
        ModLane l;
        l.init(48000.f, 7);
        l.set_range(1.f); l.set_shape(0.5f);
        l.set_step(true, 2); l.set_probability(1.f);
        l.set_fixed_slew(true);        // engage fixed slew BEFORE SMOOTH
        l.set_smooth(smooth);          // must be ignored while fixed slew is on
        l.set_rate_hz(1.f);            // step 0 spans [0,24000); boundary at ~24000
        for (int i = 0; i < 24100; ++i) l.process();
        return l.process();            // ~100 samples past the step-1 boundary
    };
    CHECK(glide_after_boundary(0.0f) == doctest::Approx(glide_after_boundary(1.0f)).epsilon(0.001));
}
```

- [ ] **Step 2: Add to build and run — expect RED first, then GREEN**

Add `tests/test_step.cpp` to `spky_tests`, then:
```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```
Expected: the four STEP cases (fires-per-step, held-value, probability, fixed-slew) run and PASS against the Task 5 implementation. The fires window is deliberately shorter than one cycle (47000 samples) so the free-running float phasor does not re-enter step 0 and register a spurious 5th fire. If a case fails, debug `lane.cpp`'s STEP branch (step index from `_phase * _steps`; boundary on `step != _cur_step`; `_fixed_slew` uses a constant slew time in `_update_slew`) until green.

- [ ] **Step 3: Commit**

```bash
git add tests/test_step.cpp CMakeLists.txt engine/mod/lane.cpp
git commit -m "test(engine): pin ModLane STEP quantization, per-step dice, determinism"
```

---

## Task 7: `ModLane` — EVOLVE vs LOOP determinism

The EVOLVE random walk already exists in `lane.cpp` (Task 5); this task pins LOOP determinism and EVOLVE drift.

**Files:**
- Test: `tests/test_evolve.cpp`
- Modify: `CMakeLists.txt` (add `tests/test_evolve.cpp`)
- Possibly modify: `engine/mod/lane.cpp` (only if a test fails)

**Interfaces:**
- Consumes: `spky::ModLane` (Task 5). No new symbols.

- [ ] **Step 1: Write the failing tests**

`tests/test_evolve.cpp`:
```cpp
#include <doctest/doctest.h>
#include <vector>
#include <cmath>
#include "mod/lane.h"
using namespace spky;

static std::vector<float> run_lane(float evolve, uint32_t seed, int n) {
    ModLane l;
    l.init(48000.f, seed);
    l.set_range(1.f);
    l.set_shape(0.25f);       // triangle: shape-sensitive, S&H unused
    l.set_smooth(0.f);
    l.set_probability(1.f);
    l.set_evolve(evolve);
    l.set_rate_hz(1.f);       // ~48000 samples per cycle
    std::vector<float> out;
    out.reserve(n);
    for (int i = 0; i < n; ++i) out.push_back(l.process());
    return out;
}

// LOOP determinism is bit-exact reproducibility: same seed -> identical stream
// every run. (A cycle-vs-cycle comparison would drift ~2.6e-3 because a
// free-running float phasor does not close a cycle in exactly 48000 samples.)
TEST_CASE("lane LOOP: deterministic and reproducible (evolve = 0)") {
    auto a = run_lane(0.f, 2024, 96000);
    auto b = run_lane(0.f, 2024, 96000);
    bool identical = true;
    for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) identical = false;
    CHECK(identical);
}

TEST_CASE("lane EVOLVE: still deterministic, but wanders away from the loop") {
    auto loop = run_lane(0.f, 2024, 96000);
    auto ev1  = run_lane(1.f, 2024, 96000);
    auto ev2  = run_lane(1.f, 2024, 96000);

    bool ev_reproducible = true;
    for (size_t i = 0; i < ev1.size(); ++i) if (ev1[i] != ev2[i]) ev_reproducible = false;
    CHECK(ev_reproducible);                          // deterministic even while evolving

    float drift = 0.f;                               // but departs from the non-evolving loop
    for (size_t i = 48000; i < ev1.size(); ++i) drift += std::fabs(ev1[i] - loop[i]);
    CHECK(drift > 1.f);
}

// EVOLVE wanders shape, phase AND rate (spec). Pin the rate axis: under EVOLVE
// the samples between fires (the cycle length) must vary noticeably.
TEST_CASE("lane EVOLVE: cycle length wanders (rate wander)") {
    ModLane l;
    l.init(48000.f, 2024);
    l.set_range(1.f); l.set_shape(0.25f); l.set_smooth(0.f);
    l.set_probability(1.f); l.set_evolve(1.f); l.set_rate_hz(1.f);
    int last_fire = -1;
    std::vector<int> gaps;
    for (int i = 0; i < 48000 * 30; ++i) {
        l.process();
        if (l.fired()) { if (last_fire >= 0) gaps.push_back(i - last_fire); last_fire = i; }
    }
    REQUIRE(gaps.size() >= 5);
    int mn = gaps[0], mx = gaps[0];
    for (int g : gaps) { if (g < mn) mn = g; if (g > mx) mx = g; }
    CHECK(mx - mn > 100);                             // cycle length varies (rate wander)
}
```

- [ ] **Step 2: Add to build and run**

Add `tests/test_evolve.cpp` to `spky_tests`, then:
```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```
Expected: all three cases PASS. LOOP determinism is verified as bit-exact reproducibility (two lanes, same seed → identical streams), not fixed-offset cycle self-similarity — a free-running float phasor does not close a cycle in exactly 48000 samples, so `o[i]` vs `o[i+48000]` would drift ~2.6e-3. EVOLVE is asserted to stay reproducible, to diverge from the LOOP run, and to vary its cycle length (rate wander). If LOOP is not bit-identical, confirm `_compute_raw` reads only phase/shape (no per-sample rng) and that `_evolve == 0` leaves `_ev_phase`/`_ev_shape`/`_ev_rate` untouched.

- [ ] **Step 3: Commit**

```bash
git add tests/test_evolve.cpp CMakeLists.txt engine/mod/lane.cpp
git commit -m "test(engine): pin ModLane LOOP determinism vs EVOLVE drift"
```

---

## Task 8: `SuperModulator` — five lanes behind one macro surface

**Files:**
- Create: `engine/mod/super_modulator.h`
- Create: `engine/mod/super_modulator.cpp`
- Test: `tests/test_super_modulator.cpp`
- Modify: `CMakeLists.txt` (add `engine/mod/super_modulator.cpp` and `tests/test_super_modulator.cpp`)

**Interfaces:**
- Consumes: `spky::ModLane`, `LaneId`, `SyncMode`, `Rng`.
- Produces: `spky::SuperModulator` with `init(float sr, uint32_t seed_base)`; macro setters `set_tempo_bpm(float)`, `set_rate(float norm)`, `set_sync_mode(SyncMode)`, `set_shape/set_probability/set_smooth/set_range/set_evolve(float)`, `set_step(bool,int)`, `set_fixed_slew(bool)`; `process()` (advances all 5 lanes one sample); queries `lane_output(int)→float`, `lane_fired(int)→bool`, `lane_frozen(int)→bool`, `lane_phase(int)→float`, `pitch_phase()→float`, `master_hz()→float`. Fixed lane rate ratios `{2, 0.5, 1, 0.75, 1.5}` indexed by `LaneId`; the PITCH lane (`LANE_PITCH`) runs at ×1 (master). FREE rate maps `0.02..30 Hz` exponentially; SYNC/triplet derive from tempo BPM.

- [ ] **Step 1: Write the failing tests**

`tests/test_super_modulator.cpp`:
```cpp
#include <doctest/doctest.h>
#include <cmath>
#include "mod/super_modulator.h"
using namespace spky;

TEST_CASE("super: lane rate ratios (x2, x1/2, x1, x3/4, x3/2)") {
    SuperModulator m;
    m.init(48000.f, 42);
    m.set_sync_mode(SyncMode::Free);
    m.set_rate(0.3f);
    m.process();                         // one sample -> each lane phase == its inc
    float pitch = m.lane_phase(LANE_PITCH);
    CHECK(m.lane_phase(LANE_SOURCE) == doctest::Approx(pitch * 2.00f));
    CHECK(m.lane_phase(LANE_SIZE)   == doctest::Approx(pitch * 0.50f));
    CHECK(m.lane_phase(LANE_MOTION) == doctest::Approx(pitch * 0.75f));
    CHECK(m.lane_phase(LANE_LEVEL)  == doctest::Approx(pitch * 1.50f));
}

TEST_CASE("super: SYNC rate follows tempo") {
    SuperModulator m;
    m.init(48000.f, 42);
    m.set_tempo_bpm(120.f);              // 2 beats/sec
    m.set_sync_mode(SyncMode::Sync);
    m.set_rate(0.625f);                  // index 5 -> 1 cycle per beat
    CHECK(m.master_hz() == doctest::Approx(2.0f));
}

TEST_CASE("super: triplet mode is 1.5x the straight sync rate") {
    SuperModulator m;
    m.init(48000.f, 42);
    m.set_tempo_bpm(120.f);
    m.set_sync_mode(SyncMode::SyncTriplet);
    m.set_rate(0.625f);
    CHECK(m.master_hz() == doctest::Approx(3.0f));
}

TEST_CASE("super: lanes are decorrelated (independent random streams)") {
    SuperModulator m;
    m.init(48000.f, 42);
    m.set_sync_mode(SyncMode::Free);
    m.set_rate(0.5f);
    m.set_shape(1.f);                    // S&H exercises each lane's own rng
    m.set_probability(1.f);
    m.set_range(1.f);
    bool differ = false;
    for (int i = 0; i < 48000; ++i) {
        m.process();
        if (std::fabs(m.lane_output(LANE_PITCH) - m.lane_output(LANE_SOURCE)) > 0.05f)
            differ = true;
    }
    CHECK(differ);
}
```

- [ ] **Step 2: Add to build and run — expect RED**

Add both files to `spky_tests`, then `cmake -S . -B build && cmake --build build`.
Expected: build FAILS — `mod/super_modulator.h: No such file or directory`.

- [ ] **Step 3: Declare `SuperModulator`**

`engine/mod/super_modulator.h`:
```cpp
#pragma once
#include <array>
#include <cstdint>
#include "mod/lane.h"
#include "mod/lane_id.h"

namespace spky {

// One performable macro surface driving five independent lanes at fixed
// musical ratios of the master RATE. The PITCH lane leads (rate x1).
class SuperModulator {
public:
    void init(float sample_rate, uint32_t seed_base);

    void set_tempo_bpm(float bpm)  { _bpm = bpm; _update_rate(); }
    void set_rate(float norm)      { _rate_norm = norm; _update_rate(); }
    void set_sync_mode(SyncMode m) { _mode = m; _update_rate(); }
    void set_shape(float s);
    void set_probability(float p);
    void set_smooth(float s);
    void set_range(float r);
    void set_evolve(float a);
    void set_step(bool on, int steps);
    void set_fixed_slew(bool on);

    void process();                // advance all lanes one sample

    float lane_output(int i) const { return _out[i]; }
    bool  lane_fired(int i)  const { return _lanes[i].fired(); }
    bool  lane_frozen(int i) const { return _lanes[i].frozen(); }
    float lane_phase(int i)  const { return _lanes[i].phase(); }
    float pitch_phase()      const { return _lanes[LANE_PITCH].phase(); }
    float master_hz()        const { return _master_hz; }

private:
    void _update_rate();

    std::array<ModLane, LANE_COUNT> _lanes;
    std::array<float, LANE_COUNT>   _out {};

    float    _sr = 48000.f;
    float    _bpm = 120.f;
    float    _rate_norm = 0.5f;
    SyncMode _mode = SyncMode::Free;
    float    _master_hz = 1.f;
};

} // namespace spky
```

- [ ] **Step 4: Implement `SuperModulator`**

`engine/mod/super_modulator.cpp`:
```cpp
#include "mod/super_modulator.h"
#include "util/math.h"
#include <cmath>

using namespace spky;

namespace {
constexpr float kLaneRatio[LANE_COUNT] = { 2.f, 0.5f, 1.f, 0.75f, 1.5f };
constexpr float kRateFreeMin = 0.02f;
constexpr float kRateFreeMax = 30.f;

float free_hz(float norm) {
    return kRateFreeMin * std::pow(kRateFreeMax / kRateFreeMin, spky::clampf(norm, 0.f, 1.f));
}

float sync_hz(float norm, float bpm, bool triplet) {
    // cycles per beat: 8 bars ... 1/32 note
    static const float cpb[9] = { 1.f/32, 1.f/16, 1.f/8, 1.f/4, 1.f/2, 1.f, 2.f, 4.f, 8.f };
    int i = static_cast<int>(std::lround(spky::clampf(norm, 0.f, 1.f) * 8.f));
    float hz = (bpm / 60.f) * cpb[i];
    return triplet ? hz * 1.5f : hz;
}
} // namespace

void SuperModulator::init(float sample_rate, uint32_t seed_base) {
    _sr = sample_rate;
    for (int i = 0; i < LANE_COUNT; ++i) {
        _lanes[i].init(sample_rate, seed_base + static_cast<uint32_t>(i) * 2654435761u);
        _out[i] = 0.f;
    }
    _update_rate();
}

void SuperModulator::_update_rate() {
    switch (_mode) {
        case SyncMode::Free:        _master_hz = free_hz(_rate_norm); break;
        case SyncMode::Sync:        _master_hz = sync_hz(_rate_norm, _bpm, false); break;
        case SyncMode::SyncTriplet: _master_hz = sync_hz(_rate_norm, _bpm, true); break;
    }
    for (int i = 0; i < LANE_COUNT; ++i)
        _lanes[i].set_rate_hz(_master_hz * kLaneRatio[i]);
}

void SuperModulator::set_shape(float s)       { for (auto& l : _lanes) l.set_shape(s); }
void SuperModulator::set_probability(float p) { for (auto& l : _lanes) l.set_probability(p); }
void SuperModulator::set_smooth(float s)      { for (auto& l : _lanes) l.set_smooth(s); }
void SuperModulator::set_range(float r)       { for (auto& l : _lanes) l.set_range(r); }
void SuperModulator::set_evolve(float a)      { for (auto& l : _lanes) l.set_evolve(a); }
void SuperModulator::set_step(bool on, int n) { for (auto& l : _lanes) l.set_step(on, n); }
void SuperModulator::set_fixed_slew(bool on)  { for (auto& l : _lanes) l.set_fixed_slew(on); }

void SuperModulator::process() {
    for (int i = 0; i < LANE_COUNT; ++i)
        _out[i] = _lanes[i].process();
}
```

- [ ] **Step 5: Build and run — expect GREEN**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```
Expected: PASS (ratios, sync=2 Hz, triplet=3 Hz, decorrelation).

- [ ] **Step 6: Commit**

```bash
git add engine/mod/super_modulator.h engine/mod/super_modulator.cpp tests/test_super_modulator.cpp CMakeLists.txt
git commit -m "feat(engine): SuperModulator — 5 lanes, fixed ratios, free/sync rate"
```

---

## Task 9: `Part` — SuperModulator + engine interface + targets

**Files:**
- Create: `engine/parts/engine_iface.h`
- Create: `engine/parts/test_tone_engine.h`
- Create: `engine/parts/part.h`
- Create: `engine/parts/part.cpp`
- Test: `tests/test_part.cpp`
- Modify: `CMakeLists.txt` (add `engine/parts/part.cpp` and `tests/test_part.cpp`)

**Interfaces:**
- Consumes: `spky::SuperModulator`, `LaneId`, `SyncMode`, `clampf`, `TWO_PI`.
- Produces:
  - `spky::IPartEngine` (abstract): `init(float sr)`, `set_targets(const float* targets, float tune)`, `trigger(float pitch_norm)`, `process(float& l, float& r)`.
  - `spky::TestToneEngine : IPartEngine` — sine whose pitch follows `targets[LANE_PITCH]`+tune and amplitude follows `targets[LANE_LEVEL]`.
  - `spky::Part` with `init(float sr, uint32_t seed_base)`; `mod()→SuperModulator&`; `set_depth(float)`, `set_tune(float)`, `set_target_active(int,bool)`, `set_target_base(int,float)`, `set_target_depth(int,float)`; `target_value(int)→float` (`base + lane*depth*tdepth`, active-gated, clamped 0..1); `lane_output(int)→float`, `lane_fired(int)→bool`, `gate()→bool`, `pitch_cv()→float`; `process(float& l, float& r)` (advances mod one sample, drives engine, raises a ~5 ms gate on PITCH fire).

- [ ] **Step 1: Write the failing tests**

`tests/test_part.cpp`:
```cpp
#include <doctest/doctest.h>
#include "parts/part.h"
using namespace spky;

TEST_CASE("part: inactive target contributes only its base value") {
    Part p;
    p.init(48000.f, 5);
    p.set_target_active(LANE_SIZE, false);
    p.set_target_base(LANE_SIZE, 0.3f);
    p.mod().set_range(1.f);
    float l, r;
    for (int i = 0; i < 1000; ++i) p.process(l, r);
    CHECK(p.target_value(LANE_SIZE) == doctest::Approx(0.3f));
}

TEST_CASE("part: active target modulates around its base, clamped to [0,1]") {
    Part p;
    p.init(48000.f, 5);
    p.set_target_active(LANE_PITCH, true);
    p.set_target_base(LANE_PITCH, 0.5f);
    p.set_target_depth(LANE_PITCH, 1.f);
    p.set_depth(1.f);
    p.mod().set_range(1.f);
    p.mod().set_shape(0.5f);
    p.mod().set_sync_mode(SyncMode::Free);
    p.mod().set_rate(0.6f);
    float minv = 1.f, maxv = 0.f, l, r;
    for (int i = 0; i < 48000; ++i) {
        p.process(l, r);
        float t = p.target_value(LANE_PITCH);
        if (t < minv) minv = t;
        if (t > maxv) maxv = t;
    }
    CHECK(maxv > minv);
    CHECK(minv >= 0.f);
    CHECK(maxv <= 1.f);
}

TEST_CASE("part: DEPTH 0 pins targets to base") {
    Part p;
    p.init(48000.f, 5);
    p.set_target_active(LANE_PITCH, true);
    p.set_target_base(LANE_PITCH, 0.5f);
    p.set_depth(0.f);
    p.mod().set_range(1.f);
    float l, r;
    for (int i = 0; i < 5000; ++i) {
        p.process(l, r);
        CHECK(p.target_value(LANE_PITCH) == doctest::Approx(0.5f));
    }
}

TEST_CASE("part: a PITCH fire raises the gate") {
    Part p;
    p.init(48000.f, 5);
    p.set_target_active(LANE_PITCH, true);
    p.mod().set_sync_mode(SyncMode::Free);
    p.mod().set_rate(0.7f);
    p.mod().set_probability(1.f);
    bool saw_gate = false;
    float l, r;
    for (int i = 0; i < 48000; ++i) {
        p.process(l, r);
        if (p.gate()) saw_gate = true;
    }
    CHECK(saw_gate);
}
```

- [ ] **Step 2: Add to build and run — expect RED**

Add both files to `spky_tests`, then `cmake -S . -B build && cmake --build build`.
Expected: build FAILS — `parts/part.h: No such file or directory`.

- [ ] **Step 3: Create the engine interface and placeholder engine**

`engine/parts/engine_iface.h`:
```cpp
#pragma once
#include "mod/lane_id.h"

namespace spky {

// A part's sound engine. Consumes the 5 normalized target values; produces
// stereo audio. M1 ships TestToneEngine; SynthVoice (M2) and SamplerEngine
// (M5) implement the same interface behind the same Part.
class IPartEngine {
public:
    virtual ~IPartEngine() = default;
    virtual void init(float sample_rate) = 0;
    virtual void set_targets(const float* targets /*[LANE_COUNT]*/, float tune) = 0;
    virtual void trigger(float pitch_norm) = 0;
    virtual void process(float& outL, float& outR) = 0;
};

} // namespace spky
```

`engine/parts/test_tone_engine.h`:
```cpp
#pragma once
#include <cmath>
#include "parts/engine_iface.h"
#include "util/math.h"

namespace spky {

// Minimal audible engine for M1: a sine whose pitch follows the PITCH target
// (+ TUNE) and whose amplitude follows the LEVEL target. Replaced by the real
// engines in later milestones.
class TestToneEngine : public IPartEngine {
public:
    void init(float sample_rate) override { _sr = sample_rate; _phase = 0.f; }

    void set_targets(const float* t, float tune) override {
        float p = clampf(t[LANE_PITCH] * 0.7f + tune * 0.3f, 0.f, 1.f);
        _freq = 110.f * std::pow(8.f, p);   // ~110..880 Hz
        _amp  = t[LANE_LEVEL];
    }

    void trigger(float /*pitch_norm*/) override {}   // test tone is continuous

    void process(float& outL, float& outR) override {
        _phase += _freq / _sr;
        if (_phase >= 1.f) _phase -= 1.f;
        float s = std::sin(_phase * TWO_PI) * _amp * 0.3f;
        outL = s;
        outR = s;
    }

private:
    float _sr = 48000.f;
    float _phase = 0.f;
    float _freq = 220.f;
    float _amp = 0.5f;
};

} // namespace spky
```

- [ ] **Step 4: Declare `Part`**

`engine/parts/part.h`:
```cpp
#pragma once
#include <array>
#include <cstdint>
#include "mod/super_modulator.h"
#include "parts/engine_iface.h"
#include "parts/test_tone_engine.h"
#include "util/math.h"

namespace spky {

// A part = SuperModulator + selectable engine + 5 targets. Combines each lane's
// bipolar output with a stored per-target base + depth, gated by the target's
// active flag and the master DEPTH.
class Part {
public:
    void init(float sample_rate, uint32_t seed_base);

    SuperModulator& mod() { return _mod; }

    void set_depth(float d) { _depth = clampf(d, 0.f, 1.f); }
    void set_tune(float t)  { _tune = clampf(t, 0.f, 1.f); }
    void set_target_active(int slot, bool on) { _active[slot] = on; }
    void set_target_base(int slot, float b)   { _base[slot] = clampf(b, 0.f, 1.f); }
    void set_target_depth(int slot, float d)  { _tdepth[slot] = clampf(d, 0.f, 1.f); }

    float target_value(int slot) const;        // base + mod*depth, clamped 0..1
    float lane_output(int slot) const { return _mod.lane_output(slot); }
    bool  lane_fired(int slot) const  { return _mod.lane_fired(slot); }
    bool  gate() const { return _gate_ctr > 0; }
    float pitch_cv() const { return target_value(LANE_PITCH); }

    void process(float& outL, float& outR);    // advance mod one sample + engine

private:
    SuperModulator _mod;
    TestToneEngine _tone;
    IPartEngine*   _engine = nullptr;

    std::array<bool,  LANE_COUNT> _active { { false, false, true, false, true } };
    std::array<float, LANE_COUNT> _base   { { 0.5f, 0.5f, 0.5f, 0.5f, 0.8f } };
    std::array<float, LANE_COUNT> _tdepth { { 1.f, 1.f, 1.f, 1.f, 1.f } };

    float _depth = 1.f;
    float _tune = 0.5f;
    int   _gate_ctr = 0;
    int   _gate_len = 240;   // ~5 ms @ 48k, recomputed in init()
    float _sr = 48000.f;
};

} // namespace spky
```

- [ ] **Step 5: Implement `Part`**

`engine/parts/part.cpp`:
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
}

float Part::target_value(int slot) const {
    float mod = _active[slot] ? _mod.lane_output(slot) * _depth * _tdepth[slot] : 0.f;
    return clampf(_base[slot] + mod, 0.f, 1.f);
}

void Part::process(float& outL, float& outR) {
    _mod.process();

    if (_mod.lane_fired(LANE_PITCH)) _gate_ctr = _gate_len;
    if (_gate_ctr > 0) --_gate_ctr;

    float targets[LANE_COUNT];
    for (int i = 0; i < LANE_COUNT; ++i) targets[i] = target_value(i);

    _engine->set_targets(targets, _tune);
    if (_mod.lane_fired(LANE_PITCH)) _engine->trigger(targets[LANE_PITCH]);
    _engine->process(outL, outR);
}
```

- [ ] **Step 6: Build and run — expect GREEN**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add engine/parts/engine_iface.h engine/parts/test_tone_engine.h engine/parts/part.h engine/parts/part.cpp tests/test_part.cpp CMakeLists.txt
git commit -m "feat(engine): Part — engine interface, test-tone engine, target routing, gate"
```

---

## Task 10: `Instrument` — the single public API

**Files:**
- Create: `engine/instrument.h`
- Create: `engine/instrument.cpp`
- Test: `tests/test_instrument.cpp`
- Modify: `CMakeLists.txt` (add `engine/instrument.cpp` and `tests/test_instrument.cpp`)

**Interfaces:**
- Consumes: `spky::Part`, `LaneId`, `SyncMode`.
- Produces: `spky::PartId {PART_A=0, PART_B=1, PART_COUNT=2}`; `spky::Instrument` with `init(float sr)`, `set_tempo_bpm(float)`, per-part normalized setters (`set_rate`, `set_sync_mode`, `set_shape`, `set_probability`, `set_smooth`, `set_range`, `set_evolve`, `set_step(int,bool,int)`, `set_fixed_slew(int,bool)`, `set_depth`, `set_tune`, `set_target_active(int,int,bool)`, `set_target_base(int,int,float)`, `set_target_depth(int,int,float)`), introspection (`lane_output(int,int)`, `target_value(int,int)`, `lane_fired(int,int)`, `gate(int)`, `pitch_cv(int)`), and `process(const float* inL, const float* inR, float* outL, float* outR, size_t n)`.

- [ ] **Step 1: Write the failing tests**

`tests/test_instrument.cpp`:
```cpp
#include <doctest/doctest.h>
#include <vector>
#include <cmath>
#include "instrument.h"
using namespace spky;

TEST_CASE("instrument: init and render a block without NaNs") {
    Instrument inst;
    inst.init(48000.f);
    inst.set_tempo_bpm(120.f);
    inst.set_target_active(PART_A, LANE_PITCH, true);
    inst.set_target_active(PART_A, LANE_LEVEL, true);
    inst.set_rate(PART_A, 0.5f);
    inst.set_range(PART_A, 1.f);

    std::vector<float> l(96), r(96);
    inst.process(nullptr, nullptr, l.data(), r.data(), 96);
    for (int i = 0; i < 96; ++i) {
        CHECK(l[i] == l[i]);            // not NaN
        CHECK(l[i] >= -1.5f);
        CHECK(l[i] <=  1.5f);
    }
}

TEST_CASE("instrument: the two parts are decorrelated") {
    Instrument inst;
    inst.init(48000.f);
    inst.set_sync_mode(PART_A, SyncMode::Free);
    inst.set_sync_mode(PART_B, SyncMode::Free);
    inst.set_rate(PART_A, 0.5f);
    inst.set_rate(PART_B, 0.5f);
    inst.set_shape(PART_A, 1.f);
    inst.set_shape(PART_B, 1.f);
    inst.set_range(PART_A, 1.f);
    inst.set_range(PART_B, 1.f);

    std::vector<float> l(1), r(1);
    bool differ = false;
    for (int i = 0; i < 48000; ++i) {
        inst.process(nullptr, nullptr, l.data(), r.data(), 1);
        if (std::fabs(inst.lane_output(PART_A, LANE_PITCH)
                    - inst.lane_output(PART_B, LANE_PITCH)) > 0.05f) differ = true;
    }
    CHECK(differ);
}
```

- [ ] **Step 2: Add to build and run — expect RED**

Add both files to `spky_tests`, then `cmake -S . -B build && cmake --build build`.
Expected: build FAILS — `instrument.h: No such file or directory`.

- [ ] **Step 3: Declare `Instrument`**

`engine/instrument.h`:
```cpp
#pragma once
#include <array>
#include <cstddef>
#include "parts/part.h"
#include "mod/lane_id.h"

namespace spky {

enum PartId { PART_A = 0, PART_B = 1, PART_COUNT = 2 };

// The complete public API. No hardware type crosses this boundary; the same
// object is driven by the desktop render host and (later) the firmware shell.
class Instrument {
public:
    void init(float sample_rate);
    void set_tempo_bpm(float bpm);

    void set_rate(int p, float n)            { _parts[p].mod().set_rate(n); }
    void set_sync_mode(int p, SyncMode m)    { _parts[p].mod().set_sync_mode(m); }
    void set_shape(int p, float n)           { _parts[p].mod().set_shape(n); }
    void set_probability(int p, float n)     { _parts[p].mod().set_probability(n); }
    void set_smooth(int p, float n)          { _parts[p].mod().set_smooth(n); }
    void set_range(int p, float n)           { _parts[p].mod().set_range(n); }
    void set_evolve(int p, float n)          { _parts[p].mod().set_evolve(n); }
    void set_step(int p, bool on, int steps) { _parts[p].mod().set_step(on, steps); }
    void set_fixed_slew(int p, bool on)      { _parts[p].mod().set_fixed_slew(on); }
    void set_depth(int p, float n)           { _parts[p].set_depth(n); }
    void set_tune(int p, float n)            { _parts[p].set_tune(n); }
    void set_target_active(int p, int s, bool on) { _parts[p].set_target_active(s, on); }
    void set_target_base(int p, int s, float n)   { _parts[p].set_target_base(s, n); }
    void set_target_depth(int p, int s, float n)  { _parts[p].set_target_depth(s, n); }

    float lane_output(int p, int s)  const { return _parts[p].lane_output(s); }
    float target_value(int p, int s) const { return _parts[p].target_value(s); }
    bool  lane_fired(int p, int s)   const { return _parts[p].lane_fired(s); }
    bool  gate(int p)  const { return _parts[p].gate(); }
    float pitch_cv(int p) const { return _parts[p].pitch_cv(); }

    void process(const float* inL, const float* inR, float* outL, float* outR, size_t n);

private:
    std::array<Part, PART_COUNT> _parts;
    float _sr = 48000.f;
    float _bpm = 120.f;
};

} // namespace spky
```

- [ ] **Step 4: Implement `Instrument`**

`engine/instrument.cpp`:
```cpp
#include "instrument.h"

using namespace spky;

void Instrument::init(float sample_rate) {
    _sr = sample_rate;
    _parts[PART_A].init(sample_rate, 0x1234abcdu);
    _parts[PART_B].init(sample_rate, 0x9e3779b9u);
    set_tempo_bpm(_bpm);
}

void Instrument::set_tempo_bpm(float bpm) {
    _bpm = bpm;
    for (auto& p : _parts) p.mod().set_tempo_bpm(bpm);
}

void Instrument::process(const float* /*inL*/, const float* /*inR*/,
                         float* outL, float* outR, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        float al = 0.f, ar = 0.f, bl = 0.f, br = 0.f;
        _parts[PART_A].process(al, ar);
        _parts[PART_B].process(bl, br);
        outL[i] = (al + bl) * 0.5f;   // MORPH/center mixing arrives in M4
        outR[i] = (ar + br) * 0.5f;
    }
}
```

- [ ] **Step 5: Build and run — expect GREEN**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add engine/instrument.h engine/instrument.cpp tests/test_instrument.cpp CMakeLists.txt
git commit -m "feat(engine): Instrument public API over two parts"
```

---

## Task 11: WAV writer (host)

**Files:**
- Create: `host/render/wav_writer.h`
- Test: `tests/test_wav.cpp`
- Modify: `CMakeLists.txt` (add `tests/test_wav.cpp` to `spky_tests`; add `host` to `spky_tests` include dirs)

**Interfaces:**
- Produces: `spky::WavWriter` (header-only, host side — `std::vector` allowed here) with ctor `WavWriter(int sample_rate)`, `push(float l, float r)`, `write(const std::string& path)→bool`. Writes 16-bit PCM stereo, little-endian (desktop-only host tool).

- [ ] **Step 1: Write the failing test**

`tests/test_wav.cpp`:
```cpp
#include <doctest/doctest.h>
#include <cstdio>
#include <cstdint>
#include "render/wav_writer.h"
using namespace spky;

TEST_CASE("wav: writes a valid RIFF/WAVE header + PCM data") {
    WavWriter w(48000);
    for (int i = 0; i < 10; ++i) w.push(0.5f, -0.5f);
    const char* path = "test_out.wav";
    REQUIRE(w.write(path));

    FILE* f = std::fopen(path, "rb");
    REQUIRE(f != nullptr);
    char riff[4]; std::fread(riff, 1, 4, f);
    CHECK(riff[0] == 'R'); CHECK(riff[1] == 'I');
    CHECK(riff[2] == 'F'); CHECK(riff[3] == 'F');
    uint32_t chunk = 0; std::fread(&chunk, 4, 1, f);
    CHECK(chunk == 36u + 10u * 2u * sizeof(int16_t));
    char wave[4]; std::fread(wave, 1, 4, f);
    CHECK(wave[0] == 'W'); CHECK(wave[3] == 'E');
    std::fclose(f);
    std::remove(path);
}
```

- [ ] **Step 2: Add to build and run — expect RED**

In `CMakeLists.txt`, add `tests/test_wav.cpp` to `spky_tests`, and give the tests target the host include dir (add this line after the `spky_tests` `target_link_libraries`):
```cmake
target_include_directories(spky_tests PRIVATE host)
```
Run `cmake -S . -B build && cmake --build build`.
Expected: build FAILS — `render/wav_writer.h: No such file or directory`.

- [ ] **Step 3: Implement `WavWriter`**

`host/render/wav_writer.h`:
```cpp
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace spky {

// Minimal 16-bit PCM stereo WAV writer (little-endian; desktop host only).
class WavWriter {
public:
    explicit WavWriter(int sample_rate) : _sr(sample_rate) {}

    void push(float l, float r) {
        _samples.push_back(to_i16(l));
        _samples.push_back(to_i16(r));
    }

    bool write(const std::string& path) const {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        uint32_t data_bytes = static_cast<uint32_t>(_samples.size() * sizeof(int16_t));
        uint32_t byte_rate = static_cast<uint32_t>(_sr) * 2u * 2u; // stereo * 2 bytes
        put_tag(f, "RIFF");
        put_u32(f, 36u + data_bytes);
        put_tag(f, "WAVE");
        put_tag(f, "fmt ");
        put_u32(f, 16u);
        put_u16(f, 1u);                              // PCM
        put_u16(f, 2u);                              // channels
        put_u32(f, static_cast<uint32_t>(_sr));
        put_u32(f, byte_rate);
        put_u16(f, 4u);                              // block align
        put_u16(f, 16u);                             // bits per sample
        put_tag(f, "data");
        put_u32(f, data_bytes);
        std::fwrite(_samples.data(), sizeof(int16_t), _samples.size(), f);
        std::fclose(f);
        return true;
    }

private:
    static int16_t to_i16(float v) {
        if (v >  1.f) v =  1.f;
        if (v < -1.f) v = -1.f;
        return static_cast<int16_t>(v * 32767.f);
    }
    static void put_u32(FILE* f, uint32_t v) { std::fwrite(&v, 4, 1, f); }
    static void put_u16(FILE* f, uint16_t v) { std::fwrite(&v, 2, 1, f); }
    static void put_tag(FILE* f, const char* s) { std::fwrite(s, 1, 4, f); }

    int _sr;
    std::vector<int16_t> _samples;
};

} // namespace spky
```

- [ ] **Step 4: Build and run — expect GREEN**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add host/render/wav_writer.h tests/test_wav.cpp CMakeLists.txt
git commit -m "feat(host): 16-bit PCM stereo WAV writer"
```

---

## Task 12: Scenario JSON parser (host)

**Files:**
- Create: `host/render/scenario.h`
- Create: `host/render/scenario.cpp`
- Test: `tests/test_scenario.cpp`
- Modify: `CMakeLists.txt` (add vendored nlohmann/json interface lib; add `host/render/scenario.cpp` and `tests/test_scenario.cpp` to `spky_tests`; link json)
- Create: `third_party/nlohmann/json.hpp` (vendored single header)

**Interfaces:**
- Consumes: `spky::Instrument`, `SyncMode`, `LaneId`.
- Produces: `spky::Event {double time_s; std::string action; int part; int slot; float value; bool flag; int ivalue; std::string svalue;}`; `spky::Scenario {int sample_rate; float bpm; double duration_s; std::vector<Event> init_events; std::vector<Event> events;}`; `bool load_scenario(const std::string& path, Scenario& out, std::string& err)` (events returned sorted by `time_s`); `void apply_event(Instrument&, const Event&)`.

- [ ] **Step 1: Write the failing test**

`tests/test_scenario.cpp`:
```cpp
#include <doctest/doctest.h>
#include <cstdio>
#include <fstream>
#include "render/scenario.h"
using namespace spky;

TEST_CASE("scenario: parses init + timeline and sorts events by time") {
    const char* path = "test_scenario.json";
    {
        std::ofstream o(path);
        o << R"({
          "sample_rate": 48000,
          "bpm": 100,
          "duration_s": 5,
          "init": [
            {"action":"set_sync_mode","part":0,"value":"free"},
            {"action":"set_rate","part":0,"value":0.5}
          ],
          "events": [
            {"t":3.0,"action":"set_probability","part":0,"value":0.2},
            {"t":1.0,"action":"set_step","part":0,"flag":true,"ivalue":16}
          ]
        })";
    }
    Scenario s;
    std::string err;
    REQUIRE(load_scenario(path, s, err));
    CHECK(s.sample_rate == 48000);
    CHECK(s.bpm == doctest::Approx(100.f));
    CHECK(s.duration_s == doctest::Approx(5.0));
    CHECK(s.init_events.size() == 2);
    REQUIRE(s.events.size() == 2);
    CHECK(s.events[0].time_s == doctest::Approx(1.0));   // sorted ascending
    CHECK(s.events[0].action == "set_step");
    CHECK(s.events[0].ivalue == 16);
    CHECK(s.events[1].action == "set_probability");

    Instrument inst;
    inst.init(48000.f);
    for (const auto& e : s.init_events) apply_event(inst, e);   // must not crash
    for (const auto& e : s.events)      apply_event(inst, e);
    std::remove(path);
}
```

- [ ] **Step 2: Vendor json, wire it into the build, add sources — expect RED**

Download the nlohmann/json single header:
```bash
mkdir -p third_party/nlohmann
curl -fsSL -o third_party/nlohmann/json.hpp \
  https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp
```
In `CMakeLists.txt`, add a json interface lib next to the `doctest` one:
```cmake
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE third_party)
```
Add `host/render/scenario.cpp` and `tests/test_scenario.cpp` to `spky_tests`, and link json to the tests target:
```cmake
target_link_libraries(spky_tests PRIVATE spky_engine doctest nlohmann_json)
```
Run `source env.sh && cmake -S . -B build && cmake --build build`.
Expected: build FAILS — `render/scenario.h: No such file or directory`.

- [ ] **Step 3: Declare the scenario types**

`host/render/scenario.h`:
```cpp
#pragma once
#include <string>
#include <vector>
#include "instrument.h"

namespace spky {

struct Event {
    double      time_s = 0.0;
    std::string action;
    int         part = 0;
    int         slot = 0;
    float       value = 0.f;
    bool        flag = false;
    int         ivalue = 0;
    std::string svalue;   // string-valued args (e.g. sync mode)
};

struct Scenario {
    int    sample_rate = 48000;
    float  bpm = 120.f;
    double duration_s = 10.0;
    std::vector<Event> init_events;   // applied at t = 0
    std::vector<Event> events;        // timeline, sorted by time_s
};

// Parse a scenario JSON file. Returns false (and sets err) on read/parse error.
bool load_scenario(const std::string& path, Scenario& out, std::string& err);

// Apply one event to the instrument.
void apply_event(Instrument& inst, const Event& e);

} // namespace spky
```

- [ ] **Step 4: Implement the parser and dispatcher**

`host/render/scenario.cpp`:
```cpp
#include "render/scenario.h"
#include <algorithm>
#include <fstream>
#include <exception>
#include "nlohmann/json.hpp"

using namespace spky;
using json = nlohmann::json;

static Event parse_event(const json& j, bool timed) {
    Event e;
    if (timed) e.time_s = j.value("t", 0.0);
    e.action = j.value("action", std::string());
    e.part   = j.value("part", 0);
    e.slot   = j.value("slot", 0);
    e.flag   = j.value("flag", false);
    e.ivalue = j.value("ivalue", 0);
    if (j.contains("value")) {
        if (j["value"].is_string()) e.svalue = j["value"].get<std::string>();
        else                        e.value  = j["value"].get<float>();
    }
    return e;
}

bool spky::load_scenario(const std::string& path, Scenario& out, std::string& err) {
    std::ifstream in(path);
    if (!in) { err = "cannot open " + path; return false; }
    json j;
    try { in >> j; }
    catch (const std::exception& ex) { err = ex.what(); return false; }

    out.sample_rate = j.value("sample_rate", 48000);
    out.bpm         = j.value("bpm", 120.f);
    out.duration_s  = j.value("duration_s", 10.0);

    if (j.contains("init"))
        for (const auto& e : j["init"]) out.init_events.push_back(parse_event(e, false));
    if (j.contains("events"))
        for (const auto& e : j["events"]) out.events.push_back(parse_event(e, true));

    std::stable_sort(out.events.begin(), out.events.end(),
                     [](const Event& a, const Event& b) { return a.time_s < b.time_s; });
    return true;
}

static SyncMode parse_sync(const std::string& s) {
    if (s == "sync")    return SyncMode::Sync;
    if (s == "triplet") return SyncMode::SyncTriplet;
    return SyncMode::Free;
}

void spky::apply_event(Instrument& inst, const Event& e) {
    const std::string& a = e.action;
    if      (a == "set_tempo_bpm")     inst.set_tempo_bpm(e.value);
    else if (a == "set_rate")          inst.set_rate(e.part, e.value);
    else if (a == "set_sync_mode")     inst.set_sync_mode(e.part, parse_sync(e.svalue));
    else if (a == "set_shape")         inst.set_shape(e.part, e.value);
    else if (a == "set_probability")   inst.set_probability(e.part, e.value);
    else if (a == "set_smooth")        inst.set_smooth(e.part, e.value);
    else if (a == "set_range")         inst.set_range(e.part, e.value);
    else if (a == "set_evolve")        inst.set_evolve(e.part, e.value);
    else if (a == "set_depth")         inst.set_depth(e.part, e.value);
    else if (a == "set_tune")          inst.set_tune(e.part, e.value);
    else if (a == "set_step")          inst.set_step(e.part, e.flag, e.ivalue);
    else if (a == "set_fixed_slew")    inst.set_fixed_slew(e.part, e.flag);
    else if (a == "set_target_active") inst.set_target_active(e.part, e.slot, e.flag);
    else if (a == "set_target_base")   inst.set_target_base(e.part, e.slot, e.value);
    else if (a == "set_target_depth")  inst.set_target_depth(e.part, e.slot, e.value);
    // unknown actions are ignored on purpose (forward-compatible scenarios)
}
```

- [ ] **Step 5: Build and run — expect GREEN**

```bash
source env.sh && cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add host/render/scenario.h host/render/scenario.cpp tests/test_scenario.cpp \
        third_party/nlohmann/json.hpp CMakeLists.txt
git commit -m "feat(host): scenario JSON parser + event dispatcher (vendored nlohmann/json)"
```

---

## Task 13: Render host CLI + demo scenario

**Files:**
- Create: `host/render/main.cpp`
- Create: `host/render/scenarios/demo_step_melody.json`
- Modify: `CMakeLists.txt` (add the `render` executable target)

**Interfaces:**
- Consumes: `spky::Instrument`, `load_scenario`, `apply_event`, `WavWriter`, `LaneId`, `PartId`.
- Produces: the `render` executable — `render <scenario.json> [out.wav] [mods.csv]`. Processes sample-by-sample; writes stereo WAV and a CSV of both parts' lane outputs, pitch CV and gate (decimated to one row per 64 samples).

- [ ] **Step 1: Implement the render CLI**

`host/render/main.cpp`:
```cpp
#include <cstdio>
#include <cstddef>
#include <string>
#include "instrument.h"
#include "render/scenario.h"
#include "render/wav_writer.h"

using namespace spky;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: render <scenario.json> [out.wav] [mods.csv]\n");
        return 1;
    }
    std::string scen_path = argv[1];
    std::string wav_path  = argc > 2 ? argv[2] : "out.wav";
    std::string csv_path  = argc > 3 ? argv[3] : "mods.csv";

    Scenario scen;
    std::string err;
    if (!load_scenario(scen_path, scen, err)) {
        std::printf("scenario error: %s\n", err.c_str());
        return 2;
    }

    Instrument inst;
    inst.init(static_cast<float>(scen.sample_rate));
    inst.set_tempo_bpm(scen.bpm);
    for (const auto& e : scen.init_events) apply_event(inst, e);

    WavWriter wav(scen.sample_rate);
    FILE* csv = std::fopen(csv_path.c_str(), "wb");
    if (csv) {
        std::fprintf(csv, "t,"
            "a_src,a_size,a_pitch,a_motion,a_level,a_pcv,a_gate,"
            "b_src,b_size,b_pitch,b_motion,b_level,b_pcv,b_gate\n");
    }

    const size_t total = static_cast<size_t>(scen.duration_s * scen.sample_rate);
    const int    csv_decim = 64;
    size_t next_event = 0;

    for (size_t i = 0; i < total; ++i) {
        double t = static_cast<double>(i) / scen.sample_rate;
        while (next_event < scen.events.size() && scen.events[next_event].time_s <= t) {
            apply_event(inst, scen.events[next_event]);
            ++next_event;
        }

        float l = 0.f, r = 0.f;
        inst.process(nullptr, nullptr, &l, &r, 1);
        wav.push(l, r);

        if (csv && (i % csv_decim == 0)) {
            std::fprintf(csv, "%.5f", t);
            for (int p = 0; p < 2; ++p) {
                for (int s = 0; s < LANE_COUNT; ++s)
                    std::fprintf(csv, ",%.4f", inst.lane_output(p, s));
                std::fprintf(csv, ",%.4f,%d", inst.pitch_cv(p), inst.gate(p) ? 1 : 0);
            }
            std::fprintf(csv, "\n");
        }
    }

    if (csv) std::fclose(csv);
    if (!wav.write(wav_path)) {
        std::printf("failed to write %s\n", wav_path.c_str());
        return 3;
    }
    std::printf("wrote %s (%zu frames) and %s\n", wav_path.c_str(), total, csv_path.c_str());
    return 0;
}
```

- [ ] **Step 2: Create the demo scenario**

`host/render/scenarios/demo_step_melody.json` (slot 2 = PITCH, slot 4 = LEVEL):
```json
{
  "sample_rate": 48000,
  "bpm": 110,
  "duration_s": 16,
  "init": [
    {"action":"set_sync_mode","part":0,"value":"sync"},
    {"action":"set_rate","part":0,"value":0.5},
    {"action":"set_step","part":0,"flag":true,"ivalue":16},
    {"action":"set_shape","part":0,"value":0.55},
    {"action":"set_range","part":0,"value":0.8},
    {"action":"set_smooth","part":0,"value":0.1},
    {"action":"set_probability","part":0,"value":1.0},
    {"action":"set_depth","part":0,"value":1.0},
    {"action":"set_target_active","part":0,"slot":2,"flag":true},
    {"action":"set_target_active","part":0,"slot":4,"flag":true},
    {"action":"set_target_base","part":0,"slot":2,"value":0.4},
    {"action":"set_target_base","part":0,"slot":4,"value":0.8}
  ],
  "events": [
    {"t":4.0,"action":"set_probability","part":0,"value":0.6},
    {"t":8.0,"action":"set_shape","part":0,"value":0.9},
    {"t":12.0,"action":"set_evolve","part":0,"value":0.5}
  ]
}
```

- [ ] **Step 3: Add the `render` target to `CMakeLists.txt`**

```cmake
add_executable(render
    host/render/main.cpp
    host/render/scenario.cpp
    engine/mod/lane.cpp
    engine/mod/super_modulator.cpp
    engine/parts/part.cpp
    engine/instrument.cpp
)
target_include_directories(render PRIVATE host engine)
target_link_libraries(render PRIVATE nlohmann_json)
```

- [ ] **Step 4: Build**

Run:
```bash
source env.sh && cmake -S . -B build && cmake --build build
```
Expected: builds `render` and `spky_tests` with no errors.

- [ ] **Step 5: Render the demo and verify output exists**

Run (Ninja single-config generator → binary directly under `build/`):
```bash
./build/render.exe host/render/scenarios/demo_step_melody.json out.wav mods.csv
```
Expected stdout: `wrote out.wav (768000 frames) and mods.csv`.

- [ ] **Step 6: Verify the modulation is evolving and quantized**

Inspect the CSV (first and later rows). Confirm, in the `a_pitch` column, that values change in discrete **steps** (held flat for a run of rows, then jump) rather than continuously, and that `a_gate` shows `1` on some rows — proof the PITCH lane is firing triggers. After `t=8` the `a_pitch` steps become more extreme (shape → S&H), and after `t=12` consecutive cycles differ (EVOLVE).

Run:
```bash
head -5 mods.csv
```
Expected: a header row plus data rows; `a_pitch` non-constant across the file, `a_gate` contains at least one `1`. (Open `out.wav` in any player to hear the stepped melodic test-tone.)

- [ ] **Step 7: Confirm the original firmware is untouched**

Run:
```bash
git status --short
```
Expected: only new files under `engine/`, `host/`, `tests/`, `third_party/`, plus `CMakeLists.txt` and the `.gitignore` edit (`env.sh`, `build/`, `out.wav`, `mods.csv` are gitignored). Nothing under `src/`, and no change to `Makefile`, `main.cpp`, `app.cpp`, `app.h` — the original firmware remains buildable.

- [ ] **Step 8: Commit**

```bash
git add host/render/main.cpp host/render/scenarios/demo_step_melody.json CMakeLists.txt
git commit -m "feat(host): render CLI + demo step-melody scenario (WAV + mods.csv)"
```

---

## Self-Review (completed against the spec)

**1. Spec coverage.** Mapped each M1-relevant spec section to a task:

| Spec section | Task(s) |
|---|---|
| SuperModulator: 5 lanes behind one macro surface | 5–8 |
| Fixed lane rate ratios (×2, ×½, ×1, ×¾, ×1½); PITCH master | 8 |
| RATE (SYNC / SYNC-triplet / FREE) | 8 |
| SHAPE continuous morph (sine→…→S&H) | 3, 5 |
| LOOP / EVOLVE | 7 |
| PROBABILITY (cycle in FLOW, step in STEP; freeze on miss) | 5, 6 |
| STEP / FLOW + fixed slew | 5, 6 |
| SMOOTH slew | 5 |
| RANGE (off → unipolar → bipolar, monotonic) | 4 |
| DEPTH (master × per-target) | 9 |
| Fixed target slots, engine interface | 9 |
| PITCH lane triggers → gate | 9 |
| Instrument public API (normalized setters, process) | 10 |
| Deterministic PRNG (capture-ready, testable stats) | 2 |
| No heap / no libDaisy in `engine/`; OnePole smoothing | Global Constraints, 1, 5 |
| Desktop render host: scenario JSON → WAV + CSV | 11–13 |
| Original firmware stays buildable | 13 (Step 7) |
| Tests: rate accuracy, step quantization, probability stats, slew, decorrelation, LOOP determinism | 5–8 |

Deferred to later plans (explicitly out of M1 scope, noted up front): polyphonic synth voice (M2 — the "overlapping voices" half of the acceptance demo), capture sequencer (M3), center/COUPLE/DRIFT/MORPH/SPOT (M4), sampler adapter (M5), firmware shell + hardware pad/gesture state machine + panel switches + LED feedback (M6). Voice-allocation and couple-symmetry tests from the spec's Testing list belong to M2/M4.

**2. Placeholder scan.** No "TBD"/"handle edge cases"/"write tests for the above"/"similar to Task N". Every code step contains complete, compilable code; every test step contains real assertions; every run step gives the exact command and expected result.

**3. Type consistency.** Verified names/signatures thread through: `ModLane` setters and `fired()/frozen()/phase()/target()` (Tasks 5–7) are consumed unchanged by `SuperModulator` (8); `SuperModulator::mod()` surface is consumed by `Part` (9) and `Instrument` (10); `LaneId` slot indices (`LANE_PITCH=2`, `LANE_LEVEL=4`) match the demo scenario's `slot:2`/`slot:4` (13); `IPartEngine::set_targets(const float*, float)` matches `TestToneEngine` and `Part::process` (9); `Instrument::process(const float*, const float*, float*, float*, size_t)` matches the render loop and tests (10, 13). Rate maps are exact for the pinned tests (SYNC norm 0.625 → index 5 → 2 Hz @ 120 BPM; triplet → 3 Hz).

**4. Numeric robustness (hardened after an adversarial review pass).** A free-running float phasor never closes a cycle in exactly N samples, so timing tests assert fire *rates* over multi-second windows (±1) or count within a window shorter than one cycle — never exact closure at a window boundary (this caught the original `fires == 2` / `fires == 4` / `max_diff < 1e-4` over-specifications). LOOP determinism is verified as bit-exact reproducibility (same seed → identical stream), not fixed-offset cycle self-similarity. The SMOOTH-glide test uses a slew time that settles within one step (smooth 0.5, τ ≈ 3 ms). Per the spec, EVOLVE wanders shape, phase **and rate**; the rate axis (`_ev_rate`, added in Task 5's `lane.cpp`) is pinned by a cycle-length-variance test in Task 7. STEP + fixed-slew has an explicit SMOOTH-independence test in Task 6. The SYNC master-rate and lane-ratio tests are pure computations (no phase accumulation) and remain exact-equality.

---

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-07-10-spotykach-modulation-first-engine-foundation.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration.

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints.

**Which approach?**

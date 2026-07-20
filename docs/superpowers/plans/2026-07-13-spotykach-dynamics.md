# M4.6 Dynamics Implementation Plan — one-knob comp per part + master limiter

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A one-knob compressor per part (turn up = more compression + auto-makeup: quiet gets LOUDER) at the end of `PartFx` before the reverb send tap, plus a stereo-linked, bit-transparent master limiter with MASTER DRIVE at the `Instrument` mix stage.

**Architecture:** Two new engine modules in the Grit/Flux style — `Comp` (per-sample stereo-linked peak detector + ×16-decimated dB-domain gain computer, one macro knob mapped to threshold/ratio/release/auto-makeup) owned by `PartFx`, and `Limiter` (stmlib gain-riding recipe, stereo-linked, exact identity below a −1 dBFS knee) owned by `Instrument`. Host gains two scenario actions and one full-wet showcase scenario.

**Tech Stack:** C++17, doctest, CMake + clang + Ninja (desktop host), no libDaisy in `engine/`.

**Spec:** `docs/superpowers/specs/2026-07-13-spotykach-dynamics-design.md` (residency repo — read it first).

## Global Constraints

- Work in the fork at `C:/Users/bernd/Documents/AI/Spotykach` (repo `spotymod`), branch `main`. The plan + spec live in the residency repo `C:/Users/bernd/Documents/AI/Synthux Design Residency`.
- Build/test cycle (Git Bash):
  ```bash
  cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh
  cmake -B build -S .            # only needed once / after CMake edits
  cmake --build build && ./build/spky_tests.exe
  ```
  Expected on success: doctest prints `Status: SUCCESS!` (181 test cases before this plan; each task adds more).
- **Engine purity:** no heap, no allocation in the audio path, no libDaisy includes in `engine/`, deterministic (no `<random>`, no wall clock). `std::log10`/`std::pow`/`std::exp` are allowed ONLY at decimated/control rate, never per-sample (M2 precedent).
- **MIT-clean:** do NOT link or include anything from `DaisySP-LGPL/`. The vendored `daisysp::Limiter` is deliberately NOT used (it applies `SoftLimit(x*0.7)` unconditionally — never bit-transparent, ~−3 dB coloring, mono per instance).
- `src/`, `main.cpp`, `app.cpp/h`, `Makefile` (firmware) stay untouched.
- Commit style: `feat(m4.6): …` / `docs: …`, every commit ends with
  `Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>`
  (git `user.email` in the fork is already set correctly — do not change it).
- Boot defaults: comp amount 0 (bit-exact bypass), master drive 0 (pure safety). Existing scenarios must render bit-identically EXCEPT where peaks exceed −1 dBFS (see Task 5's re-pin note).
- Design intent, binding for every tuning decision: **the comp knob is a loudness knob first — when in doubt, err toward louder** (spec "Motivation").

---

### Task 1: `Comp` — one-knob compressor core

**Files:**
- Create: `engine/fx/comp.h`
- Create: `engine/fx/comp.cpp`
- Create: `tests/test_comp.cpp`
- Modify: `CMakeLists.txt` (add `engine/fx/comp.cpp` + `tests/test_comp.cpp` to `spky_tests`; add `engine/fx/comp.cpp` to `render`)

**Interfaces:**
- Consumes: `spky::OnePole` (`engine/util/onepole.h`).
- Produces (Task 3 relies on these exact signatures):
  `void Comp::init(float sample_rate)`, `void Comp::set_amount(float n)` (0..1, clamped), `float Comp::amount() const`, `bool Comp::engaged() const`, `void Comp::process(float& l, float& r)`, `float Comp::gain_db() const` (test/meter aid).

- [ ] **Step 1: Write the failing tests**

Create `tests/test_comp.cpp`:

```cpp
#include <doctest/doctest.h>
#include <cmath>
#include <vector>
#include "fx/comp.h"
using namespace spky;

static std::vector<float> sine(int n, float amp) {
    std::vector<float> v(n);
    for (int i = 0; i < n; ++i)
        v[i] = amp * std::sin(6.2831853f * 220.f * i / 48000.f);
    return v;
}

static float rms(const std::vector<float>& v, size_t from = 0) {
    double acc = 0.0;
    for (size_t i = from; i < v.size(); ++i) acc += v[i] * v[i];
    return std::sqrt((float)(acc / (v.size() - from)));
}

// Run a mono-as-stereo signal through a fresh Comp at a given amount,
// skipping the first half second so envelopes/smoothers settle.
static std::vector<float> run(const std::vector<float>& in, float amount) {
    Comp c;
    c.init(48000.f);
    c.set_amount(amount);
    std::vector<float> out;
    out.reserve(in.size());
    for (float s : in) {
        float l = s, r = s;
        c.process(l, r);
        out.push_back(l);
    }
    return out;
}

TEST_CASE("comp: amount 0 is a bit-exact bypass") {
    Comp c;
    c.init(48000.f);
    auto in = sine(4800, 0.5f);
    for (float s : in) {
        float l = s, r = -s;
        c.process(l, r);
        CHECK(l == s);
        CHECK(r == -s);
    }
    CHECK(!c.engaged());
}

TEST_CASE("comp: loudness rises monotonically with the knob on quiet material") {
    // The spec's design intent, verified: quiet (-24 dBFS) must come UP.
    auto in = sine(96000, 0.063f);
    const float amounts[4] = {0.f, 0.33f, 0.66f, 1.f};
    float prev = -1.f;
    for (float a : amounts) {
        float level = rms(run(in, a), 24000);
        CHECK(level > prev);
        prev = level;
    }
}

TEST_CASE("comp: hot material is gain-reduced at full amount") {
    // 0.9-amp sine at amount 1: threshold -32 dB, ratio 10:1 -> heavy GR.
    // Net gain (GR + makeup) must be BELOW the makeup-only quiet case,
    // i.e. peaks come down relative to quiet material = compression.
    auto hot = sine(96000, 0.9f);
    auto quiet = sine(96000, 0.063f);
    float hot_gain = rms(run(hot, 1.f), 24000) / rms(hot, 24000);
    float quiet_gain = rms(run(quiet, 1.f), 24000) / rms(quiet, 24000);
    CHECK(quiet_gain > hot_gain);   // dynamics squeezed toward each other
    CHECK(hot_gain < 2.f);          // hot must not explode under makeup
}

TEST_CASE("comp: stereo link keeps the image") {
    // R at half of L: the SAME gain must hit both, so the ratio holds.
    Comp c;
    c.init(48000.f);
    c.set_amount(0.8f);
    auto in = sine(48000, 0.8f);
    for (size_t i = 0; i < in.size(); ++i) {
        float l = in[i], r = 0.5f * in[i];
        c.process(l, r);
        if (i > 24000 && std::fabs(in[i]) > 0.1f)
            CHECK(r == doctest::Approx(0.5f * l).epsilon(1e-5));
    }
}

TEST_CASE("comp: release slows as the knob rises (pump zone)") {
    // Step from loud to quiet; measure how long the envelope keeps the
    // gain depressed. At amount 1 (release 350 ms) recovery must take
    // longer than at amount 0.4 (release ~106 ms).
    auto recovery_gain = [](float amount) {
        Comp c;
        c.init(48000.f);
        c.set_amount(amount);
        // 1 s loud to charge the envelope
        for (int i = 0; i < 48000; ++i) {
            float l = 0.9f * std::sin(6.2831853f * 220.f * i / 48000.f);
            float r = l;
            c.process(l, r);
        }
        // 120 ms of near-silence, then read the gain
        for (int i = 0; i < 5760; ++i) { float l = 1e-4f, r = 1e-4f; c.process(l, r); }
        return c.gain_db();
    };
    // Deeper knob = slower recovery = gain still lower after 120 ms
    CHECK(recovery_gain(1.f) < recovery_gain(0.4f));
}

TEST_CASE("comp: attack bites within tens of milliseconds") {
    // Hot DC-ish input at full amount: after 50 ms the applied gain must be
    // BELOW unity (heavy GR beats the +20 dB makeup). Without a working
    // attack the envelope stays empty and the gain would sit at +20 dB.
    Comp c;
    c.init(48000.f);
    c.set_amount(1.f);
    for (int i = 0; i < 2400; ++i) { float l = 0.9f, r = 0.9f; c.process(l, r); }
    CHECK(c.gain_db() < 0.f);
}

TEST_CASE("comp: deterministic") {
    auto in = sine(48000, 0.7f);
    auto a = run(in, 0.75f);
    auto b = run(in, 0.75f);
    for (size_t i = 0; i < a.size(); ++i) CHECK(a[i] == b[i]);
}

TEST_CASE("comp: turning the knob back to 0 re-arms the bit-exact bypass") {
    Comp c;
    c.init(48000.f);
    c.set_amount(1.f);
    auto in = sine(24000, 0.5f);
    for (float s : in) { float l = s, r = s; c.process(l, r); }
    c.set_amount(0.f);
    // let the 2 ms amount smoother settle
    for (int i = 0; i < 4800; ++i) { float l = 0.f, r = 0.f; c.process(l, r); }
    CHECK(!c.engaged());
    for (float s : in) {
        float l = s, r = s;
        c.process(l, r);
        CHECK(l == s);
        CHECK(r == s);
    }
}
```

- [ ] **Step 2: Wire into CMake and verify the tests fail to compile**

In `CMakeLists.txt`, inside `add_executable(spky_tests ...)` after the line `tests/test_part_fx.cpp`, add:

```cmake
    engine/fx/comp.cpp
    tests/test_comp.cpp
```

and inside `add_executable(render ...)` after `engine/fx/part_fx.cpp`, add:

```cmake
    engine/fx/comp.cpp
```

Run: `cmake -B build -S . && cmake --build build`
Expected: FAIL — `engine/fx/comp.cpp` / `fx/comp.h` not found. That is the failing state; proceed.

- [ ] **Step 3: Write the implementation**

Create `engine/fx/comp.h`:

```cpp
#pragma once
#include "util/onepole.h"

namespace spky {

// One-knob compressor (M4.6 dynamics spec). Turning up = deeper threshold,
// higher ratio, slower release, auto-makeup: glue at ~1/3, dense at ~2/3,
// audible pumping in the top third. The knob is a loudness knob first —
// quiet material must come UP as the knob comes up.
//
// Cost control: detector + gain smoothing run per sample; the dB-domain
// gain computer (the only log/pow code) runs decimated every kDecimate
// samples (spec: ~0.3 % for both parts).
class Comp {
public:
    void init(float sample_rate);
    void set_amount(float n);                 // 0..1; 0 = bit-exact bypass
    float amount() const { return _amount_target; }
    // true while audibly processing: target > 0 or still smoothing to 0
    bool engaged() const { return _amount_target > 0.f || _amount.value() > kEps; }
    float gain_db() const;                    // applied gain incl. makeup (tests/meter)
    void process(float& l, float& r);

private:
    static constexpr int   kDecimate = 16;
    static constexpr float kEps      = 1e-4f;

    void update_curve(float a);               // amount -> thr/ratio/release/makeup
    void compute_gain();                      // decimated dB-domain gain computer

    OnePole _amount;                          // ~2 ms knob smoothing
    float _sr            = 48000.f;
    float _amount_target = 0.f;
    // curve (recomputed at the decimated rate from the smoothed amount)
    float _thr_db    = 0.f;
    float _inv_ratio = 1.f;                   // 1/ratio
    float _makeup_db = 0.f;
    // detector (stereo-linked linear peak envelope)
    float _env      = 0.f;
    float _att_coef = 0.f;                    // fixed ~5 ms
    float _rel_coef = 0.f;                    // from the knob
    // gain
    float _gain        = 1.f;                 // per-sample smoothed linear gain
    float _gain_target = 1.f;
    float _gain_coef   = 0.f;                 // ~2 ms
    int   _ctr         = 0;                   // decimation counter
};

} // namespace spky
```

Create `engine/fx/comp.cpp`:

```cpp
#include "fx/comp.h"
#include <algorithm>
#include <cmath>

using namespace spky;

namespace {
constexpr float kKneeDb     = 6.f;    // soft knee width
// THE by-ear loudness handle (spec: when in doubt, err toward loud).
constexpr float kMakeupComp = 0.7f;

inline float coef_for(float time_s, float sr) {
    return 1.f - std::exp(-1.f / (time_s * sr));
}
} // namespace

void Comp::init(float sample_rate) {
    _sr = sample_rate;
    _amount.init(sample_rate, 0.002f);
    _amount.reset(0.f);
    _amount_target = 0.f;
    _att_coef  = coef_for(0.005f, sample_rate);
    _gain_coef = coef_for(0.002f, sample_rate);
    update_curve(0.f);
    _env = 0.f;
    _gain = _gain_target = 1.f;
    _ctr = 0;
}

void Comp::set_amount(float n) {
    _amount_target = std::clamp(n, 0.f, 1.f);
}

float Comp::gain_db() const { return 20.f * std::log10(_gain); }

void Comp::update_curve(float a) {
    _thr_db = -32.f * a;
    const float ratio = 1.f + 9.f * a * a;          // 2:1 at 1/3, 5:1 at 2/3, 10:1 at 1
    _inv_ratio = 1.f / ratio;
    const float release_s = 0.06f + 0.29f * a * a;  // ~90 ms .. 350 ms (pump up top)
    _rel_coef = coef_for(release_s, _sr);
    _makeup_db = -_thr_db * (1.f - _inv_ratio) * kMakeupComp;
}

void Comp::compute_gain() {
    update_curve(_amount.value());
    const float env_db = 20.f * std::log10(std::max(_env, 1e-6f));
    const float over = env_db - _thr_db;
    float gr_db = 0.f;
    if (over >= kKneeDb * 0.5f) {
        gr_db = -over * (1.f - _inv_ratio);
    } else if (over > -kKneeDb * 0.5f) {            // quadratic soft knee
        const float t = over + kKneeDb * 0.5f;
        gr_db = -(t * t) / (2.f * kKneeDb) * (1.f - _inv_ratio);
    }
    _gain_target = std::pow(10.f, (gr_db + _makeup_db) / 20.f);
}

void Comp::process(float& l, float& r) {
    if (!engaged()) {
        if (_gain != 1.f) {                          // re-arm after disengage
            _gain = _gain_target = 1.f;
            _env = 0.f;
            _ctr = 0;
        }
        return;                                      // bit-exact bypass
    }
    _amount.process(_amount_target);
    const float peak = std::max(std::fabs(l), std::fabs(r));   // stereo link
    _env += (peak > _env ? _att_coef : _rel_coef) * (peak - _env);
    if (_ctr == 0) { compute_gain(); _ctr = kDecimate; }
    --_ctr;
    _gain += _gain_coef * (_gain_target - _gain);
    l *= _gain;
    r *= _gain;
}
```

- [ ] **Step 4: Build and run the tests**

Run: `cmake --build build && ./build/spky_tests.exe`
Expected: PASS, `Status: SUCCESS!`, test-case count = 181 + 8 = 189.

- [ ] **Step 5: Commit**

```bash
git add engine/fx/comp.h engine/fx/comp.cpp tests/test_comp.cpp CMakeLists.txt
git commit -m "feat(m4.6): one-knob Comp core — stereo-linked detector, decimated gain computer, auto-makeup

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 2: `Limiter` — stereo-linked, bit-transparent master ceiling

**Files:**
- Create: `engine/fx/limiter.h` (header-only)
- Create: `tests/test_limiter.cpp`
- Modify: `CMakeLists.txt` (add `tests/test_limiter.cpp` to `spky_tests`)

**Interfaces:**
- Consumes: nothing project-specific (`<algorithm>`, `<cmath>`).
- Produces (Task 4 relies on these exact signatures):
  `void Limiter::init()`, `void Limiter::set_drive(float n)` (0..1 → pre-gain 1–4×), `float Limiter::pre_gain() const`, `void Limiter::process(float& l, float& r)`.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_limiter.cpp`:

```cpp
#include <doctest/doctest.h>
#include <cmath>
#include <vector>
#include "fx/limiter.h"
using namespace spky;

static std::vector<float> sine(int n, float amp) {
    std::vector<float> v(n);
    for (int i = 0; i < n; ++i)
        v[i] = amp * std::sin(6.2831853f * 220.f * i / 48000.f);
    return v;
}

TEST_CASE("limiter: bit-transparent below the knee at drive 0") {
    // -2 dBFS (0.794) sits below the -1 dBFS knee (0.891): out == in, bit-exact.
    Limiter lim;
    lim.init();
    auto in = sine(48000, 0.794f);
    for (float s : in) {
        float l = s, r = -0.5f * s;
        lim.process(l, r);
        CHECK(l == s);
        CHECK(r == -0.5f * s);
    }
}

TEST_CASE("limiter: never exceeds 1.0, even at 4x drive into a full-scale square") {
    Limiter lim;
    lim.init();
    lim.set_drive(1.f);
    CHECK(lim.pre_gain() == doctest::Approx(4.f));
    for (int i = 0; i < 96000; ++i) {
        float l = (i / 100) % 2 ? 1.f : -1.f;
        float r = l;
        lim.process(l, r);
        CHECK(std::fabs(l) <= 1.f);
        CHECK(std::fabs(r) <= 1.f);
        CHECK(std::isfinite(l));
    }
}

TEST_CASE("limiter: stereo-linked — one gain for both channels") {
    // Loud L, quiet R: R must be scaled by the SAME riding gain as L
    // (below its own knee R would otherwise pass untouched).
    Limiter lim;
    lim.init();
    lim.set_drive(0.5f);                      // pre-gain 2.5x forces riding
    auto in = sine(48000, 0.9f);
    for (size_t i = 0; i < in.size(); ++i) {
        float l = in[i], r = 0.1f * in[i];
        lim.process(l, r);
        if (i > 4800 && std::fabs(in[i]) > 0.5f) {
            // R stays exactly 0.1 of the pre-ceiling L path: both got the
            // same pre-gain and the same riding gain; only the ceiling is
            // per-channel and R is far below it.
            float gain_l_path = l / in[i];   // includes ceiling on L
            float gain_r_path = r / (0.1f * in[i]);
            CHECK(gain_r_path >= gain_l_path - 1e-4f);  // R uncrushed
            CHECK(gain_r_path <= lim.pre_gain());       // but gain-ridden
        }
    }
}

TEST_CASE("limiter: deterministic") {
    auto run = [] {
        Limiter lim;
        lim.init();
        lim.set_drive(0.8f);
        std::vector<float> out;
        for (int i = 0; i < 48000; ++i) {
            float l = 0.9f * std::sin(6.2831853f * 90.f * i / 48000.f), r = l;
            lim.process(l, r);
            out.push_back(l);
        }
        return out;
    };
    auto a = run(), b = run();
    for (size_t i = 0; i < a.size(); ++i) CHECK(a[i] == b[i]);
}
```

- [ ] **Step 2: Wire into CMake, verify failure**

In `CMakeLists.txt`, `add_executable(spky_tests ...)`, after `tests/test_comp.cpp` add:

```cmake
    tests/test_limiter.cpp
```

Run: `cmake -B build -S . && cmake --build build`
Expected: FAIL — `fx/limiter.h` not found.

- [ ] **Step 3: Write the implementation**

Create `engine/fx/limiter.h`:

```cpp
#pragma once
#include <algorithm>
#include <cmath>

namespace spky {

// Master peak limiter (M4.6 dynamics spec). Gain-riding recipe after
// stmlib's Limiter (© Emilie Gillet, MIT — see THIRD_PARTY.md; no code
// copied verbatim), with three deliberate differences from the DaisySP
// copy: stereo-linked peak follower, EXACT bit-transparency below the
// knee at drive 0 (daisysp::Limiter applies SoftLimit(x*0.7)
// unconditionally), and a built-in master drive. Delivers the M6 shell
// spec's "Engine delta 3" (master soft-clip, transparent below ~-1 dBFS).
class Limiter {
public:
    void init() {
        _peak = 0.5f;
        _pre = 1.f;
    }
    void set_drive(float n) { _pre = 1.f + 3.f * std::clamp(n, 0.f, 1.f); }
    float pre_gain() const { return _pre; }

    void process(float& l, float& r) {
        const float pl = l * _pre, pr = r * _pre;
        const float peak = std::max(std::fabs(pl), std::fabs(pr));   // stereo link
        const float e = peak - _peak;
        _peak += (e > 0.f ? 0.05f : 0.00002f) * e;                   // stmlib slopes
        if (_pre == 1.f && _peak <= 1.f && peak <= kKnee)
            return;                                   // transparent: out == in, bit-exact
        const float gain = _peak > 1.f ? 1.f / _peak : 1.f;
        l = ceiling(pl * gain);
        r = ceiling(pr * gain);
    }

private:
    static constexpr float kKnee = 0.89125f;          // -1 dBFS

    // Piecewise ceiling: exact identity below the knee, tanh toward an
    // asymptote of exactly 1.0 above it (C1-continuous at the knee).
    static float ceiling(float x) {
        const float ax = std::fabs(x);
        if (ax <= kKnee) return x;
        const float y = kKnee + (1.f - kKnee) * std::tanh((ax - kKnee) / (1.f - kKnee));
        return x < 0.f ? -y : y;
    }

    float _peak = 0.5f;
    float _pre  = 1.f;
};

} // namespace spky
```

Note: the per-sample `std::tanh` runs ONLY above the knee (rare by design); the transparent path costs a compare. This honors the "no per-sample libm" rule in spirit — the hot path has none.

- [ ] **Step 4: Build and run the tests**

Run: `cmake --build build && ./build/spky_tests.exe`
Expected: PASS, `Status: SUCCESS!`, 189 + 4 = 193 test cases.

- [ ] **Step 5: Commit**

```bash
git add engine/fx/limiter.h tests/test_limiter.cpp CMakeLists.txt
git commit -m "feat(m4.6): stereo-linked master Limiter — bit-transparent knee, stmlib recipe, master drive

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 3: `Comp` into `PartFx` (before the send tap) + `Instrument::set_comp`

**Files:**
- Modify: `engine/fx/part_fx.h` (member + accessors)
- Modify: `engine/fx/part_fx.cpp:6-12` (`init`), `engine/fx/part_fx.cpp:19-46` (`process`)
- Modify: `engine/instrument.h:51-57` (FX API block — add `set_comp`)
- Test: `tests/test_part_fx.cpp` (append), `tests/test_instrument.cpp` (append)

**Interfaces:**
- Consumes: `Comp` from Task 1 (exact signatures listed there).
- Produces (Task 5 relies on): `void Instrument::set_comp(int p, float n)`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_part_fx.cpp` (reuse that file's existing includes/echo-buffer pattern; if it declares static echo buffers, reuse them instead of redeclaring):

```cpp
TEST_CASE("part_fx: comp default 0 leaves chain and send bit-exact") {
    static float el[Flux::kMaxSamples], er[Flux::kMaxSamples];
    PartFx fx;
    fx.init(48000.f, el, er);
    const float fxv[FXT_COUNT] = {0.f, 0.5f, 1.f, 0.5f, 0.f};
    for (int i = 0; i < 4800; ++i) {
        float s = 0.5f * std::sin(6.2831853f * 220.f * i / 48000.f);
        float l = s, r = s, sl = 0.f, sr = 0.f;
        fx.process(l, r, sl, sr, fxv);
        CHECK(l == s);                                   // FX off + comp 0 = dry bits
        CHECK(sl == doctest::Approx(s * std::sin(0.5f * 1.5707963f)));
    }
}

TEST_CASE("part_fx: comp sits BEFORE the send tap — the send gets louder too") {
    static float el[Flux::kMaxSamples], er[Flux::kMaxSamples];
    const float fxv[FXT_COUNT] = {0.f, 0.5f, 1.f, 0.8f, 0.f};
    auto send_rms = [&](float amount) {
        PartFx fx;
        fx.init(48000.f, el, er);
        fx.set_comp(amount);
        double acc = 0.0;
        int n = 0;
        for (int i = 0; i < 96000; ++i) {
            float s = 0.05f * std::sin(6.2831853f * 220.f * i / 48000.f);  // quiet!
            float l = s, r = s, sl = 0.f, sr = 0.f;
            fx.process(l, r, sl, sr, fxv);
            if (i >= 24000) { acc += sl * sl; ++n; }
        }
        return std::sqrt((float)(acc / n));
    };
    CHECK(send_rms(1.f) > send_rms(0.f) * 1.5f);   // full-wet motivation, verified
}
```

Append to `tests/test_instrument.cpp` (reuse its existing FxMem/init pattern for constructing an Instrument with FX memory — copy the fixture the reverb tests in that file use):

```cpp
TEST_CASE("instrument: set_comp forwards to the part chain") {
    // Two identically-seeded instruments, one with comp up: the comp'd one
    // must be louder on the same deterministic synth content.
    auto render_rms = [](float comp) {
        Instrument inst;
        inst.init(48000.f);                       // engine-only init: no FxMem needed
        inst.set_comp(0, comp);
        inst.trigger_manual(0);
        double acc = 0.0;
        float l[96], r[96];
        const float inL[96] = {0}, inR[96] = {0};
        int n = 0;
        for (int b = 0; b < 500; ++b) {
            inst.process(inL, inR, l, r, 96);
            if (b == 250) inst.trigger_manual(0);
            for (int i = 0; i < 96; ++i) { acc += l[i] * l[i]; ++n; }
        }
        return std::sqrt((float)(acc / n));
    };
    CHECK(render_rms(1.f) > render_rms(0.f));
}
```

NOTE for the implementer: `Instrument::init(float)` (engine-only) skips FxMem — check whether `PartFx` still runs in that mode (it does: `Part::init` receives null echo buffers and `Flux` handles it — see how existing instrument tests call `init(48000.f)`). If the quiet default synth level makes the RMS check flaky, raise comp'd expectation loudness by triggering more often — but keep the comparison strictly `>`.

- [ ] **Step 2: Run tests to verify failure**

Run: `cmake --build build && ./build/spky_tests.exe`
Expected: FAIL to compile — `PartFx::set_comp` / `Instrument::set_comp` don't exist yet.

- [ ] **Step 3: Implement**

`engine/fx/part_fx.h` — add the include at the top with the others:

```cpp
#include "fx/comp.h"
```

Inside `class PartFx`, after the `Flux& flux()` accessor block, add:

```cpp
    Comp& comp() { return _comp; }
    const Comp& comp() const { return _comp; }
    void set_comp(float n) { _comp.set_amount(n); }
```

In the private section, after `Flux _flux;` add:

```cpp
    Comp _comp;
```

Update the class comment (`// Per-part chain: GRIT -> FLUX -> FX MIX, plus the post-FX reverb send tap.`) to:

```cpp
// Per-part chain: GRIT -> FLUX -> FX MIX -> COMP, plus the post-COMP reverb
// send tap (M4.6: comp BEFORE the tap — dry and send are compressed and
// auto-gained together, so full-wet profits fully).
```

`engine/fx/part_fx.cpp` — in `init`, after `_flux.init(...)`:

```cpp
    _comp.init(sample_rate);
```

In `process`, between the FX-MIX blend block and the send-tap line, insert:

```cpp
    _comp.process(l, r);   // one-knob comp — BEFORE the send tap (spec: full-wet must profit)
```

`engine/instrument.h` — in the FX API block after `set_grit_mix`, add:

```cpp
    void set_comp(int p, float n)                  { _parts[p].fx().set_comp(n); }
```

(Confirm `Part` exposes `fx()` — `set_fx_on` at `engine/instrument.h:51` already uses `_parts[p].fx()`.)

- [ ] **Step 4: Build and run ALL tests**

Run: `cmake --build build && ./build/spky_tests.exe`
Expected: PASS, `Status: SUCCESS!`, 193 + 3 = 196 test cases. Existing part_fx/instrument/scenario tests must stay green — comp defaults to bit-exact bypass.

- [ ] **Step 5: Commit**

```bash
git add engine/fx/part_fx.h engine/fx/part_fx.cpp engine/instrument.h tests/test_part_fx.cpp tests/test_instrument.cpp
git commit -m "feat(m4.6): Comp into PartFx before the send tap; Instrument::set_comp

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 4: `Limiter` at the `Instrument` mix stage + `set_master_drive`

**Files:**
- Modify: `engine/instrument.h` (include, member, `set_master_drive`)
- Modify: `engine/instrument.cpp:7-18` (`init`), `engine/instrument.cpp:25-56` (`process`)
- Test: `tests/test_instrument.cpp` (append)

**Interfaces:**
- Consumes: `Limiter` from Task 2 (exact signatures listed there).
- Produces (Task 5 relies on): `void Instrument::set_master_drive(float n)`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_instrument.cpp`:

```cpp
TEST_CASE("instrument: master output never exceeds 1.0 even driven hard") {
    Instrument inst;
    inst.init(48000.f);
    inst.set_master_drive(1.f);                    // 4x into the ceiling
    inst.set_comp(0, 1.f);
    inst.set_comp(1, 1.f);
    inst.trigger_manual(0);
    inst.trigger_manual(1);
    float l[96], r[96];
    const float inL[96] = {0}, inR[96] = {0};
    for (int b = 0; b < 1000; ++b) {
        inst.process(inL, inR, l, r, 96);
        if (b % 100 == 0) { inst.trigger_manual(0); inst.trigger_manual(1); }
        for (int i = 0; i < 96; ++i) {
            CHECK(std::fabs(l[i]) <= 1.f);
            CHECK(std::fabs(r[i]) <= 1.f);
            CHECK(std::isfinite(l[i]));
        }
    }
}

TEST_CASE("instrument: dynamics chain is deterministic end to end") {
    auto run = [] {
        Instrument inst;
        inst.init(48000.f);
        inst.set_master_drive(0.7f);
        inst.set_comp(0, 0.9f);
        inst.trigger_manual(0);
        std::vector<float> out;
        float l[96], r[96];
        const float inL[96] = {0}, inR[96] = {0};
        for (int b = 0; b < 500; ++b) {
            inst.process(inL, inR, l, r, 96);
            for (int i = 0; i < 96; ++i) out.push_back(l[i]);
        }
        return out;
    };
    auto a = run(), b = run();
    for (size_t i = 0; i < a.size(); ++i) CHECK(a[i] == b[i]);
}
```

- [ ] **Step 2: Run tests to verify failure**

Run: `cmake --build build && ./build/spky_tests.exe`
Expected: FAIL to compile — `set_master_drive` doesn't exist.

- [ ] **Step 3: Implement**

`engine/instrument.h` — add with the includes:

```cpp
#include "fx/limiter.h"
```

In the public API after `set_reverb_depth`, add:

```cpp
    void set_master_drive(float n) { _limiter.set_drive(n); }
```

In the private section after `AmbientReverb* _reverb = nullptr;`, add:

```cpp
    Limiter _limiter;
```

`engine/instrument.cpp` — in `init(float, const FxMem&)` after `if (_reverb) _reverb->init(sample_rate);`:

```cpp
    _limiter.init();
```

In `process`, replace the two output lines

```cpp
        outL[i] = l;
        outR[i] = r;
```

with:

```cpp
        _limiter.process(l, r);   // master ceiling (M6 engine delta 3, delivered early)
        outL[i] = l;
        outR[i] = r;
```

- [ ] **Step 4: Build and run ALL tests**

Run: `cmake --build build && ./build/spky_tests.exe`
Expected: PASS, `Status: SUCCESS!`, 196 + 2 = 198 test cases. Existing tests stay green (drive 0 + sub-knee signals = bit-exact path).

- [ ] **Step 5: Commit**

```bash
git add engine/instrument.h engine/instrument.cpp tests/test_instrument.cpp
git commit -m "feat(m4.6): master Limiter at the Instrument mix stage; set_master_drive

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 5: Host actions + `comp_pump` showcase + render verification

**Files:**
- Modify: `host/render/scenario.cpp:104-109` (two new actions)
- Create: `host/render/scenarios/comp_pump.json`
- Test: `tests/test_scenario.cpp` (append)

**Interfaces:**
- Consumes: `Instrument::set_comp(int, float)` (Task 3), `Instrument::set_master_drive(float)` (Task 4).
- Produces: scenario actions `set_comp` (uses `part`, `value`) and `set_master_drive` (uses `value`).

- [ ] **Step 1: Write the failing test**

Append to `tests/test_scenario.cpp` (follow that file's existing pattern for constructing an `Event` and calling `apply_event` — copy the shape of an existing "action dispatch" case):

```cpp
TEST_CASE("scenario: set_comp and set_master_drive dispatch without throwing") {
    Instrument inst;
    inst.init(48000.f);
    Event e;
    e.action = "set_comp";
    e.part = 1;
    e.value = 0.8f;
    apply_event(inst, e);
    e.action = "set_master_drive";
    e.value = 0.5f;
    apply_event(inst, e);
    // No getters exist by design (matches set_reverb_*); reaching here alive
    // plus the Task 3/4 integration tests covers the wiring.
    CHECK(true);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build && ./build/spky_tests.exe`
Expected: the new case passes trivially even before wiring (unknown actions are ignored by design) — so verify failure differently: the test alone is NOT sufficient. Add the wiring in Step 3 and rely on the showcase render (Step 5) as the real end-to-end check. (This is the same accepted pattern as previous milestones' scenario actions.)

- [ ] **Step 3: Implement the actions**

`host/render/scenario.cpp` — after the `set_grit_mix` line, add:

```cpp
    else if (a == "set_comp")             inst.set_comp(e.part, e.value);
    else if (a == "set_master_drive")     inst.set_master_drive(e.value);
```

- [ ] **Step 4: Create the showcase scenario**

Create `host/render/scenarios/comp_pump.json` (full-wet duet after `entropy_duet.json`, sends pushed high; the comp ride is the story):

```json
{
  "sample_rate": 48000,
  "bpm": 75,
  "duration_s": 64,
  "init": [
    {"_comment":"M4.6 dynamics showcase. Full-wet duet (high reverb sends). Act 1 (0-10s): raw - quiet, distant. Act 2 (10s): comp glue - the room lifts. Act 3 (24s): dense - quiet notes bloom up. Act 4 (36s): full pump - the tail breathes rhythmically. Act 5 (48s): master drive moment. Outro: back to glue."},
    {"action":"set_scale","value":"min_pent"},

    {"action":"set_engine","part":0,"value":"synth"},
    {"action":"set_sync_mode","part":0,"value":"sync"},
    {"action":"set_rate","part":0,"value":0.25},
    {"action":"set_step","part":0,"flag":true,"ivalue":8},
    {"action":"set_shape","part":0,"value":0.95},
    {"action":"set_range","part":0,"value":0.7},
    {"action":"set_smooth","part":0,"value":0.4},
    {"action":"set_probability","part":0,"value":0.55},
    {"action":"set_entropy","part":0,"value":0.3},
    {"action":"set_depth","part":0,"value":0.9},
    {"action":"set_quant_mode","part":0,"value":"scale"},
    {"action":"set_target_active","part":0,"slot":0,"flag":true},
    {"action":"set_target_base","part":0,"slot":0,"value":0.4},
    {"action":"set_target_active","part":0,"slot":1,"flag":true},
    {"action":"set_target_base","part":0,"slot":1,"value":0.32},
    {"action":"set_target_depth","part":0,"slot":1,"value":0.5},
    {"action":"set_target_active","part":0,"slot":2,"flag":true},
    {"action":"set_target_base","part":0,"slot":2,"value":0.55},
    {"action":"set_target_active","part":0,"slot":4,"flag":true},
    {"action":"set_target_base","part":0,"slot":4,"value":0.55},
    {"action":"set_voice_attack","part":0,"value":0.01},
    {"action":"set_voice_decay","part":0,"value":0.6},
    {"action":"set_voice_resonance","part":0,"value":0.3},
    {"action":"set_voice_sub","part":0,"value":0.2},
    {"action":"set_voice_detune","part":0,"value":0.15},

    {"action":"set_engine","part":1,"value":"synth"},
    {"action":"set_sync_mode","part":1,"value":"sync"},
    {"action":"set_rate","part":1,"value":0.125},
    {"action":"set_step","part":1,"flag":true,"ivalue":8},
    {"action":"set_shape","part":1,"value":0.9},
    {"action":"set_range","part":1,"value":0.6},
    {"action":"set_smooth","part":1,"value":0.5},
    {"action":"set_probability","part":1,"value":0.5},
    {"action":"set_entropy","part":1,"value":0.2},
    {"action":"set_depth","part":1,"value":0.8},
    {"action":"set_quant_mode","part":1,"value":"scale"},
    {"action":"set_target_active","part":1,"slot":0,"flag":true},
    {"action":"set_target_base","part":1,"slot":0,"value":0.3},
    {"action":"set_target_active","part":1,"slot":2,"flag":true},
    {"action":"set_target_base","part":1,"slot":2,"value":0.3},
    {"action":"set_target_active","part":1,"slot":4,"flag":true},
    {"action":"set_target_base","part":1,"slot":4,"value":0.5},
    {"action":"set_voice_attack","part":1,"value":0.45},
    {"action":"set_voice_decay","part":1,"value":0.9},
    {"action":"set_voice_resonance","part":1,"value":0.3},
    {"action":"set_voice_sub","part":1,"value":0.5},
    {"action":"set_voice_detune","part":1,"value":0.3},

    {"_comment":"Full-wet: big room, HIGH sends from both parts. LEVELs kept moderate - the comp ride provides the loudness arc."},
    {"action":"set_reverb_size","value":0.7},
    {"action":"set_reverb_tone","value":0.55},
    {"action":"set_reverb_decay","value":0.78},
    {"action":"set_reverb_depth","value":0.3},
    {"action":"set_fx_target_active","part":0,"slot":3,"flag":true},
    {"action":"set_fx_target_base","part":0,"slot":3,"value":0.8},
    {"action":"set_fx_target_active","part":1,"slot":3,"flag":true},
    {"action":"set_fx_target_base","part":1,"slot":3,"value":0.85}
  ],
  "events": [
    {"t":10.0,"action":"set_comp","part":0,"value":0.35,"_comment":"glue in - the wash lifts"},
    {"t":10.0,"action":"set_comp","part":1,"value":0.35},
    {"t":24.0,"action":"set_comp","part":0,"value":0.7,"_comment":"dense - quiet notes bloom up"},
    {"t":24.0,"action":"set_comp","part":1,"value":0.7},
    {"t":36.0,"action":"set_comp","part":0,"value":1.0,"_comment":"full pump - the room breathes"},
    {"t":36.0,"action":"set_comp","part":1,"value":1.0},
    {"t":48.0,"action":"set_master_drive","value":0.6,"_comment":"drive moment - loudness wall"},
    {"t":56.0,"action":"set_master_drive","value":0.0,"_comment":"outro: back to glue"},
    {"t":56.0,"action":"set_comp","part":0,"value":0.4},
    {"t":56.0,"action":"set_comp","part":1,"value":0.4}
  ]
}
```

- [ ] **Step 5: Build, run all tests, render, verify**

```bash
cmake --build build && ./build/spky_tests.exe
./build/render.exe host/render/scenarios/comp_pump.json renders/comp_pump.wav renders/comp_pump.csv
./build/render.exe host/render/scenarios/comp_pump.json renders/comp_pump_2.wav renders/comp_pump_2.csv
cmp renders/comp_pump.wav renders/comp_pump_2.wav && echo DETERMINISTIC
```

Expected: tests `Status: SUCCESS!` (198 + 1 = 199); render completes; `DETERMINISTIC` prints. Then verify the arc numerically — RMS must STEP UP at 10 s / 24 s and the pump must not clip:

```bash
python - <<'EOF'
import wave, struct
w = wave.open("renders/comp_pump.wav")
n, sr, ch = w.getnframes(), w.getframerate(), w.getnchannels()
raw = struct.unpack(f"<{n*ch}h", w.readframes(n))
def rms(t0, t1):
    seg = raw[int(t0*sr)*ch:int(t1*sr)*ch]
    return (sum(s*s for s in seg)/len(seg))**0.5
print("raw   0-10s:", round(rms(2,10)))
print("glue 10-24s:", round(rms(12,24)))
print("dense 24-36:", round(rms(26,36)))
print("pump 36-48s:", round(rms(38,48)))
print("peak:", max(abs(s) for s in raw), "of 32767")
EOF
```

Expected: each stage RMS ≥ the previous (glue > raw is the acceptance-critical step: **quiet gets louder**); peak ≤ 32767 with zero clipped runs. If a stage does NOT rise, the tuning handles (in order): `kMakeupComp` 0.7 → 0.8 in `comp.cpp`, then scenario LEVEL bases. Do not accept a non-rising arc.

- [ ] **Step 6: Re-render ALL existing scenarios and check the re-pin caveat**

```bash
for s in host/render/scenarios/*.json; do
  n=$(basename "$s" .json)
  ./build/render.exe "$s" "renders/$n.wav" "renders/$n.csv"
done
```

Expected: all render without error. Scenarios peaking below −1 dBFS are bit-identical to before (comp defaults 0, drive 0, limiter transparent). `ambient_wash` peaks at ~−0.8 dBFS — above the knee — so its bits MAY change minutely (spec caveat, accepted; renders/ is gitignored, nothing to commit). Note in the task report which scenarios changed.

- [ ] **Step 7: Commit**

```bash
git add host/render/scenario.cpp host/render/scenarios/comp_pump.json tests/test_scenario.cpp
git commit -m "feat(m4.6): set_comp/set_master_drive scenario actions + comp_pump full-wet showcase

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 6: Docs — fork roadmap/README/THIRD_PARTY + shell-spec supersession (residency repo)

**Files:**
- Modify: `docs/roadmap.md` (fork — summary table row after M4.5 + detail section after the M4.5 section)
- Modify: `README.md` (fork — milestone table row after M4.5)
- Modify: `THIRD_PARTY.md` (fork — stmlib-recipe credit)
- Modify: `docs/superpowers/specs/2026-07-12-spotykach-firmware-shell-design.md` (residency repo — Engine delta 3 supersession note)

**Interfaces:** none — documentation only.

- [ ] **Step 1: Fork roadmap summary row**

In `docs/roadmap.md`, after the `| **M4.5** | … |` row, add:

```markdown
| **M4.6** | Dynamics — one-knob comp per part (glue → dense → pump, auto-makeup) + stereo-linked master limiter with MASTER DRIVE (delivers M6 engine delta 3 early) | ✅ **done** (engine + host; UI wiring deferred to M6) |
```

- [ ] **Step 2: Fork roadmap detail section**

After the `### M4.5 — Ambient reverb v2 (Oliverb port) ✅` section, add:

```markdown
### M4.6 — Dynamics ✅

One-knob compressor per part (`engine/fx/comp.*`, end of the PartFx chain
BEFORE the reverb send tap — dry and send are compressed and auto-gained
together, so full-wet patches profit fully) plus a stereo-linked master
limiter (`engine/fx/limiter.h`, stmlib gain-riding recipe, exact
bit-transparency below the −1 dBFS knee) at the Instrument mix stage with
MASTER DRIVE (pre-gain 1–4×). The comp knob is a loudness knob first:
threshold/ratio/release/auto-makeup ride one macro (glue ~2:1 at a third,
dense ~5:1 at two thirds, 10:1 + 350 ms pumping at the top). API:
`set_comp(part, n)` / `set_master_drive(n)`, boot defaults 0/0. Delivers
the M6 shell spec's "Engine delta 3" (master soft-clip) early. Showcase:
`comp_pump.json`. Spec + plan in the residency repo
(`2026-07-13-spotykach-dynamics-*.md`). M6 knob-map suggestions: GRIT
layer SMOOTH → COMP (per side), FLUX-layer TUNE (ex-shimmer) → MASTER
DRIVE.
```

- [ ] **Step 3: Fork README milestone row**

In `README.md`, after the `| **M4.5** | … |` row, add:

```markdown
| **M4.6** | Dynamics — one-knob comp per part + master limiter w/ MASTER DRIVE | **done** (engine + host) |
```

- [ ] **Step 4: THIRD_PARTY.md credit**

In `THIRD_PARTY.md`, in the "Vendored" section's bullet list (after the **Oliverb** bullet), add:

```markdown
- **stmlib Limiter recipe** — `engine/fx/limiter.h` reimplements the
  gain-riding recipe of stmlib's `Limiter` (© Emilie Gillet, MIT) with a
  stereo-linked peak follower, an exactly-transparent sub-knee path, and a
  built-in master drive. No stmlib code is copied verbatim; the recipe
  credit is retained here out of courtesy.
```

- [ ] **Step 5: Commit (fork)**

```bash
git add docs/roadmap.md README.md THIRD_PARTY.md
git commit -m "docs(m4.6): roadmap/README rows + stmlib limiter-recipe credit

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

- [ ] **Step 6: Residency repo — shell-spec supersession note**

In `docs/superpowers/specs/2026-07-12-spotykach-firmware-shell-design.md` (residency repo), find Engine delta 3 (the numbered item beginning `3. **Master soft-clip** at the `Instrument` mix stage`) and append to that item:

```markdown
   **[Superseded by M4.6, 2026-07-13]:** delivered early as the
   stereo-linked master limiter (`engine/fx/limiter.h`: gain riding +
   piecewise ceiling, exactly bit-transparent below the −1 dBFS knee at
   drive 0) plus `set_master_drive` (pre-gain 1–4×). M6 only wires the
   gestures — suggested homes per the dynamics spec: GRIT layer
   SMOOTH → COMP per side, FLUX-layer TUNE (ex-shimmer) → MASTER DRIVE.
```

- [ ] **Step 7: Commit (residency repo)**

```bash
cd "/c/Users/bernd/Documents/AI/Synthux Design Residency"
git add docs/superpowers/specs/2026-07-12-spotykach-firmware-shell-design.md
git commit -m "docs: M4.6 supersession note in the M6 shell spec (engine delta 3 delivered early)

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

## Acceptance criteria (whole plan)

1. All doctest cases green (199 expected), fresh configure + build.
2. `comp_pump.wav` renders deterministically (double-render `cmp` identical) with a monotonically rising RMS arc across raw → glue → dense → pump and zero clipping (peak ≤ 32767).
3. With comp 0 / drive 0, every pre-existing scenario that peaks below −1 dBFS renders bit-identically to the pre-M4.6 build.
4. No LGPL code compiled or linked; `engine/` has no libDaisy include and no per-sample libm call in the dynamics hot path (log/pow decimated ×16; tanh only above the knee).
5. `src/`, firmware `main.cpp`/`app.*`, `Makefile` untouched.
6. By-ear pass (user, after implementation): the comp knob sweep on `comp_pump` — glue musical, pump breathing not stuttering, quiet material audibly blooming up; ear-tunables `kMakeupComp` (0.7), knee 6 dB, release curve, mapping anchors.

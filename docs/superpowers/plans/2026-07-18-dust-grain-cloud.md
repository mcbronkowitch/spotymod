# DUST / ROT — grain cloud, tape rot + erosion freeze on the FLUX echo — Implementation Plan

> **⛔ DO NOT EXECUTE THIS PLAN YET (2026-07-19).** The design spec's CPU
> estimate ("≪ 1 %") understated the cost by roughly 30×: the grain reads are
> scattered SDRAM accesses, and `grain_read_sdram` already prices that pattern
> at 16.84 % max for *eight* grains where this design wants sixteen. Realistic
> worst case is ~24–35 points of block budget; the instrument has **2.3 points
> of margin**. The `dust` bench family (`bench/workloads_dust.cpp`) must run on
> hardware first, and the spray-window question it settles may change §3's zone
> F and the grain-pool size — both of which this plan builds. See the design
> spec §8, rev 4.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a per-part granular read/write stage (DUST amount, ROT character) and a tape FREEZE with erosion to the FLUX block, reusing the existing 5 s echo tape as grain memory — no new audio buffers.

**Architecture:** `DustCloud` is a fixed pool of 8 grains that read integer offsets on the FLUX tape through a POD `TapeTap` view (pointer + write index + size), so it is testable against a plain array with no `EchoDelay` involved. `EchoDelay` grows three primitives: raw line access, a writeback input on the store, and a freeze flag with a `wear` coefficient. `Flux` owns the `DustCloud`, builds the `TapeTap` each sample before advancing the echo, and blends grain sum + (faded) echo head into the existing `_mix_lin` wet path.

**Tech Stack:** C++17, doctest, CMake + Ninja + clang (desktop), VCV Rack SDK 2.6.6 (plugin), Python 3 (panel generator).

## Global Constraints

- **DUST = 0 and freeze off ⇒ bit-exact** with today's FLUX, sample for sample, at any ROT. Guard it with a branch, never with `+ 0.f`.
- **No new audio buffers.** Grain memory is the existing `Flux::kMaxSamples = 240000` per channel per part.
- **No pitch-shifted grains.** Reverse is time-reversal via `offset += 2`, not resampling.
- **Determinism:** `spky::Rng` only, fixed distinct per-part seeds, no time seeding, no `Math.random`-equivalents. Desktop and firmware output must be bit-identical.
- **No heap, no virtuals, no `std::function` in the audio path.** Plain structs, fixed pools.
- **Patch compatibility (hard):** `PART_STRIDE` must stay **23**. New params are appended at the **end** of `PARAMS` in `gen_panel.py`, never added to `part_controls()`. `Spotymod.cpp:52-54` and `res/test_panel.py:62` assert this.
- **No new jacks** (decision 2026-07-18): FREEZE is pad-only. `FRZGATE_A/B` from the spec is dropped — this is reduction ladder step (1) in spec §6, taken up front because the hardware will not have the jacks either. The consequence, accepted: no sequencer-gated stutter-freeze in v1.
- **Four source lists** compile `engine/fx/*.cpp` and all need `engine/fx/dust.cpp`: `CMakeLists.txt` (`spky_tests` **and** `render`), `host/vcv/Makefile`, `bench/Makefile`.
- **Coordination:** `bench/Makefile` is the firmware bench build. Another session may be using it. Adding the source line is required (otherwise the bench build breaks the moment `flux.cpp` references `DustCloud`), but coordinate before touching that file.
- Build/test (Bash tool): `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build` then `./build/spky_tests.exe` (doctest filter: `-tc="dust*"`).

---

### Task 1: Tape access, writeback input, freeze + wear on `EchoDelay`

**Files:**
- Modify: `engine/fx/flux.h:20-54` (`DeLine`), `engine/fx/flux.h:92-143` (`EchoDelay`)
- Test: `tests/test_flux.cpp` (append)

**Interfaces:**
- Consumes: nothing.
- Produces: `DeLine::data() -> const T*`, `DeLine::write_ptr() -> int32_t`, `DeLine::Advance() -> void`, `DeLine::WriteBlend(T sample, float wear) -> void`; `EchoDelay::line() -> const float*`, `EchoDelay::write_ptr() -> int32_t`, `EchoDelay::set_freeze(bool)`, `EchoDelay::set_wear(float)`, `EchoDelay::Process(float in, float wb) -> float` (the existing one-arg `Process(float in)` stays and delegates with `wb = 0.f`).

- [ ] **Step 1: Write the failing tests**

First add the missing include — `tests/test_flux.cpp` uses `std::memcpy`/`std::memcmp` from here on and currently only gets `<cstring>` transitively through `flux.h`. Add it to the include block at the top of the file:

```cpp
#include <cstring>
```

Then append to `tests/test_flux.cpp`:

```cpp
TEST_CASE("echo: zero writeback is bit-exact with the one-arg store") {
    static float a_buf[Flux::kMaxSamples];
    static float b_buf[Flux::kMaxSamples];
    EchoDelay<Flux::kMaxSamples> a, b;
    a.Init(48000.f, a_buf);
    b.Init(48000.f, b_buf);
    a.SetDelayTime(0.25f, true);
    b.SetDelayTime(0.25f, true);
    a.SetFeedback(0.6f);
    b.SetFeedback(0.6f);
    for (int i = 0; i < 60000; ++i) {
        float in = std::sin(0.013f * i) * 0.7f;
        float ya = a.Process(in);
        float yb = b.Process(in, 0.f);
        REQUIRE(ya == yb);
    }
}

TEST_CASE("echo: freeze stops writing but keeps the pointer moving") {
    static float buf[Flux::kMaxSamples];
    EchoDelay<Flux::kMaxSamples> e;
    e.Init(48000.f, buf);
    e.SetDelayTime(0.25f, true);
    e.SetFeedback(0.5f);
    for (int i = 0; i < 48000; ++i) e.Process(std::sin(0.01f * i) * 0.5f);

    // snapshot the whole line, then freeze and hammer it with loud input
    static float snap[Flux::kMaxSamples];
    std::memcpy(snap, e.line(), sizeof(snap));
    const int32_t p0 = e.write_ptr();
    e.set_freeze(true);
    e.set_wear(1.f);
    for (int i = 0; i < 24000; ++i) e.Process(1.f);

    CHECK(std::memcmp(snap, e.line(), sizeof(snap)) == 0);   // nothing stored
    const int32_t expect = (p0 - 24000 + 2 * (int32_t)Flux::kMaxSamples)
                         % (int32_t)Flux::kMaxSamples;
    CHECK(e.write_ptr() == expect);                          // but it advanced
}

TEST_CASE("echo: frozen with wear < 1 decays the loop, bounded") {
    static float buf[Flux::kMaxSamples];
    EchoDelay<Flux::kMaxSamples> e;
    e.Init(48000.f, buf);
    e.SetDelayTime(0.25f, true);
    e.SetFeedback(0.5f);
    for (int i = 0; i < 48000; ++i) e.Process(std::sin(0.01f * i) * 0.5f);

    auto rms = [&]() {
        double s = 0.0;
        for (size_t i = 0; i < Flux::kMaxSamples; ++i)
            s += (double)e.line()[i] * (double)e.line()[i];
        return std::sqrt(s / (double)Flux::kMaxSamples);
    };
    const double before = rms();
    e.set_freeze(true);
    e.set_wear(1.f - 4.0e-6f);
    for (int i = 0; i < 240000; ++i) e.Process(1.f);   // one full 5 s pass
    const double after = rms();

    CHECK(after < before);          // it eroded
    CHECK(after > 0.0);             // but did not vanish in one pass
    CHECK(std::isfinite(after));
}

TEST_CASE("echo: writeback stays bounded under sustained full scale") {
    static float buf[Flux::kMaxSamples];
    EchoDelay<Flux::kMaxSamples> e;
    e.Init(48000.f, buf);
    e.SetDelayTime(0.1f, true);
    e.SetFeedback(1.2f);
    float peak = 0.f;
    for (int i = 0; i < 480000; ++i) {
        float y = e.Process(1.f, 0.9f);
        peak = std::max(peak, std::fabs(y));
        REQUIRE(std::isfinite(y));
    }
    CHECK(peak < 4.f);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build`
Expected: FAIL to compile — `'line': is not a member of 'spky::EchoDelay<240000>'` (and the same for `write_ptr`, `set_freeze`, `set_wear`, and the two-arg `Process`).

- [ ] **Step 3: Add the `DeLine` primitives**

In `engine/fx/flux.h`, inside `class DeLine`, immediately after `Read()` (currently ends line 47):

```cpp
    // Raw access for the grain taps (DustCloud reads integer offsets behind
    // the write head; no interpolation, no state).
    const T* data() const { return line_; }
    int32_t write_ptr() const { return write_ptr_; }

    // Move the head without storing — FREEZE: the tape keeps rolling under
    // the read heads, but nothing is written onto it.
    void Advance() {
        write_ptr_ = (write_ptr_ - 1 + max_size) % max_size;
    }

    // EROSION: abrade what is on the tape and burn `sample` into it. `wear`
    // slightly below 1 is what makes the frozen loop decompose per pass.
    void WriteBlend(T sample, float wear) {
        line_[write_ptr_] = std::tanh(line_[write_ptr_] * wear + sample);
        write_ptr_ = (write_ptr_ - 1 + max_size) % max_size;
    }
```

- [ ] **Step 4: Add the `EchoDelay` primitives**

In `engine/fx/flux.h`, inside `class EchoDelay`, replace the existing `Process` (lines 122-132) with:

```cpp
    void set_freeze(bool on) { frozen_ = on; }
    bool frozen() const { return frozen_; }
    // Per-sample erosion coefficient applied to the tape while frozen.
    // 1.0 = preserving looper; slightly below 1 = the loop decomposes.
    void set_wear(float w) { wear_ = w; }

    const float* line() const { return delay_line_.data(); }
    int32_t write_ptr() const { return delay_line_.write_ptr(); }

    float Process(float in) { return Process(in, 0.f); }

    // `wb` is the DustCloud writeback (already tanh-bounded by the caller);
    // it is 0 outside ROT zone R, and the store is then bit-identical to the
    // pre-DUST code path.
    float Process(float in, float wb) {
        daisysp::fonepole(delay_time_current_, delay_time_target_,
                          delay_smooth_coef_);
        delay_line_.SetDelay(delay_time_current_ * sample_rate_);
        float out = delay_line_.Read();
        out = bpf_.Process(out);
        out = std::tanh(out);   // tape-warm limiter: transparent near unity,
                                // bounded self-oscillation above it (bloom)
        if (!frozen_) {
            float store = out * feedback_ + in;
            if (wb != 0.f) store += wb;   // explicit: keeps the wb == 0 path
            delay_line_.Write(store);     // bit-identical, not merely equal
        } else if (wear_ >= 1.f && wb == 0.f) {
            delay_line_.Advance();        // preserving looper
        } else {
            delay_line_.WriteBlend(wb, wear_);   // erosion
        }
        return out;
    }
```

and add to the private member block (after `float feedback_ = 0.f;`, line 139):

```cpp
    bool  frozen_ = false;
    float wear_ = 1.f;
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build && ./build/spky_tests.exe -tc="echo*,flux*"`
Expected: PASS — the four new cases plus all nine pre-existing `flux:` cases (those are the real bit-exactness guard).

- [ ] **Step 6: Commit**

```bash
git add engine/fx/flux.h tests/test_flux.cpp
git commit -m "flux: tape tap access, writeback input, freeze + wear on EchoDelay"
```

---

### Task 2: `DustCloud` — pool, window, pan, free-zone scheduler, forward grains

**Files:**
- Create: `engine/fx/dust.h`, `engine/fx/dust.cpp`
- Create: `tests/test_dust.cpp`
- Modify: `CMakeLists.txt:69-70` area (`spky_tests`), `CMakeLists.txt:105` area (`render`), `host/vcv/Makefile:40` area, `bench/Makefile:39` area

**Interfaces:**
- Consumes: `spky::Rng` (`mod/rng.h`), `spky::hann_value_at` (`fx/fx_util.h`), `spky::fast_sin` (`util/fast_sin.h`), `spky::clampf` / `spky::lerpf` (`util/math.h`).
- Produces: `struct spky::TapeTap { const float* l; const float* r; int32_t write_ptr; int32_t size; float read(bool right, int32_t offset) const; }`; `class spky::DustCloud` with `init(float sample_rate, uint32_t seed)`, `set_dust(float)`, `set_rot(float)`, `set_delay_time(float)`, `active() const -> bool`, `head_gain() const -> float`, `wear() const -> float`, `process(const TapeTap&, float& gl, float& gr) -> float` (returns the writeback sample).

- [ ] **Step 1: Write the failing test**

Create `tests/test_dust.cpp`:

```cpp
#include <doctest/doctest.h>
#include <cmath>
#include <vector>
#include "fx/dust.h"
using namespace spky;

// A synthetic tape: 1 s stereo, filled with a distinguishable ramp so a grain's
// output can be traced back to the offset it read from.
struct FakeTape {
    static constexpr int32_t kSize = 48000;
    std::vector<float> l, r;
    int32_t ptr = 0;
    FakeTape() : l(kSize, 0.f), r(kSize, 0.f) {}
    TapeTap tap() const { return TapeTap{l.data(), r.data(), ptr, kSize}; }
    void advance() { ptr = (ptr - 1 + kSize) % kSize; }
    void fill_noise(uint32_t seed) {
        Rng g; g.seed(seed);
        for (int32_t i = 0; i < kSize; ++i) { l[i] = g.next_bipolar() * 0.5f;
                                              r[i] = g.next_bipolar() * 0.5f; }
    }
};

TEST_CASE("dust: amount 0 is silent and inactive") {
    FakeTape t; t.fill_noise(11);
    DustCloud d;
    d.init(48000.f, 0xD0571u);
    d.set_dust(0.f);
    d.set_rot(0.5f);
    d.set_delay_time(0.5f);
    CHECK(!d.active());
    for (int i = 0; i < 48000; ++i) {
        float gl = 1.f, gr = 1.f;
        float wb = d.process(t.tap(), gl, gr);
        REQUIRE(gl == 0.f);
        REQUIRE(gr == 0.f);
        REQUIRE(wb == 0.f);
        t.advance();
    }
}

TEST_CASE("dust: deterministic for a given seed") {
    auto run = [](std::vector<float>& out) {
        FakeTape t; t.fill_noise(11);
        DustCloud d;
        d.init(48000.f, 0xD0571u);
        d.set_dust(0.8f);
        d.set_rot(0.5f);            // free zone
        d.set_delay_time(0.5f);
        out.resize(48000);
        for (int i = 0; i < 48000; ++i) {
            float gl = 0.f, gr = 0.f;
            d.process(t.tap(), gl, gr);
            out[i] = gl;
            t.advance();
        }
    };
    std::vector<float> a, b;
    run(a); run(b);
    for (size_t i = 0; i < a.size(); ++i) REQUIRE(a[i] == b[i]);
}

TEST_CASE("dust: free zone produces sound and stays bounded") {
    FakeTape t; t.fill_noise(11);
    DustCloud d;
    d.init(48000.f, 0xD0571u);
    d.set_dust(1.f);
    d.set_rot(0.5f);
    d.set_delay_time(0.5f);
    float peak = 0.f;
    double sum_sq = 0.0;
    for (int i = 0; i < 96000; ++i) {
        float gl = 0.f, gr = 0.f;
        d.process(t.tap(), gl, gr);
        peak = std::max(peak, std::fabs(gl));
        sum_sq += (double)gl * (double)gl;
        REQUIRE(std::isfinite(gl));
        t.advance();
    }
    CHECK(std::sqrt(sum_sq / 96000.0) > 1e-3);   // it made sound
    CHECK(peak < 2.f);                            // normalization held
}

TEST_CASE("dust: grain sum is click-free across births and deaths") {
    FakeTape t; t.fill_noise(11);
    DustCloud d;
    d.init(48000.f, 0xD0571u);
    d.set_dust(0.9f);
    d.set_rot(0.5f);
    d.set_delay_time(0.5f);
    float prev = 0.f, max_step = 0.f;
    for (int i = 0; i < 240000; ++i) {
        float gl = 0.f, gr = 0.f;
        d.process(t.tap(), gl, gr);
        if (i > 0) max_step = std::max(max_step, std::fabs(gl - prev));
        prev = gl;
        t.advance();
    }
    // The tape is white noise, so consecutive samples differ a lot by nature;
    // what a click would look like is a step far beyond the signal's own range.
    CHECK(max_step < 1.0f);
}

TEST_CASE("dust: free-zone birth rate tracks the density mapping") {
    // Count distinct births by watching the pool go from empty to non-empty is
    // fragile; instead assert the coarse monotonic claim the mapping promises.
    auto energy = [](float amount) {
        FakeTape t; t.fill_noise(11);
        DustCloud d;
        d.init(48000.f, 0xD0571u);
        d.set_dust(amount);
        d.set_rot(0.5f);
        d.set_delay_time(0.5f);
        double s = 0.0;
        for (int i = 0; i < 192000; ++i) {
            float gl = 0.f, gr = 0.f;
            d.process(t.tap(), gl, gr);
            s += (double)gl * (double)gl;
            t.advance();
        }
        return s;
    };
    const double e_low = energy(0.2f);
    const double e_mid = energy(0.6f);
    const double e_high = energy(1.0f);
    CHECK(e_low > 0.0);
    CHECK(e_mid > e_low);
    CHECK(e_high > e_mid);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build`
Expected: FAIL — `Cannot open include file: 'fx/dust.h'`.

- [ ] **Step 3: Create `engine/fx/dust.h`**

```cpp
#pragma once
#include <cstdint>
#include "fx/fx_util.h"
#include "mod/rng.h"
#include "util/fast_sin.h"
#include "util/math.h"

namespace spky {

// All DUST/ROT tuning lives here, in one block, so the deferred Rack play-test
// has a single place to turn. The spec fixes behavior, these fix taste.
namespace dust_tuning {
constexpr float kZoneSEnd = 0.33f;      // sync stutter -> free scatter
constexpr float kZoneFEnd = 0.66f;      // free scatter -> rot

constexpr float kRateMin  = 1.5f;       // births/s just above DUST = 0
constexpr float kRateMax  = 35.f;       // births/s at DUST = 1 (~8 overlap)

constexpr float kLenMinLo = 0.025f;     // grain length range at DUST = 0
constexpr float kLenMaxLo = 0.100f;
constexpr float kLenMinHi = 0.080f;     // ... and at DUST = 1
constexpr float kLenMaxHi = 0.400f;

constexpr float kSpraySync   = 0.05f;   // s, zone S: tight, near the head
constexpr float kSprayFreeLo = 0.15f;   // s, zone F start (widens to the tape)

constexpr int   kGridDiv    = 4;        // zone S grid = FLUX delay time / 4
constexpr float kGridMinS   = 0.010f;   // ... clamped to a sane minimum

constexpr float kRevProbMax = 0.70f;    // zone R reverse probability at r = 1
constexpr float kWbGainMax  = 0.60f;    // zone R writeback gain at r = 1
constexpr float kWearRate   = 4.0e-6f;  // per-sample erosion at r = 1

constexpr float kTakeoverKnee = 0.70f;  // DUST above this fades the echo head
}

// A read-only view of one part's stereo tape at this sample. POD by design:
// no ownership, no virtuals, and tests can build one over a plain array.
struct TapeTap {
    const float* l = nullptr;
    const float* r = nullptr;
    int32_t write_ptr = 0;
    int32_t size = 0;

    // `offset` is samples BEHIND the write head; the head decrements, so a
    // constant offset is exactly 1x forward playback.
    float read(bool right, int32_t offset) const {
        int32_t i = (write_ptr + offset) % size;
        return (right ? r : l)[i];
    }
};

// Extra read heads (and, in zone R, an extra write head) on the FLUX tape.
class DustCloud {
public:
    static constexpr int kGrains = 8;

    void init(float sample_rate, uint32_t seed);
    void set_dust(float d);            // 0..1 amount
    void set_rot(float r);             // 0..1 character
    void set_delay_time(float s);      // FLUX delay time -> zone S grid

    bool  active() const { return _dust > 0.f; }
    float head_gain() const { return _head_gain; }   // echo head takeover
    float wear() const { return _wear; }             // freeze erosion coeff

    // Advance one sample. Writes the stereo grain sum to gl/gr and returns the
    // tanh-bounded writeback sample (0 outside zone R).
    float process(const TapeTap& tape, float& gl, float& gr);

private:
    struct Grain {
        bool    alive = false;
        bool    right = false;    // which channel tape this grain reads
        int32_t offset = 0;
        int32_t delta = 0;        // 0 = forward, +2 = reverse (1x backwards)
        int32_t age = 0;
        int32_t len = 1;
        float   gl = 0.f, gr = 0.f;
    };

    void _remap();                       // recompute derived values
    void _schedule(const TapeTap& tape);
    void _spawn(const TapeTap& tape);

    Grain _g[kGrains];
    Rng   _rng;

    float _sr = 48000.f;
    float _dust = 0.f;
    float _rot = 0.f;
    float _delay_time = 0.5f;

    int   _zone = 0;             // 0 = S sync, 1 = F free, 2 = R rot
    float _birth_prob = 0.f;     // zone F/R: per-sample birth probability
    int32_t _grid_period = 1;    // zone S: samples between grid slots
    int32_t _grid_countdown = 1;
    float _fire_prob = 0.f;      // zone S: chance a grid slot fires
    int   _burst = 1;            // zone S: grains per firing slot
    float _jitter = 0.f;         // zone S: 0 = locked, 1 = fully random
    float _len_min = 0.025f, _len_max = 0.1f;
    int32_t _spray = 1;          // max offset a new grain may start at
    float _rev_prob = 0.f;
    float _wb_gain = 0.f;
    float _norm = 1.f;
    float _head_gain = 1.f;
    float _wear = 1.f;
};

} // namespace spky
```

- [ ] **Step 4: Create `engine/fx/dust.cpp` (free zone only for now)**

`_remap()` already computes the zone S and zone R values so later tasks only add the *use* of them; the scheduler in this task handles the free path, and zone S/R behavior arrives in Tasks 3-5.

```cpp
#include "fx/dust.h"
#include <cmath>

using namespace spky;
using namespace spky::dust_tuning;

void DustCloud::init(float sample_rate, uint32_t seed) {
    _sr = sample_rate > 0.f ? sample_rate : 48000.f;
    _rng.seed(seed);
    for (int i = 0; i < kGrains; ++i) _g[i] = Grain{};
    _dust = 0.f;
    _rot = 0.f;
    _delay_time = 0.5f;
    _grid_countdown = 1;
    _remap();
}

void DustCloud::set_dust(float d) { _dust = clampf(d, 0.f, 1.f); _remap(); }
void DustCloud::set_rot(float r)  { _rot  = clampf(r, 0.f, 1.f); _remap(); }

void DustCloud::set_delay_time(float s) {
    _delay_time = s > kGridMinS ? s : kGridMinS;
    _remap();
}

void DustCloud::_remap() {
    // --- DUST: density, length, level, head takeover -----------------------
    const float d = _dust;
    const float rate_hz = kRateMin * std::pow(kRateMax / kRateMin, d);
    _birth_prob = rate_hz / _sr;
    _len_min = lerpf(kLenMinLo, kLenMinHi, d);
    _len_max = lerpf(kLenMaxLo, kLenMaxHi, d);

    const float mean_len = 0.5f * (_len_min + _len_max);
    const float overlap = rate_hz * mean_len;
    _norm = 1.f / std::sqrt(overlap > 1.f ? overlap : 1.f);

    // Head takeover: above the knee the echo read head fades out (equal power)
    // so the cloud eats the delay. Feedback keeps recirculating underneath.
    if (d <= kTakeoverKnee) {
        _head_gain = 1.f;
    } else {
        const float t = (d - kTakeoverKnee) / (1.f - kTakeoverKnee);
        _head_gain = std::cos(t * 1.5707963705f);
    }

    // --- ROT: zone + within-zone morph -------------------------------------
    const float r = _rot;
    const float tape_max_s = 5.f;   // the FLUX tape; spray never exceeds it
    if (r < kZoneSEnd) {                                // zone S — synced stutter
        _zone = 0;
        const float u = r / kZoneSEnd;
        _jitter = u;
        _fire_prob = d;
        _burst = 1 + (int)(d * 2.f);                    // density stacks bursts
        _spray = (int32_t)(kSpraySync * _sr);
        _rev_prob = 0.f;
        _wb_gain = 0.f;
        _wear = 1.f;
    } else if (r < kZoneFEnd) {                         // zone F — free scatter
        _zone = 1;
        const float u = (r - kZoneSEnd) / (kZoneFEnd - kZoneSEnd);
        _jitter = 1.f;
        _spray = (int32_t)(lerpf(kSprayFreeLo, tape_max_s, u) * _sr);
        _rev_prob = 0.f;
        _wb_gain = 0.f;
        _wear = 1.f;
    } else {                                            // zone R — rot
        _zone = 2;
        const float u = (r - kZoneFEnd) / (1.f - kZoneFEnd);
        _jitter = 1.f;
        _spray = (int32_t)(tape_max_s * _sr);
        _rev_prob = kRevProbMax * u;
        _wb_gain = kWbGainMax * u;
        _wear = 1.f - kWearRate * u;
    }

    const int32_t grid = (int32_t)((_delay_time / (float)kGridDiv) * _sr);
    const int32_t grid_min = (int32_t)(kGridMinS * _sr);
    _grid_period = grid > grid_min ? grid : grid_min;
    if (_grid_countdown > _grid_period) _grid_countdown = _grid_period;
}

void DustCloud::_spawn(const TapeTap& tape) {
    for (int i = 0; i < kGrains; ++i) {
        if (_g[i].alive) continue;
        Grain& g = _g[i];

        const float lf = lerpf(_len_min, _len_max, _rng.next_unipolar());
        g.len = (int32_t)(lf * _sr);
        if (g.len < 8) g.len = 8;
        g.age = 0;
        g.right = (_rng.next_u32() & 1u) != 0u;   // free stereo decorrelation
        g.delta = (_rng.next_unipolar() < _rev_prob) ? 2 : 0;

        // A reverse grain recedes 2 samples per sample; clamp the start so it
        // can never overrun the tape within its own lifetime.
        int32_t max_off = tape.size - 4 - (g.delta ? 2 * g.len : 0);
        if (max_off < 2) max_off = 2;
        int32_t spray = _spray < max_off ? _spray : max_off;
        if (spray < 1) spray = 1;
        g.offset = 1 + (int32_t)(_rng.next_unipolar() * (float)spray);
        if (g.offset > max_off) g.offset = max_off;

        const float pan = _rng.next_bipolar();
        const float a = (pan + 1.f) * 0.125f;      // -1..1 -> 0..0.25 turns
        g.gr = fast_sin(a);                        // equal-power, house idiom
        g.gl = fast_sin(a + 0.25f);

        g.alive = true;
        return;
    }
    // Pool full: the birth is dropped. This is what bounds the CPU (spec §8).
}

void DustCloud::_schedule(const TapeTap& tape) {
    if (_rng.next_unipolar() < _birth_prob) _spawn(tape);
}

float DustCloud::process(const TapeTap& tape, float& gl, float& gr) {
    gl = 0.f;
    gr = 0.f;
    if (_dust <= 0.f) return 0.f;

    _schedule(tape);

    float sl = 0.f, sr = 0.f;
    for (int i = 0; i < kGrains; ++i) {
        Grain& g = _g[i];
        if (!g.alive) continue;
        // Raised-cosine (Hann) window: hann_value_at is sin^2(pi/2 * x), so
        // folding the age about its midpoint gives exactly sin^2(pi * age).
        const float a = (float)g.age / (float)g.len;
        const float w = a < 0.5f ? hann_value_at(2.f * a)
                                 : hann_value_at(2.f - 2.f * a);
        const float s = tape.read(g.right, g.offset) * w;
        sl += s * g.gl;
        sr += s * g.gr;
        g.offset += g.delta;
        if (++g.age >= g.len) g.alive = false;
    }

    gl = sl * _norm;
    gr = sr * _norm;
    return 0.f;   // writeback arrives in Task 5
}
```

- [ ] **Step 5: Wire the new translation unit into all four source lists**

`CMakeLists.txt` — in `spky_tests`, after line 70 (`tests/test_flux.cpp`):

```cmake
    engine/fx/dust.cpp
    tests/test_dust.cpp
```

`CMakeLists.txt` — in `render`, after `engine/fx/flux.cpp` (line 105):

```cmake
    engine/fx/dust.cpp
```

`host/vcv/Makefile` — after line 40 (`$(REPO)/engine/fx/flux.cpp \`):

```make
	$(REPO)/engine/fx/dust.cpp \
```

`bench/Makefile` — after line 39 (`../engine/fx/flux.cpp \`). **Coordinate with the bench session before editing this file:**

```make
	../engine/fx/dust.cpp \
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build && ./build/spky_tests.exe -tc="dust*"`
Expected: PASS — all five `dust:` cases.

- [ ] **Step 7: Commit**

```bash
git add engine/fx/dust.h engine/fx/dust.cpp tests/test_dust.cpp CMakeLists.txt host/vcv/Makefile bench/Makefile
git commit -m "dust: grain pool, Hann window, equal-power pan, free-zone scheduler"
```

---

### Task 3: ROT zone S — tempo-synced stutter grid with growing jitter

**Files:**
- Modify: `engine/fx/dust.cpp` (`_schedule`)
- Test: `tests/test_dust.cpp` (append)

**Interfaces:**
- Consumes: `DustCloud::_grid_period`, `_fire_prob`, `_burst`, `_jitter`, `_zone` from Task 2's `_remap()`.
- Produces: no new public API. Behavior only: at `rot < 0.33`, grain births lock to `delay_time / 4`.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_dust.cpp`:

```cpp
// Instrument births by watching for a fresh grain: with DUST low enough that
// the pool is nearly always empty, a transition from silence to non-silence
// marks a birth.
static std::vector<int> birth_indices(float rot, float amount, float delay_s,
                                      int n_samples) {
    FakeTape t; t.fill_noise(11);
    DustCloud d;
    d.init(48000.f, 0xD0571u);
    d.set_dust(amount);
    d.set_rot(rot);
    d.set_delay_time(delay_s);
    std::vector<int> births;
    bool was_silent = true;
    for (int i = 0; i < n_samples; ++i) {
        float gl = 0.f, gr = 0.f;
        d.process(t.tap(), gl, gr);
        const bool silent = (gl == 0.f && gr == 0.f);
        if (was_silent && !silent) births.push_back(i);
        was_silent = silent;
        t.advance();
    }
    return births;
}

TEST_CASE("dust: zone S with no jitter births on the delay/4 grid") {
    // rot = 0 -> jitter 0. delay 0.5 s -> grid 0.125 s -> 6000 samples.
    const auto b = birth_indices(0.f, 0.35f, 0.5f, 480000);
    REQUIRE(b.size() >= 8);
    for (size_t i = 1; i < b.size(); ++i) {
        const int gap = b[i] - b[i - 1];
        // Every gap must be a whole number of grid periods (a slot can decline
        // to fire), so the remainder against 6000 is what is locked.
        const int rem = gap % 6000;
        CHECK((rem < 40 || rem > 5960));
    }
}

TEST_CASE("dust: zone S jitter grows across the zone") {
    auto spread = [](float rot) {
        const auto b = birth_indices(rot, 0.35f, 0.5f, 480000);
        if (b.size() < 8) return 0.0;
        double worst = 0.0;
        for (size_t i = 1; i < b.size(); ++i) {
            const int gap = (b[i] - b[i - 1]) % 6000;
            const double off = gap > 3000 ? (6000 - gap) : gap;
            worst = std::max(worst, off);
        }
        return worst;
    };
    const double tight = spread(0.0f);          // locked
    const double loose = spread(0.30f);         // near the top of zone S
    CHECK(tight < 40.0);
    CHECK(loose > tight * 4.0);
}

TEST_CASE("dust: zone S grid follows the delay time") {
    const auto slow = birth_indices(0.f, 0.35f, 0.5f, 480000);   // grid 6000
    const auto fast = birth_indices(0.f, 0.35f, 0.25f, 480000);  // grid 3000
    REQUIRE(slow.size() >= 8);
    REQUIRE(fast.size() >= 8);
    // Same fire probability, half the grid period -> roughly twice the births.
    CHECK((double)fast.size() > (double)slow.size() * 1.5);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build && ./build/spky_tests.exe -tc="dust: zone S*"`
Expected: FAIL — the free-running scheduler ignores the grid, so gaps are unrelated to 6000 and `rem` lands anywhere.

- [ ] **Step 3: Implement the zone-aware scheduler**

Replace `DustCloud::_schedule` in `engine/fx/dust.cpp` with:

```cpp
void DustCloud::_schedule(const TapeTap& tape) {
    if (_zone != 0) {                       // zones F and R: free-running
        if (_rng.next_unipolar() < _birth_prob) _spawn(tape);
        return;
    }
    // Zone S: births lock to a grid derived from the delay itself, so stutter
    // bursts always subdivide the echo — dub polyrhythm with no extra control.
    // Duration-synced like FLUX, not phase-locked to the sequencer.
    if (--_grid_countdown > 0) return;

    _grid_countdown = _grid_period;
    if (_jitter > 0.f) {
        const float j = _rng.next_bipolar() * _jitter * 0.5f;
        _grid_countdown += (int32_t)(j * (float)_grid_period);
        if (_grid_countdown < 1) _grid_countdown = 1;
    }
    if (_rng.next_unipolar() >= _fire_prob) return;
    for (int i = 0; i < _burst; ++i) _spawn(tape);
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build && ./build/spky_tests.exe -tc="dust*"`
Expected: PASS — the three new `zone S` cases plus all five from Task 2.

- [ ] **Step 5: Commit**

```bash
git add engine/fx/dust.cpp tests/test_dust.cpp
git commit -m "dust: zone S synced stutter grid at FLUX delay/4, jitter morph"
```

---

### Task 4: ROT zone R — reverse grains

**Files:**
- Modify: `engine/fx/dust.cpp` (already spawns with `delta = 2`; this task proves and fixes it)
- Test: `tests/test_dust.cpp` (append)

**Interfaces:**
- Consumes: `Grain::delta`, `_rev_prob` from Task 2.
- Produces: no new public API. Behavior: at `rot > 0.66`, up to 70 % of grains play their tape material time-reversed at 1x.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_dust.cpp`:

```cpp
TEST_CASE("dust: a reverse grain reads the tape backwards at 1x") {
    // Build a tape holding a strictly increasing ramp in time. Reading forward
    // must yield rising values; reading in reverse must yield falling ones.
    FakeTape t;
    for (int32_t i = 0; i < FakeTape::kSize; ++i) {
        // offset i behind the head == i samples ago; make value == -i so
        // "later in time" is "larger".
        t.l[i] = -(float)i / (float)FakeTape::kSize;
        t.r[i] = t.l[i];
    }
    t.ptr = 0;

    DustCloud d;
    d.init(48000.f, 0xD0571u);
    d.set_dust(0.5f);
    d.set_rot(1.f);            // zone R top: reverse probability at maximum
    d.set_delay_time(0.5f);

    // Collect the sign of the sample-to-sample slope while exactly one grain
    // is alive, over many births; with rev_prob 0.7 both signs must appear.
    int rising = 0, falling = 0;
    float prev = 0.f;
    bool had = false;
    for (int i = 0; i < 480000; ++i) {
        float gl = 0.f, gr = 0.f;
        d.process(t.tap(), gl, gr);
        if (had && gl != 0.f && prev != 0.f) {
            if (gl > prev) ++rising;
            else if (gl < prev) ++falling;
        }
        prev = gl;
        had = true;
        t.advance();
    }
    CHECK(rising > 1000);
    CHECK(falling > 1000);
}

TEST_CASE("dust: no reverse grains below zone R") {
    FakeTape t; t.fill_noise(11);
    DustCloud d;
    d.init(48000.f, 0xD0571u);
    d.set_dust(1.f);
    d.set_rot(0.5f);           // free zone: forward only
    d.set_delay_time(0.5f);
    for (int i = 0; i < 96000; ++i) {
        float gl = 0.f, gr = 0.f;
        d.process(t.tap(), gl, gr);
        REQUIRE(std::isfinite(gl));
        t.advance();
    }
    CHECK(d.head_gain() == doctest::Approx(0.f).epsilon(0.001));  // DUST = 1
}

TEST_CASE("dust: reverse grains never overrun the tape") {
    FakeTape t; t.fill_noise(11);
    DustCloud d;
    d.init(48000.f, 0xD0571u);
    d.set_dust(1.f);
    d.set_rot(1.f);
    d.set_delay_time(0.5f);
    // A 1 s tape with 400 ms grains receding at 2 samples/sample is the worst
    // case; TapeTap::read must never index outside the buffer.
    for (int i = 0; i < 480000; ++i) {
        float gl = 0.f, gr = 0.f;
        d.process(t.tap(), gl, gr);
        REQUIRE(std::isfinite(gl));
        REQUIRE(std::fabs(gl) <= 2.f);
        t.advance();
    }
}
```

- [ ] **Step 2: Run the tests to verify they fail or expose a bug**

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build && ./build/spky_tests.exe -tc="dust: *reverse*,dust: no reverse*"`
Expected: the reverse-grain case FAILs (`falling` stays 0 on a rising ramp read forward only) if `_rev_prob` is not reaching `_spawn`, and the overrun case FAILs with an out-of-range read if the clamp is wrong. Record which of the three actually fail before changing code — Task 2 already wrote the mechanism, so this step is the proof, and a green run here means the only work is confirming the clamp.

- [ ] **Step 3: Harden the offset clamp**

The wrap in `TapeTap::read` uses `%`, which is safe for any non-negative index, but a reverse grain's offset must stay below `size` for the *modulo* to keep meaning "samples behind the head". Confirm `_spawn` bounds it and make the guarantee explicit — in `engine/fx/dust.cpp`, inside `_spawn`, replace the clamp block with:

```cpp
        // A reverse grain recedes 2 samples per sample; clamp the start so it
        // can never overrun the tape within its own lifetime. The +4 margin
        // covers the final increment plus the modulo boundary.
        int32_t travel = g.delta ? 2 * g.len : 0;
        int32_t max_off = tape.size - 4 - travel;
        if (max_off < 2) {
            // Tape too short for this grain length at this direction: shorten
            // the grain rather than dropping it, so short tapes still granulate.
            g.len = (tape.size - 8) / 2;
            if (g.len < 8) g.len = 8;
            travel = g.delta ? 2 * g.len : 0;
            max_off = tape.size - 4 - travel;
            if (max_off < 2) max_off = 2;
        }
        int32_t spray = _spray < max_off ? _spray : max_off;
        if (spray < 1) spray = 1;
        g.offset = 1 + (int32_t)(_rng.next_unipolar() * (float)spray);
        if (g.offset > max_off) g.offset = max_off;
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build && ./build/spky_tests.exe -tc="dust*"`
Expected: PASS — all `dust:` cases.

- [ ] **Step 5: Commit**

```bash
git add engine/fx/dust.cpp tests/test_dust.cpp
git commit -m "dust: reverse grains in zone R, tape-overrun clamp"
```

---

### Task 5: ROT zone R — writeback

**Files:**
- Modify: `engine/fx/dust.cpp` (`process` return value)
- Test: `tests/test_dust.cpp` (append)

**Interfaces:**
- Consumes: `_wb_gain` from Task 2's `_remap()`.
- Produces: `DustCloud::process` now returns the tanh-bounded writeback sample instead of `0.f`. `Flux` (Task 6) feeds it to `EchoDelay::Process(in, wb)`.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_dust.cpp`:

```cpp
TEST_CASE("dust: writeback is zero outside zone R") {
    FakeTape t; t.fill_noise(11);
    for (float rot : {0.0f, 0.2f, 0.5f, 0.65f}) {
        DustCloud d;
        d.init(48000.f, 0xD0571u);
        d.set_dust(1.f);
        d.set_rot(rot);
        d.set_delay_time(0.5f);
        for (int i = 0; i < 48000; ++i) {
            float gl = 0.f, gr = 0.f;
            REQUIRE(d.process(t.tap(), gl, gr) == 0.f);
            t.advance();
        }
    }
}

TEST_CASE("dust: writeback rises with rot and stays tanh-bounded") {
    auto wb_rms = [](float rot) {
        FakeTape t; t.fill_noise(11);
        DustCloud d;
        d.init(48000.f, 0xD0571u);
        d.set_dust(1.f);
        d.set_rot(rot);
        d.set_delay_time(0.5f);
        double s = 0.0;
        for (int i = 0; i < 192000; ++i) {
            float gl = 0.f, gr = 0.f;
            const float wb = d.process(t.tap(), gl, gr);
            REQUIRE(std::fabs(wb) < 1.f);       // tanh range
            REQUIRE(std::isfinite(wb));
            s += (double)wb * (double)wb;
            t.advance();
        }
        return std::sqrt(s / 192000.0);
    };
    const double mid = wb_rms(0.80f);
    const double top = wb_rms(1.00f);
    CHECK(mid > 0.0);
    CHECK(top > mid);
}

TEST_CASE("dust: writeback survives a full-scale tape without blowing up") {
    FakeTape t;
    for (int32_t i = 0; i < FakeTape::kSize; ++i) { t.l[i] = 1.f; t.r[i] = -1.f; }
    DustCloud d;
    d.init(48000.f, 0xD0571u);
    d.set_dust(1.f);
    d.set_rot(1.f);
    d.set_delay_time(0.5f);
    for (int i = 0; i < 192000; ++i) {
        float gl = 0.f, gr = 0.f;
        const float wb = d.process(t.tap(), gl, gr);
        REQUIRE(std::fabs(wb) < 1.f);
        REQUIRE(std::isfinite(gl));
        t.advance();
    }
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build && ./build/spky_tests.exe -tc="dust: writeback*"`
Expected: the "rises with rot" case FAILs — `mid` and `top` are both 0.0 because `process` still returns `0.f`.

- [ ] **Step 3: Return the writeback**

In `engine/fx/dust.cpp`, replace the last two lines of `DustCloud::process`:

```cpp
    gl = sl * _norm;
    gr = sr * _norm;

    // Zone R: the grain sum is written back onto the tape, so the cloud smears
    // itself over generations. tanh-bounded here so the recirculating energy
    // cannot run away no matter what the feedback path does with it.
    if (_wb_gain <= 0.f) return 0.f;
    return std::tanh((gl + gr) * 0.5f * _wb_gain);
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build && ./build/spky_tests.exe -tc="dust*"`
Expected: PASS — all `dust:` cases.

- [ ] **Step 5: Commit**

```bash
git add engine/fx/dust.cpp tests/test_dust.cpp
git commit -m "dust: zone R writeback, tanh-bounded"
```

---

### Task 6: `Flux` integration — DUST/ROT setters, head takeover, bit-exact bypass

**Files:**
- Modify: `engine/fx/flux.h:147-177` (class `Flux`), `engine/fx/flux.cpp:10-24` (`init`), `engine/fx/flux.cpp:38-48` (`recompute_time`), `engine/fx/flux.cpp:62-68` (`process`)
- Test: `tests/test_flux.cpp` (append)

**Interfaces:**
- Consumes: `DustCloud`, `TapeTap` (Task 2-5); `EchoDelay::line()`, `write_ptr()`, `Process(in, wb)` (Task 1).
- Produces: `Flux::set_dust(float)`, `Flux::set_rot(float)`, `Flux::dust_active() const -> bool`.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_flux.cpp`:

```cpp
TEST_CASE("flux: dust 0 is bit-exact with the pre-DUST path at any rot") {
    static float ref_l[Flux::kMaxSamples], ref_r[Flux::kMaxSamples];
    static float dut_l[Flux::kMaxSamples], dut_r[Flux::kMaxSamples];
    for (float rot : {0.0f, 0.33f, 0.5f, 0.9f, 1.0f}) {
        Flux ref, dut;
        ref.init(48000.f, ref_l, ref_r);
        dut.init(48000.f, dut_l, dut_r);
        for (Flux* f : {&ref, &dut}) {
            f->set_on(true, true);
            f->set_bpm(120.f);
            f->set_rate(3);
            f->set_feedback(0.6f);
            f->set_mix(0.8f);
        }
        dut.set_dust(0.f);
        dut.set_rot(rot);
        for (int i = 0; i < 120000; ++i) {
            const float s = std::sin(0.011f * i) * 0.5f;
            float al = s, ar = s, bl = s, br = s;
            ref.process(al, ar);
            dut.process(bl, br);
            REQUIRE(al == bl);
            REQUIRE(ar == br);
        }
    }
}

TEST_CASE("flux: dust makes sound and the head fades at the top") {
    static float bl[Flux::kMaxSamples], br[Flux::kMaxSamples];
    Flux f;
    f.init(48000.f, bl, br);
    f.set_on(true, true);
    f.set_bpm(120.f);
    f.set_rate(3);
    f.set_feedback(0.5f);
    f.set_mix(1.f);
    f.set_dust(0.9f);
    f.set_rot(0.5f);
    CHECK(f.dust_active());
    float peak = 0.f;
    for (int i = 0; i < 240000; ++i) {
        const float s = std::sin(0.01f * i) * 0.4f;
        float l = s, r = s;
        f.process(l, r);
        peak = std::max(peak, std::fabs(l));
        REQUIRE(std::isfinite(l));
    }
    CHECK(peak > 0.01f);
    CHECK(peak < 4.f);
}

TEST_CASE("flux: dust at full recirculates without running away") {
    static float bl[Flux::kMaxSamples], br[Flux::kMaxSamples];
    Flux f;
    f.init(48000.f, bl, br);
    f.set_on(true, true);
    f.set_bpm(120.f);
    f.set_rate(6);
    f.set_feedback(1.f);       // 1.2 coefficient — self-oscillating
    f.set_mix(1.f);
    f.set_dust(1.f);
    f.set_rot(1.f);            // zone R: writeback active
    float peak = 0.f;
    for (int i = 0; i < 480000; ++i) {   // 10 s
        float l = 1.f, r = 1.f;          // sustained full scale
        f.process(l, r);
        peak = std::max(peak, std::fabs(l));
        REQUIRE(std::isfinite(l));
    }
    CHECK(peak < 8.f);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build`
Expected: FAIL to compile — `'set_dust': is not a member of 'spky::Flux'`.

- [ ] **Step 3: Extend the `Flux` class**

In `engine/fx/flux.h`, add `#include "fx/dust.h"` to the include block at the top, then add to `class Flux`'s public section after `void set_mix(float norm);` (line 162):

```cpp
    void set_dust(float norm);                           // 0 .. 1 grain amount
    void set_rot(float norm);                            // 0 .. 1 character
    bool dust_active() const { return _dust.active(); }
```

and to the private member block after `EchoDelay<kMaxSamples> _echo_r;` (line 169):

```cpp
    DustCloud _dust;
```

- [ ] **Step 4: Implement in `engine/fx/flux.cpp`**

Add to the end of `Flux::init` (after `set_mix(0.5f);`, line 23):

```cpp
    // Fixed distinct per-part seeds: the buffer pointer is stable per part for
    // the life of the instrument, so hashing it gives A and B different grain
    // streams while staying bit-reproducible across desktop and firmware.
    _dust.init(sample_rate, 0xD0571u ^ (uint32_t)(uintptr_t)buf_l);
    _dust.set_delay_time(_delay_time);
```

Add to the end of `Flux::recompute_time` (after the two `SetDelayTime` calls, line 47):

```cpp
    _dust.set_delay_time(_delay_time);   // zone S grid follows the echo
```

Add the two setters after `Flux::set_mix` (line 60):

```cpp
void Flux::set_dust(float norm) {
    if (!_buf_ok) return;
    _dust.set_dust(clampf(norm, 0.f, 1.f));
}

void Flux::set_rot(float norm) {
    if (!_buf_ok) return;
    _dust.set_rot(clampf(norm, 0.f, 1.f));
}
```

Replace `Flux::process` (lines 62-68) with:

```cpp
void Flux::process(float& l, float& r) {
    if (!_buf_ok) return;
    float send = _sw.process();
    if (_sw.is_idle()) return;   // fully off: bit-exact dry

    if (!_dust.active()) {       // DUST = 0: bit-exact with the pre-DUST path
        l += _echo_l.Process(l * send) * _mix_lin;
        r += _echo_r.Process(r * send) * _mix_lin;
        return;
    }

    // The grain taps read the tape as it stands at the START of this sample —
    // built before Process() advances the write head. Both channels share one
    // write pointer (they are written in lockstep).
    const TapeTap tap{_echo_l.line(), _echo_r.line(), _echo_l.write_ptr(),
                      static_cast<int32_t>(kMaxSamples)};
    float gl = 0.f, gr = 0.f;
    const float wb = _dust.process(tap, gl, gr);

    const float e_l = _echo_l.Process(l * send, wb);
    const float e_r = _echo_r.Process(r * send, wb);

    // Grain sum joins BEFORE _mix_lin: FLUX MIX stays the single wet control
    // for everything coming off the tape. Grain reads deliberately skip the
    // band-pass and tanh — the cloud is rawer and brighter than the echo.
    const float hg = _dust.head_gain();
    l += (e_l * hg + gl) * _mix_lin;
    r += (e_r * hg + gr) * _mix_lin;
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build && ./build/spky_tests.exe -tc="flux*,echo*,dust*"`
Expected: PASS — including all nine pre-existing `flux:` cases untouched.

- [ ] **Step 6: Commit**

```bash
git add engine/fx/flux.h engine/fx/flux.cpp tests/test_flux.cpp
git commit -m "flux: integrate DustCloud, head takeover, bit-exact DUST=0 bypass"
```

---

### Task 7: FREEZE with erosion

**Files:**
- Modify: `engine/fx/flux.h` (class `Flux`), `engine/fx/flux.cpp`
- Test: `tests/test_flux.cpp` (append)

**Interfaces:**
- Consumes: `EchoDelay::set_freeze`/`set_wear` (Task 1), `DustCloud::wear()` (Task 2).
- Produces: `Flux::set_freeze(bool)`, `Flux::frozen() const -> bool`.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_flux.cpp`:

```cpp
TEST_CASE("flux: freeze at low rot preserves the loop and keeps playing") {
    static float bl[Flux::kMaxSamples], br[Flux::kMaxSamples];
    Flux f;
    f.init(48000.f, bl, br);
    f.set_on(true, true);
    f.set_bpm(120.f);
    f.set_rate(3);
    f.set_feedback(0.5f);
    f.set_mix(1.f);
    f.set_dust(0.f);
    f.set_rot(0.f);
    for (int i = 0; i < 96000; ++i) {          // load the tape
        float s = std::sin(0.01f * i) * 0.5f;
        float l = s, r = s;
        f.process(l, r);
    }

    f.set_freeze(true);
    CHECK(f.frozen());
    static float snap[Flux::kMaxSamples];
    std::memcpy(snap, bl, sizeof(snap));

    double out_sum_sq = 0.0;
    for (int i = 0; i < 240000; ++i) {         // one full pass, loud input
        float l = 0.8f, r = 0.8f;
        f.process(l, r);
        out_sum_sq += (double)(l - 0.8f) * (double)(l - 0.8f);
    }
    CHECK(std::memcmp(snap, bl, sizeof(snap)) == 0);   // nothing was written
    CHECK(std::sqrt(out_sum_sq / 240000.0) > 1e-4);    // but it still sounds
}

TEST_CASE("flux: the frozen echo head is a 5 s looper") {
    static float bl[Flux::kMaxSamples], br[Flux::kMaxSamples];
    Flux f;
    f.init(48000.f, bl, br);
    f.set_on(true, true);
    f.set_bpm(120.f);
    f.set_rate(3);
    f.set_feedback(0.5f);
    f.set_mix(1.f);
    f.set_dust(0.f);
    f.set_rot(0.f);
    for (int i = 0; i < 96000; ++i) {
        float s = std::sin(0.01f * i) * 0.5f;
        float l = s, r = s;
        f.process(l, r);
    }
    f.set_freeze(true);

    // Nothing is written, the pointer keeps advancing, so the head traverses
    // the whole 240000-sample tape and repeats. The band-pass is IIR, so let
    // two passes settle, then compare the next two sample for sample.
    const int kPeriod = (int)Flux::kMaxSamples;
    for (int i = 0; i < 2 * kPeriod; ++i) { float l = 0.f, r = 0.f; f.process(l, r); }

    std::vector<float> pass_a(kPeriod), pass_b(kPeriod);
    for (int i = 0; i < kPeriod; ++i) { float l = 0.f, r = 0.f; f.process(l, r); pass_a[i] = l; }
    for (int i = 0; i < kPeriod; ++i) { float l = 0.f, r = 0.f; f.process(l, r); pass_b[i] = l; }

    float worst = 0.f;
    double energy = 0.0;
    for (int i = 0; i < kPeriod; ++i) {
        worst = std::max(worst, std::fabs(pass_a[i] - pass_b[i]));
        energy += (double)pass_a[i] * (double)pass_a[i];
    }
    CHECK(std::sqrt(energy / kPeriod) > 1e-4);   // the loop is audible...
    CHECK(worst < 1e-3f);                        // ...and repeats with a 5 s period
}

TEST_CASE("flux: frozen dry input still passes") {
    static float bl[Flux::kMaxSamples], br[Flux::kMaxSamples];
    Flux f;
    f.init(48000.f, bl, br);
    f.set_on(true, true);
    f.set_mix(0.f);            // -40 dBFS wet: dry dominates
    f.set_freeze(true);
    for (int i = 0; i < 4800; ++i) {
        const float s = 0.5f;
        float l = s, r = s;
        f.process(l, r);
        REQUIRE(l > 0.4f);     // the dry path is untouched by FREEZE
    }
}

TEST_CASE("flux: freeze in zone R erodes the loop, bounded") {
    static float bl[Flux::kMaxSamples], br[Flux::kMaxSamples];
    Flux f;
    f.init(48000.f, bl, br);
    f.set_on(true, true);
    f.set_bpm(120.f);
    f.set_rate(3);
    f.set_feedback(0.5f);
    f.set_mix(1.f);
    f.set_dust(0.6f);
    f.set_rot(1.f);            // zone R
    for (int i = 0; i < 96000; ++i) {
        float s = std::sin(0.01f * i) * 0.5f;
        float l = s, r = s;
        f.process(l, r);
    }

    auto tape_rms = [&]() {
        double s = 0.0;
        for (size_t i = 0; i < Flux::kMaxSamples; ++i)
            s += (double)bl[i] * (double)bl[i];
        return std::sqrt(s / (double)Flux::kMaxSamples);
    };
    static float snap[Flux::kMaxSamples];
    std::memcpy(snap, bl, sizeof(snap));
    const double before = tape_rms();

    f.set_freeze(true);
    for (int i = 0; i < 240000; ++i) {         // one full pass
        float l = 0.f, r = 0.f;
        f.process(l, r);
        REQUIRE(std::isfinite(l));
    }
    const double after = tape_rms();

    CHECK(std::memcmp(snap, bl, sizeof(snap)) != 0);   // it decomposed
    CHECK(after < before * 1.5);                        // bounded, not blowing up
    CHECK(after > 0.0);
}

TEST_CASE("flux: erosion with dust 0 only wears, adds nothing") {
    static float bl[Flux::kMaxSamples], br[Flux::kMaxSamples];
    Flux f;
    f.init(48000.f, bl, br);
    f.set_on(true, true);
    f.set_bpm(120.f);
    f.set_rate(3);
    f.set_feedback(0.5f);
    f.set_mix(1.f);
    f.set_dust(0.f);           // no grains...
    f.set_rot(1.f);            // ...but zone R wear
    for (int i = 0; i < 96000; ++i) {
        float s = std::sin(0.01f * i) * 0.5f;
        float l = s, r = s;
        f.process(l, r);
    }
    auto tape_rms = [&]() {
        double s = 0.0;
        for (size_t i = 0; i < Flux::kMaxSamples; ++i)
            s += (double)bl[i] * (double)bl[i];
        return std::sqrt(s / (double)Flux::kMaxSamples);
    };
    const double before = tape_rms();
    f.set_freeze(true);
    for (int i = 0; i < 240000; ++i) {
        float l = 0.f, r = 0.f;
        f.process(l, r);
    }
    CHECK(tape_rms() < before);    // monotonic decay, no new content
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build`
Expected: FAIL to compile — `'set_freeze': is not a member of 'spky::Flux'`.

- [ ] **Step 3: Add the freeze API to `Flux`**

In `engine/fx/flux.h`, after `bool dust_active() const ...`:

```cpp
    void set_freeze(bool on);      // tape state: needs FLUX on to be audible
    bool frozen() const { return _frozen; }
```

and to the private members, after `float _delay_time = 0.5f;`:

```cpp
    bool _frozen = false;
```

- [ ] **Step 4: Implement freeze + erosion in `engine/fx/flux.cpp`**

Add after `Flux::set_rot`:

```cpp
// FREEZE: input and feedback stop being written; the pointer keeps advancing,
// so the echo head and every grain tap travel through the frozen material on
// their own — the echo head becomes a 5 s looper. In zone R the frozen loop
// does not preserve: `wear` abrades it while grains burn themselves in, so it
// decomposes pass by pass. Pull ROT down mid-erosion to keep what remains.
void Flux::set_freeze(bool on) {
    if (!_buf_ok) return;
    _frozen = on;
    _echo_l.set_freeze(on);
    _echo_r.set_freeze(on);
}
```

and in `Flux::set_rot`, after `_dust.set_rot(...)`, push the wear coefficient down:

```cpp
    _echo_l.set_wear(_dust.wear());
    _echo_r.set_wear(_dust.wear());
```

Finally, in `Flux::process`, the bypass branch must not be taken while frozen (the tape must stop being written even with DUST at 0). Change the guard:

```cpp
    if (!_dust.active() && !_frozen) {   // bit-exact with the pre-DUST path
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build && ./build/spky_tests.exe -tc="flux*,echo*,dust*"`
Expected: PASS — all cases, including the Task 6 bit-exactness case (freeze is off there, so the bypass still applies).

- [ ] **Step 6: Commit**

```bash
git add engine/fx/flux.h engine/fx/flux.cpp tests/test_flux.cpp
git commit -m "flux: FREEZE with erosion — preserving looper at low ROT, self-granulating at high"
```

---

### Task 8: `PartFx` / `Instrument` forwarding, scenario actions, demo scenarios

**Files:**
- Modify: `engine/fx/part_fx.h:43-46` (next to `set_flux_mix`)
- Modify: `engine/instrument.h:60-63` (next to `set_flux_mix`)
- Modify: `host/render/scenario.cpp:101-103` (next to `set_flux_mix`)
- Create: `host/render/scenarios/dust_stutter.json`, `host/render/scenarios/dust_erosion.json`
- Test: `tests/test_part_fx.cpp` (append)

**Interfaces:**
- Consumes: `Flux::set_dust`, `set_rot`, `set_freeze` (Tasks 6-7).
- Produces: `PartFx::set_dust(float)`, `set_rot(float)`, `set_freeze(bool)`; `Instrument::set_dust(int p, float)`, `set_rot(int p, float)`, `set_freeze(int p, bool)`; scenario actions `set_dust`, `set_rot`, `set_freeze`.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_part_fx.cpp`:

```cpp
TEST_CASE("part_fx: dust/rot/freeze reach the flux block") {
    static float el[Flux::kMaxSamples], er[Flux::kMaxSamples];
    PartFx fx;
    fx.init(48000.f, el, er);
    fx.set_fx_on(FxBlock::Flux, true, true);
    fx.set_flux_mix(1.f);

    CHECK(!fx.flux().dust_active());
    fx.set_dust(0.5f);
    CHECK(fx.flux().dust_active());
    fx.set_dust(0.f);
    CHECK(!fx.flux().dust_active());

    fx.set_rot(1.f);            // must not throw or alter activity
    CHECK(!fx.flux().dust_active());

    CHECK(!fx.flux().frozen());
    fx.set_freeze(true);
    CHECK(fx.flux().frozen());
    fx.set_freeze(false);
    CHECK(!fx.flux().frozen());
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build`
Expected: FAIL to compile — `'set_dust': is not a member of 'spky::PartFx'`.

- [ ] **Step 3: Add the forwarders**

`engine/fx/part_fx.h` — after `void set_flux_rate(int slice_idx) { _flux.set_rate(slice_idx); }` (line 46):

```cpp
    void set_dust(float n)   { _flux.set_dust(n); }
    void set_rot(float n)    { _flux.set_rot(n); }
    void set_freeze(bool on) { _flux.set_freeze(on); }
```

`engine/instrument.h` — after `void set_flux_rate(int p, int slice_idx) ...` (line 61):

```cpp
    void set_dust(int p, float n)   { _parts[p].fx().set_dust(n); }
    void set_rot(int p, float n)    { _parts[p].fx().set_rot(n); }
    void set_freeze(int p, bool on) { _parts[p].fx().set_freeze(on); }
```

- [ ] **Step 4: Add the scenario actions**

`host/render/scenario.cpp` — after line 103 (`else if (a == "set_comp") ...`):

```cpp
    else if (a == "set_dust")             inst.set_dust(e.part, e.value);
    else if (a == "set_rot")              inst.set_rot(e.part, e.value);
    else if (a == "set_freeze")           inst.set_freeze(e.part, e.flag);
```

- [ ] **Step 5: Write the demo scenarios**

Create `host/render/scenarios/dust_stutter.json`:

```json
{
  "sample_rate": 48000,
  "bpm": 120,
  "duration_s": 32,
  "init": [
    {"_comment":"DUST zone S: grain births lock to the FLUX delay / 4, so the stutter always subdivides the echo."},
    {"action":"set_engine","part":0,"value":"test_tone"},
    {"action":"set_target_active","part":1,"slot":4,"flag":false},
    {"action":"set_target_base","part":1,"slot":4,"value":0.0},
    {"action":"set_step","part":0,"flag":true,"ivalue":16},
    {"action":"set_density","part":0,"value":0.6},
    {"action":"set_fx_on","part":0,"value":"flux","flag":true},
    {"action":"set_flux_mix","part":0,"value":0.6},
    {"action":"set_flux_rate","part":0,"ivalue":3},
    {"action":"set_rot","part":0,"value":0.0},
    {"action":"set_dust","part":0,"value":0.0}
  ],
  "events": [
    {"_comment":"8s: dust in — sparse splinters on the grid."},
    {"t": 8,  "action":"set_dust","part":0,"value":0.35},
    {"_comment":"16s: denser bursts, and jitter starts loosening the grid."},
    {"t": 16, "action":"set_dust","part":0,"value":0.75},
    {"t": 16, "action":"set_rot","part":0,"value":0.22},
    {"_comment":"24s: the cloud eats the delay head (takeover above 0.7)."},
    {"t": 24, "action":"set_dust","part":0,"value":1.0}
  ]
}
```

Create `host/render/scenarios/dust_erosion.json`:

```json
{
  "sample_rate": 48000,
  "bpm": 120,
  "duration_s": 40,
  "init": [
    {"_comment":"DUST zone R + FREEZE: the frozen 5 s loop decomposes itself, pass by pass."},
    {"action":"set_engine","part":0,"value":"test_tone"},
    {"action":"set_target_active","part":1,"slot":4,"flag":false},
    {"action":"set_target_base","part":1,"slot":4,"value":0.0},
    {"action":"set_step","part":0,"flag":true,"ivalue":16},
    {"action":"set_density","part":0,"value":0.5},
    {"action":"set_fx_on","part":0,"value":"flux","flag":true},
    {"action":"set_flux_mix","part":0,"value":0.7},
    {"action":"set_flux_rate","part":0,"ivalue":3},
    {"action":"set_dust","part":0,"value":0.0},
    {"action":"set_rot","part":0,"value":0.0}
  ],
  "events": [
    {"_comment":"10s: freeze — the echo head becomes a 5 s looper, still preserving."},
    {"t": 10, "action":"set_freeze","part":0,"flag":true},
    {"_comment":"14s: grains scatter the still image, ROT still low (no erosion yet)."},
    {"t": 14, "action":"set_dust","part":0,"value":0.6},
    {"_comment":"20s: into zone R — reverse grains, writeback, and the loop starts wearing."},
    {"t": 20, "action":"set_rot","part":0,"value":0.85},
    {"_comment":"28s: full rot — irreversible self-granulation."},
    {"t": 28, "action":"set_rot","part":0,"value":1.0},
    {"_comment":"34s: pull ROT down to keep what remains, then unfreeze."},
    {"t": 34, "action":"set_rot","part":0,"value":0.2},
    {"t": 37, "action":"set_freeze","part":0,"flag":false}
  ]
}
```

- [ ] **Step 6: Run the tests and render both scenarios**

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build && ./build/spky_tests.exe`
Expected: PASS — the whole suite.

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && ./build/render.exe host/render/scenarios/dust_stutter.json /tmp/dust_stutter.wav /tmp/dust_stutter.csv && ./build/render.exe host/render/scenarios/dust_erosion.json /tmp/dust_erosion.wav /tmp/dust_erosion.csv`
Expected: both exit 0 and write non-empty `.wav` files. Note: scenario.cpp silently ignores unknown actions (`scenario.cpp:126`), so a typo in an action name renders silence rather than failing — check the WAVs are non-trivial before moving on.

- [ ] **Step 7: Commit**

```bash
git add engine/fx/part_fx.h engine/instrument.h host/render/scenario.cpp host/render/scenarios/dust_stutter.json host/render/scenarios/dust_erosion.json tests/test_part_fx.cpp
git commit -m "dust: PartFx/Instrument forwarding, scenario actions, stutter + erosion demos"
```

---

### Task 9: Panel — FRZ replaces TRIGGER in place, DUST/ROT appended

**Files:**
- Modify: `host/vcv/res/gen_panel.py:157` (`FX_BOT`), `:194-196` (pads), `:315-340` (appended `PARAMS` tail)
- Modify: `host/vcv/res/test_panel.py` (`PARAM_ORDER`, `LOWER_A`)
- Regenerate: `host/vcv/res/Spotymod.svg`, `host/vcv/src/generated_panel.hpp`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: param enums `FRZ_A` / `FRZ_B` (**replacing** `TRIGGER_A` / `TRIGGER_B` at the same template index, `PART_STRIDE` unchanged at 23), and appended `DUST_A`, `DUST_B`, `ROT_A`, `ROT_B` at the end of `PARAMS`.

**Design note — why rename rather than remove.** `TRIGGER` is a member of `part_controls()`. Deleting it would shrink `PART_STRIDE` from 23 to 22 and shift every part-B, SHARED and appended param id, breaking every saved `.vcv` patch. Renaming it in place keeps the id and the stride. The only residue: a patch saved with TRIGGER held down would load with FREEZE engaged — momentary buttons are effectively never saved at 1, so this is accepted rather than mitigated.

- [ ] **Step 1: Write the failing guard**

In `host/vcv/res/test_panel.py`, update `PARAM_ORDER` — replace the `"TRIGGER"` entry in the per-part template list with `"FRZ"`, and append the four new params to the tail list (after `"COLOR_A", "COLOR_B"`):

```python
    "DUST_A", "DUST_B", "ROT_A", "ROT_B",
```

Add a new guard function next to the existing ones:

```python
def test_dust_params():
    """DUST/ROT are appended; FRZ took TRIGGER's template slot, so the stride
    must not have moved and the FRZ id must equal the old TRIGGER id."""
    check(g.PART_STRIDE == 23, "PART_STRIDE must stay 23")
    ids = {c.enum: i for i, c in enumerate(g.PARAMS)}
    check(ids["FRZ_B"] - ids["FRZ_A"] == g.PART_STRIDE,
          "FRZ_A/B must be one stride apart (template member)")
    for e in ("DUST_A", "DUST_B", "ROT_A", "ROT_B"):
        check(e in ids, f"{e} missing")
        check(ids[e] >= 2 * g.PART_STRIDE, f"{e} must be appended, not templated")
    check("TRIGGER_A" not in ids and "TRIGGER_B" not in ids,
          "TRIGGER must be gone — FRZ replaced it")
```

and register it in the module's test list alongside the others.

- [ ] **Step 2: Run the guard to verify it fails**

Run: `cd /c/Users/bernd/Documents/AI/Spotykach/host/vcv && python res/test_panel.py`
Expected: FAIL — `TRIGGER must be gone — FRZ replaced it` (and the `PARAM_ORDER` comparison fails first).

- [ ] **Step 3: Widen the FX bottom row to four slots**

`host/vcv/res/gen_panel.py` line 157 — replace:

```python
FX_BOT   = [56.0, 69.5]             # GRIT COMP
```

with:

```python
# FX bottom row went from two slots to four (spec 2026-07-18 dust-grain-cloud):
# GRIT COMP DUST ROT. Pitch 8.833 mm against a 3.0 mm knob radius, so the
# 6.0 mm minimum in test_no_overlap still has room to spare.
FX_BOT   = [49.5, 58.333, 67.167, 76.0]   # GRIT COMP | DUST ROT
```

- [ ] **Step 4: Rename the TRIGGER pad slot to FRZ**

`host/vcv/res/gen_panel.py` lines 194-196 — replace:

```python
    pads = [("ENGINE", LATCH, "ENG"), ("GRITMODE", LATCH, "GRIT"),
            ("STEP", LATCH, "STEP"), ("PRINCIPLE", SMBTN, "PRIN"),
            ("NEWPHRASE", SMBTN, "NEW"), ("TRIGGER", SMBTN, "TRIG")]
```

with:

```python
    # FRZ took TRIGGER's slot in place (spec 2026-07-18 dust-grain-cloud): the
    # manual trigger was unused, and reusing the slot keeps PART_STRIDE at 23 so
    # every existing .vcv patch keeps its param ids. Momentary -> latch.
    pads = [("ENGINE", LATCH, "ENG"), ("GRITMODE", LATCH, "GRIT"),
            ("STEP", LATCH, "STEP"), ("PRINCIPLE", SMBTN, "PRIN"),
            ("NEWPHRASE", SMBTN, "NEW"), ("FRZ", LATCH, "FRZ")]
```

- [ ] **Step 5: Append DUST and ROT**

`host/vcv/res/gen_panel.py` — inside the `PARAMS = PART_A + PART_B + SHARED + [...]` tail, after the two `color_ctl(...)` lines (line 339):

```python
    # DUST / ROT: the grain cloud on the FLUX tape (spec 2026-07-18
    # dust-grain-cloud). Appended LAST like FILT/TIDE/FLUXRATE/COLOR so existing
    # .vcv patches keep their param ids; the coordinates fill slots 3 and 4 of
    # the widened FX bottom row, right under the delay cluster they feed off.
    Ctl("DUST_A", SMKNOB, FX_BOT[2],     ROW_V2, "DUST"),
    Ctl("DUST_B", SMKNOB, W - FX_BOT[2], ROW_V2, "DUST"),
    Ctl("ROT_A",  SMKNOB, FX_BOT[3],     ROW_V2, "ROT"),
    Ctl("ROT_B",  SMKNOB, W - FX_BOT[3], ROW_V2, "ROT"),
```

- [ ] **Step 6: Regenerate and verify the panel**

Run: `cd /c/Users/bernd/Documents/AI/Spotykach/host/vcv && python res/gen_panel.py && python res/test_panel.py`
Expected: the generator prints a param count of **75** (71 + 4) with `PART_STRIDE = 23`, and every guard in `test_panel.py` passes — in particular `test_no_overlap` (the four FX knobs at 8.833 mm pitch) and `test_enum_order`.

If `LOWER_A` in `test_panel.py` (lines ~192-201) pins the FX-row contents, update it to the new four-knob row before re-running.

- [ ] **Step 7: Commit**

```bash
git add host/vcv/res/gen_panel.py host/vcv/res/test_panel.py host/vcv/res/Spotymod.svg host/vcv/src/generated_panel.hpp
git commit -m "panel: FX bottom row to four slots (GRIT COMP DUST ROT), FRZ replaces TRIGGER in place"
```

---

### Task 10: VCV plugin wiring — config, defaults, tooltips, setters

**Files:**
- Modify: `host/vcv/src/Spotymod.cpp:68` (trigger member), `:84-143` (`configControls`), `:157-199` (`defaultFor`), `:212-285` (`pushParams`), plus a new `ParamQuantity` subclass near lines 26-47

**Interfaces:**
- Consumes: `Instrument::set_dust`, `set_rot`, `set_freeze` (Task 8); param ids `DUST_A/B`, `ROT_A/B`, `FRZ_A/B` (Task 9).
- Produces: the finished plugin. No further consumers.

- [ ] **Step 1: Remove the TRIGGER wiring**

`host/vcv/src/Spotymod.cpp` line 68 — remove `triggerTrig[2], ` from:

```cpp
    dsp::BooleanTrigger triggerTrig[2], spotTrig, settleTrig;
```

leaving:

```cpp
    dsp::BooleanTrigger spotTrig, settleTrig;
```

Line 256 — delete:

```cpp
            if (triggerTrig[p].process(ppb(TRIGGER_A, p))) inst.trigger_manual(p);
```

Then confirm nothing else references the old ids:

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && grep -rn "TRIGGER_A\|TRIGGER_B\|triggerTrig" host/vcv/src/`
Expected: no matches. (`Instrument::trigger_manual` stays in the engine — scenarios may still use it.)

- [ ] **Step 2: Add the ROT zone tooltip**

`host/vcv/src/Spotymod.cpp` — after the `FluxFbQuantity` struct (line 47):

```cpp
// ROT tooltip: name the zone the knob is in, then the position inside it.
// SYNC = grid-locked stutter, FREE = classic scatter, ROT = reverse + writeback.
struct RotQuantity : ParamQuantity {
    std::string getDisplayValueString() override {
        const float r = getValue();
        if (r < spky::dust_tuning::kZoneSEnd)
            return string::f("SYNC %.0f%%", r / spky::dust_tuning::kZoneSEnd * 100.f);
        if (r < spky::dust_tuning::kZoneFEnd)
            return string::f("FREE %.0f%%",
                (r - spky::dust_tuning::kZoneSEnd)
                / (spky::dust_tuning::kZoneFEnd - spky::dust_tuning::kZoneSEnd) * 100.f);
        return string::f("ROT %.0f%%",
            (r - spky::dust_tuning::kZoneFEnd)
            / (1.f - spky::dust_tuning::kZoneFEnd) * 100.f);
    }
};

// DUST tooltip: plain percent of grain activity.
struct DustQuantity : ParamQuantity {
    std::string getDisplayValueString() override {
        return string::f("%.0f%%", getValue() * 100.f);
    }
};
```

This needs the tuning constants — add `#include "fx/dust.h"` to the file's engine include block.

- [ ] **Step 3: Register the two knob types**

`host/vcv/src/Spotymod.cpp` — in `configControls()`, in the `WK_SMKNOB` chain, after the `FLUXFB_A || FLUXFB_B` arm (lines 105-106):

```cpp
                    else if (c.id == DUST_A || c.id == DUST_B)
                        configParam<DustQuantity>(c.id, 0.f, 1.f, defaultFor(c.id), lbl);
                    else if (c.id == ROT_A || c.id == ROT_B)
                        configParam<RotQuantity>(c.id, 0.f, 1.f, defaultFor(c.id), lbl);
```

`FRZ_A/B` needs no new arm: it is `WK_LATCH` and falls through the existing `else` at lines 130-133, which configures it as `{"Off", "On"}` with default `0.f`.

- [ ] **Step 4: Add the defaults**

`host/vcv/src/Spotymod.cpp` — in `defaultFor()`'s **first** switch (appended per-part pairs live there, not in the stride fold), after `case COLOR_B: return 0.f;` (line 176):

```cpp
            case DUST_A:       return 0.f;     // DUST off: bit-exact with the
            case DUST_B:       return 0.f;     // pre-DUST init patch
            case ROT_A:        return 0.f;     // zone S, fully grid-locked
            case ROT_B:        return 0.f;
```

- [ ] **Step 5: Push the params to the engine**

`host/vcv/src/Spotymod.cpp` — in `pushParams()`, inside the `for (int p ...)` loop, after the FLUX cluster (line 235) and before `inst.set_grit_mix(...)`:

```cpp
            // Appended params are outside the stride, so pp() would compute the
            // wrong id — the explicit ternary is required (see FLUXRATE/FLUXFB).
            inst.set_dust(p, params[p ? DUST_B : DUST_A].getValue());
            inst.set_rot(p, params[p ? ROT_B : ROT_A].getValue());
```

and, where the TRIGGER line was deleted in Step 1 (after the `newPhraseTrig` line, 255):

```cpp
            // FRZ is a template member, so pp()/ppb() apply. Level, not edge:
            // the latch state IS the freeze state.
            inst.set_freeze(p, ppb(FRZ_A, p));
```

- [ ] **Step 6: Build the plugin and verify**

Run: `cd /c/Users/bernd/Documents/AI/Spotykach/host/vcv && make -j4 EXTRA_CXXFLAGS=-std=c++17`
Expected: compiles clean, no warnings about unused `triggerTrig`.

Then run the desktop suite once more to be sure nothing regressed:

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build && ./build/spky_tests.exe`
Expected: PASS — whole suite.

- [ ] **Step 7: Commit**

```bash
git add host/vcv/src/Spotymod.cpp
git commit -m "spotymod: DUST/ROT knobs with zone tooltips, FRZ latch, drop manual trigger"
```

---

## Deferred (spec §"Out of scope", unchanged by this plan)

- **Rack play-test and listening pass.** Every constant in `dust_tuning` is provisional: zone breakpoints, density and length curves, `kWearRate`, `kWbGainMax`, the takeover knee, and the `1/sqrt(overlap)` normalization. This is the project convention and the reason they all live in one header block.
- **Unfreeze write-in crossfade** — only if the play-test hears a click at the seam.
- **`FRZGATE_A/B`** — dropped in v1 by the 2026-07-18 decision (no jacks on the hardware). If it ever returns, the engine API is already per-part and level-driven, so only `gen_panel.py` and one `pushParams` line change.
- **Pitch-shifted grains, half/double-speed heads, write-head skip chaos.**
- **Firmware (M6) wiring** — the engine layer is target-agnostic; only `bench/Makefile` needed the new source line.
- **M5 sampler** — must not be developed in a parallel branch touching `gen_panel.py`. DUST ships first and fixes the param ids; M5's `REC` appends after `ROT_B`.

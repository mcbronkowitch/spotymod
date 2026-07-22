# Sampler Slice-Groove Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** STEP mode plays transient-sliced material through the phrase engine — slice cursor for loop fidelity, MOTION as the order axis, SIZE trimming around slice unity, DENS capping tempo-locked rolls (spec `docs/superpowers/specs/2026-07-22-sampler-slice-groove-design.md`).

**Architecture:** A new `SliceMap` (fixed marker array, write-path transient detector) lives inside `SamplerEngine`. Part pushes step clock + phrase position through a sampler-specific side channel (the `set_sampler_overlap` idiom). The STEP scheduler is replaced: one fire = one slice grain, rolls retrigger at `step_samples / subdiv`. FLOW is untouched.

**Tech Stack:** C++17, no heap in `engine/` (host-injected memory only), doctest, CMake + Ninja + clang (desktop), deterministic Rng with documented draw order.

## Global Constraints

- **Build/test loop (always this, never MSVC):**
  `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build --target spky_tests && ./build/spky_tests -tc="<filter>"`
  Full suite: `./build/spky_tests` (exit code 0 = green).
- **No heap, no libDaisy includes in `engine/`.** Fixed-size arrays only.
- **Determinism:** every random draw goes through the engine's `Rng`; draw counts must not depend on outcomes (draw always, apply conditionally — the `pg_gen_groove` pattern).
- **FLOW must stay bit-identical.** The existing FLOW golden vector test ("sampler: golden vector -- Rng draw order and SOURCE mapping are locked", `tests/test_sampler_engine.cpp:848`) must keep passing unchanged. If it turns out to exercise STEP, see Task 9 — do not silently regenerate it.
- **No per-sample `std::pow`/`exp2f`:** trigger-rate and control-rate calls are fine (the `scan_rate()` precedent), the per-sample audio path is not.
- **Never search the SDRAM record buffer at spawn time.** All slice queries go through the marker array.
- **Commit trailer (every commit):** `Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>` — never the default Claude/Anthropic trailer.
- New tuning constants go to `engine/sampler/sampler_config.h` with a comment stating ear-tunable vs contract, matching the file's style.

---

### Task 1: SliceMap — detector core, offline scan, queries

**Files:**
- Create: `engine/sampler/slice_map.h`
- Create: `engine/sampler/slice_map.cpp`
- Create: `tests/test_slice_map.cpp`
- Modify: `engine/sampler/sampler_config.h` (append constants)
- Modify: `CMakeLists.txt` (add `engine/sampler/slice_map.cpp` next to **every** occurrence of `engine/sampler/sampler_engine.cpp` — the `spky_tests` list and the `render` list; add `tests/test_slice_map.cpp` to `spky_tests`)

**Interfaces:**
- Consumes: `SampleBuffer::Frame` (`engine/sampler/sample_buffer.h:20`), `sampler_cfg` constants.
- Produces (later tasks rely on these exact signatures):
  - `void SliceMap::init(float sample_rate)`
  - `void SliceMap::clear()`
  - `void SliceMap::scan(const SampleBuffer::Frame* buf, size_t frames)`
  - `int SliceMap::count() const`
  - `int SliceMap::index_at(float frame) const` — last marker with `start <= frame`; `0` if frame precedes all markers; `-1` only when empty
  - `uint32_t SliceMap::start(int i) const`
  - `uint32_t SliceMap::length(int i, size_t content) const` — to the next marker; last marker runs to `content`
  - `uint8_t SliceMap::strength(int i) const`

- [ ] **Step 1: Append constants to `sampler_config.h`** (before the closing namespaces)

```cpp
// --- slice groove (spec 2026-07-22 sampler-slice-groove-design.md) ---
// Marker capacity. 512 x 8 bytes = 4 KB SRAM. With kOnsetRefractS = 40 ms the
// map covers ~20 s of continuous worst-case onset rate before it is full; a
// full map ignores further onsets (oldest content keeps its markers). NOT
// ear-tunable: it is a memory budget.
constexpr int    kMaxSlices     = 512;
// Below this many markers the engine treats the material as transientless and
// slices on the tempo grid instead (SamplerEngine::_pool_size). Ear-tunable.
constexpr int    kMinSlices     = 4;
// Onset detector: fast/slow envelope pair on the written frame. All five are
// ear-tunable EXCEPT the refractory time's floor role: it also bounds marker
// density and therefore how fast the map fills (see kMaxSlices).
constexpr float  kOnsetFastS    = 0.001f;   // fast envelope time constant
constexpr float  kOnsetSlowS    = 0.080f;   // slow envelope time constant
constexpr float  kOnsetThresh   = 2.0f;     // fast/slow ratio that fires
constexpr float  kOnsetRearm    = 1.2f;     // ratio must fall below to re-arm
constexpr float  kOnsetFloor    = 0.01f;    // absolute fast-env floor (noise gate)
constexpr float  kOnsetRefractS = 0.040f;   // dead time after an onset
constexpr float  kOnsetPreRollS = 0.002f;   // marker sits this far BEFORE detection
// SIZE in STEP: window = slice length x 2^((SIZE - 0.5) * 2 * kSliceSizeOct),
// so knob centre is exactly one slice (findable unity, the SCAN 1.0x idiom),
// the bottom is 1/16th of the slice (attack tip), the top 16x (overrun into
// following material). Ear-tunable.
constexpr float  kSliceSizeOct  = 4.f;
```

- [ ] **Step 2: Write the failing tests** — `tests/test_slice_map.cpp`

```cpp
#include <doctest/doctest.h>
#include <cmath>
#include <vector>
#include "sampler/slice_map.h"
#include "sampler/sampler_config.h"
using namespace spky;

// Content with decaying clicks at the given frame positions: each click is a
// 1.0 impulse decaying over ~5 ms, silence elsewhere. Loud enough to clear
// kOnsetFloor, spaced by the caller.
static std::vector<SampleBuffer::Frame> clicks(size_t frames,
                                               const std::vector<size_t>& at) {
    std::vector<SampleBuffer::Frame> v(frames, SampleBuffer::Frame{0.f, 0.f});
    for (size_t c : at)
        for (size_t i = c; i < c + 240 && i < frames; ++i) {
            const float a = 1.0f * std::exp(-float(i - c) / 60.f);
            v[i].l = a; v[i].r = a;
        }
    return v;
}

TEST_CASE("slice map: offline scan marks each click, pre-rolled") {
    const std::vector<size_t> at = { 4800, 14400, 24000, 33600 };
    auto buf = clicks(48000, at);
    SliceMap m;
    m.init(48000.f);
    m.scan(buf.data(), buf.size());
    REQUIRE(m.count() == 4);
    const int pre = int(sampler_cfg::kOnsetPreRollS * 48000.f);
    for (int i = 0; i < 4; ++i) {
        // marker within [click - preroll, click + 3 ms detector lag]
        CHECK(m.start(i) >= at[i] - pre);
        CHECK(m.start(i) <= at[i] + 144);
    }
}

TEST_CASE("slice map: refractory time swallows a double hit") {
    // second click 20 ms after the first: inside kOnsetRefractS = 40 ms
    auto buf = clicks(48000, { 4800, 4800 + 960 });
    SliceMap m;
    m.init(48000.f);
    m.scan(buf.data(), buf.size());
    CHECK(m.count() == 1);
}

TEST_CASE("slice map: silence yields no markers") {
    auto buf = clicks(48000, {});
    SliceMap m;
    m.init(48000.f);
    m.scan(buf.data(), buf.size());
    CHECK(m.count() == 0);
    CHECK(m.index_at(1000.f) == -1);
}

TEST_CASE("slice map: index_at and length walk the sorted markers") {
    const std::vector<size_t> at = { 4800, 14400, 24000 };
    auto buf = clicks(30000, at);
    SliceMap m;
    m.init(48000.f);
    m.scan(buf.data(), buf.size());
    REQUIRE(m.count() == 3);
    CHECK(m.index_at(0.f) == 0);                      // before the first: slice 0
    CHECK(m.index_at(float(m.start(1))) == 1);
    CHECK(m.index_at(20000.f) == 1);
    CHECK(m.index_at(29000.f) == 2);
    CHECK(m.length(0, 30000) == m.start(1) - m.start(0));
    CHECK(m.length(2, 30000) == 30000 - m.start(2));  // last runs to content
}

TEST_CASE("slice map: clear empties, init resets the detector") {
    auto buf = clicks(48000, { 4800 });
    SliceMap m;
    m.init(48000.f);
    m.scan(buf.data(), buf.size());
    REQUIRE(m.count() == 1);
    m.clear();
    CHECK(m.count() == 0);
}
```

- [ ] **Step 3: Run to verify failure**

Run: `source env.sh && cmake --build build --target spky_tests`
Expected: FAIL to compile — `sampler/slice_map.h` not found. (CMake edit from this task must be in place first.)

- [ ] **Step 4: Implement** — `engine/sampler/slice_map.h`

```cpp
#pragma once
#include <cstddef>
#include <cstdint>
#include "sampler/sample_buffer.h"
#include "sampler/sampler_config.h"

namespace spky {

// Transient markers over the record buffer (spec 2026-07-22 slice-groove).
// Detection runs where writing runs -- once per written frame, a handful of
// ops -- and readers get a sorted, dense array of slice starts. No heap; no
// SDRAM search at spawn time, ever.
//
// The array is ALWAYS sorted by frame and dense (no tombstones): the live
// write path (Task 2) removes stale markers with a memmove the moment the
// write head passes them, which the refractory spacing bounds to at most one
// removal per kOnsetRefractS of audio.
class SliceMap {
public:
    static constexpr int kMax = sampler_cfg::kMaxSlices;

    void init(float sample_rate);
    void clear();

    // Offline path (load_sample): detect over [0, frames) in one call.
    // Replaces any existing markers.
    void scan(const SampleBuffer::Frame* buf, size_t frames);

    // Live path (Task 2): `frame` was just written; l/r are what actually
    // landed in the buffer (post overdub mix).
    void on_write(size_t frame, float l, float r);

    int count() const { return _n; }
    // Last marker with start <= frame. 0 when frame precedes every marker
    // (the material before the first transient belongs to slice 0), -1 only
    // when the map is empty.
    int index_at(float frame) const;
    uint32_t start(int i) const { return _m[i].frame; }
    // Frames from marker i to the next marker; the last runs to `content`.
    uint32_t length(int i, size_t content) const;
    uint8_t strength(int i) const { return _m[i].strength; }

private:
    struct Marker { uint32_t frame; uint8_t strength; };

    void _reset_detector();
    void _detect(size_t frame, float l, float r);
    void _insert(uint32_t frame, uint8_t strength);
    int  _lower_bound(uint32_t frame) const;   // first marker with frame >= arg

    Marker _m[kMax];
    int    _n = 0;
    int    _sweep = 0;                 // live path (Task 2): next stale marker
    size_t _last_frame = SIZE_MAX;     // live path: discontinuity detection
    float  _env_fast = 0.f, _env_slow = 0.f;
    float  _cf_fast = 1.f, _cf_slow = 1.f;
    int    _refract = 0;
    int    _refract_len = 1920;
    int    _preroll = 96;
    bool   _armed = true;
};

}  // namespace spky
```

`engine/sampler/slice_map.cpp`:

```cpp
#include "sampler/slice_map.h"
#include <cmath>
#include <cstring>

using namespace spky;
using namespace spky::sampler_cfg;

void SliceMap::init(float sample_rate) {
    // One-pole coefficient: 1 - exp(-1 / (tau * sr)), the OnePole idiom.
    _cf_fast = 1.f - std::exp(-1.f / (kOnsetFastS * sample_rate));
    _cf_slow = 1.f - std::exp(-1.f / (kOnsetSlowS * sample_rate));
    _refract_len = static_cast<int>(kOnsetRefractS * sample_rate);
    _preroll     = static_cast<int>(kOnsetPreRollS * sample_rate);
    clear();
}

void SliceMap::clear() {
    _n = 0;
    _sweep = 0;
    _last_frame = SIZE_MAX;
    _reset_detector();
}

void SliceMap::_reset_detector() {
    _env_fast = 0.f;
    _env_slow = 0.f;
    _refract  = 0;
    _armed    = true;
}

void SliceMap::scan(const SampleBuffer::Frame* buf, size_t frames) {
    clear();
    for (size_t i = 0; i < frames; ++i) _detect(i, buf[i].l, buf[i].r);
    _last_frame = SIZE_MAX;   // a following live take re-aims cleanly
}

void SliceMap::_detect(size_t frame, float l, float r) {
    const float x = 0.5f * std::fabs(l) + 0.5f * std::fabs(r);
    _env_fast += _cf_fast * (x - _env_fast);
    _env_slow += _cf_slow * (x - _env_slow);
    if (_refract > 0) { --_refract; return; }
    if (_armed) {
        if (_env_fast > kOnsetFloor && _env_fast > _env_slow * kOnsetThresh) {
            const uint32_t at = frame > static_cast<size_t>(_preroll)
                ? static_cast<uint32_t>(frame - _preroll) : 0u;
            // Strength: the fast/slow ratio, mapped so kOnsetThresh -> 0 and
            // ratio 8 -> 255. Saturating; a slow env of 0 (leading silence)
            // maps to full strength.
            float ratio = _env_slow > 1e-9f ? _env_fast / _env_slow : 8.f;
            float sn = (ratio - kOnsetThresh) / (8.f - kOnsetThresh);
            if (sn < 0.f) sn = 0.f;
            if (sn > 1.f) sn = 1.f;
            _insert(at, static_cast<uint8_t>(sn * 255.f));
            _armed = false;
            _refract = _refract_len;
        }
    } else if (_env_fast < _env_slow * kOnsetRearm) {
        _armed = true;
    }
}

int SliceMap::_lower_bound(uint32_t frame) const {
    int lo = 0, hi = _n;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (_m[mid].frame < frame) lo = mid + 1; else hi = mid;
    }
    return lo;
}

void SliceMap::_insert(uint32_t frame, uint8_t strength) {
    if (_n >= kMax) return;   // full: keep what we have (see kMaxSlices)
    const int p = _lower_bound(frame);
    if (p < _n)
        std::memmove(&_m[p + 1], &_m[p],
                     static_cast<size_t>(_n - p) * sizeof(Marker));
    _m[p].frame = frame;
    _m[p].strength = strength;
    ++_n;
}

int SliceMap::index_at(float frame) const {
    if (_n == 0) return -1;
    if (frame < 0.f) return 0;
    const int p = _lower_bound(static_cast<uint32_t>(frame) + 1u);
    return p > 0 ? p - 1 : 0;
}

uint32_t SliceMap::length(int i, size_t content) const {
    if (i < 0 || i >= _n) return 0;
    const uint32_t end = (i + 1 < _n)
        ? _m[i + 1].frame : static_cast<uint32_t>(content);
    return end > _m[i].frame ? end - _m[i].frame : 0;
}

// Live path lands in Task 2; a stub keeps the linker honest until then.
void SliceMap::on_write(size_t, float, float) {}
```

- [ ] **Step 5: Run tests**

Run: `source env.sh && cmake --build build --target spky_tests && ./build/spky_tests -tc="slice map*"`
Expected: all 5 cases PASS.

- [ ] **Step 6: Commit**

```bash
git add engine/sampler/slice_map.h engine/sampler/slice_map.cpp tests/test_slice_map.cpp engine/sampler/sampler_config.h CMakeLists.txt
git commit -m "feat(sampler): SliceMap -- offline transient detection over the record buffer"
```

---

### Task 2: SliceMap — live write path (overdub-safe)

**Files:**
- Modify: `engine/sampler/slice_map.cpp` (replace the `on_write` stub)
- Modify: `engine/sampler/slice_map.h` (private `_remove` helper)
- Test: `tests/test_slice_map.cpp` (append)

**Interfaces:**
- Consumes: Task 1's internals.
- Produces: `void SliceMap::on_write(size_t frame, float l, float r)` — call once per frame that actually landed in the buffer; handles sequential writes, ring wraps, and punch-ins.

- [ ] **Step 1: Write the failing tests** (append to `tests/test_slice_map.cpp`)

```cpp
TEST_CASE("slice map: live writes detect like the offline scan") {
    const std::vector<size_t> at = { 4800, 14400, 24000 };
    auto buf = clicks(30000, at);
    SliceMap live, off;
    live.init(48000.f);
    off.init(48000.f);
    off.scan(buf.data(), buf.size());
    for (size_t i = 0; i < buf.size(); ++i) live.on_write(i, buf[i].l, buf[i].r);
    REQUIRE(live.count() == off.count());
    for (int i = 0; i < live.count(); ++i) CHECK(live.start(i) == off.start(i));
}

TEST_CASE("slice map: overdub pass replaces the markers it overwrites") {
    auto take1 = clicks(30000, { 4800, 14400, 24000 });
    SliceMap m;
    m.init(48000.f);
    for (size_t i = 0; i < take1.size(); ++i) m.on_write(i, take1[i].l, take1[i].r);
    REQUIRE(m.count() == 3);
    // Second pass overwrites [0, 20000) with ONE click at 9600. The passed
    // region's old markers (4800, 14400) must go; 24000 must survive.
    auto take2 = clicks(20000, { 9600 });
    for (size_t i = 0; i < take2.size(); ++i) m.on_write(i, take2[i].l, take2[i].r);
    REQUIRE(m.count() == 2);
    const int pre = int(sampler_cfg::kOnsetPreRollS * 48000.f);
    CHECK(m.start(0) >= 9600 - size_t(pre));
    CHECK(m.start(0) <= 9600 + 144);
    CHECK(m.start(1) >= 24000 - size_t(pre));
}

TEST_CASE("slice map: a punch-in mid-buffer only clears what it passes") {
    auto take1 = clicks(30000, { 4800, 24000 });
    SliceMap m;
    m.init(48000.f);
    for (size_t i = 0; i < take1.size(); ++i) m.on_write(i, take1[i].l, take1[i].r);
    REQUIRE(m.count() == 2);
    // Punch in at 20000, write 6000 silent frames: passes 24000's marker,
    // leaves 4800's alone. (Silence detects nothing new.)
    for (size_t i = 20000; i < 26000; ++i) m.on_write(i, 0.f, 0.f);
    REQUIRE(m.count() == 1);
    CHECK(m.start(0) <= 4800);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `./build/spky_tests -tc="slice map: live*,slice map: overdub*,slice map: a punch-in*"` (after build)
Expected: FAIL — `on_write` is a no-op stub, counts are 0.

- [ ] **Step 3: Implement.** In `slice_map.h`, add to the private section:

```cpp
    void _remove(int i);
```

In `slice_map.cpp`, replace the stub:

```cpp
void SliceMap::_remove(int i) {
    if (i + 1 < _n)
        std::memmove(&_m[i], &_m[i + 1],
                     static_cast<size_t>(_n - i - 1) * sizeof(Marker));
    --_n;
}

// Sequential writes sweep stale markers out from under the head: a marker at
// frame F describes content that no longer exists once the head has written
// F. Removal is a memmove, bounded to at most one per kOnsetRefractS of audio
// by the refractory spacing of the markers themselves. Fresh markers from
// THIS pass sit at frame - preroll <= head and must survive the sweep --
// _insert leaves _sweep pointing past them (see below).
void SliceMap::on_write(size_t frame, float l, float r) {
    if (_last_frame == SIZE_MAX || frame != _last_frame + 1) {
        // New take, punch-in, or ring wrap: re-aim the sweep and reset the
        // detector -- the envelope history belongs to other material.
        _sweep = _lower_bound(static_cast<uint32_t>(frame));
        _reset_detector();
    }
    _last_frame = frame;
    while (_sweep < _n && _m[_sweep].frame <= frame) _remove(_sweep);
    const int n_before = _n;
    _detect(frame, l, r);
    if (_n != n_before) {
        // _detect inserted at some p <= _sweep (its frame <= head).
        // The sweep must stay aimed at the first marker AHEAD of the head.
        _sweep = _lower_bound(static_cast<uint32_t>(frame) + 1u);
    }
}
```

- [ ] **Step 4: Run tests**

Run: `./build/spky_tests -tc="slice map*"`
Expected: all 8 cases PASS.

- [ ] **Step 5: Commit**

```bash
git add engine/sampler/slice_map.h engine/sampler/slice_map.cpp tests/test_slice_map.cpp
git commit -m "feat(sampler): SliceMap live write path -- overdub-safe marker sweep"
```

---

### Task 3: Lane + SuperModulator accessors (step clock, slot)

**Files:**
- Modify: `engine/mod/lane.h` (public accessors, after `clock_scale()` at line 55)
- Modify: `engine/mod/super_modulator.h` (forwarders, after `pitch_sustain()` at line 32)
- Test: `tests/test_lane.cpp` (append)

**Interfaces:**
- Produces:
  - `int ModLane::cur_step() const` — current STEP slot (`-1` before the first boundary)
  - `int ModLane::steps() const`
  - `float ModLane::step_samples() const` — samples per STEP slot at the current rate; `0.f` when the lane is stopped
  - `int SuperModulator::pitch_cur_step() const`, `int SuperModulator::pitch_steps() const`, `float SuperModulator::pitch_step_samples() const`

- [ ] **Step 1: Write the failing test** (append to `tests/test_lane.cpp` — mirror the file's existing setup idiom for constructing a lane; it already builds STEP-mode lanes)

```cpp
TEST_CASE("lane: step clock accessors expose slot and step duration") {
    ModLane l;
    l.init(48000.f, 77);
    l.set_melodic(true);
    l.set_rate_hz(1.f);            // 1 Hz cycle
    l.set_step(true, 8);
    // step_samples: phase covers one cycle per 1/(rate*clock_scale) seconds;
    // with 8 steps at clock_scale 8/8 = 1 a step is sr / (rate * 8) = 6000.
    CHECK(l.steps() == 8);
    CHECK(l.step_samples() == doctest::Approx(6000.f).epsilon(0.001));
    CHECK(l.cur_step() == -1);     // no boundary yet
    // run one full step: the slot counter must have advanced into range
    for (int i = 0; i < 6001; ++i) l.process();
    CHECK(l.cur_step() >= 0);
    CHECK(l.cur_step() < 8);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `./build/spky_tests -tc="lane: step clock accessors*"` (after build)
Expected: FAIL to compile — no `cur_step` on `ModLane`.

- [ ] **Step 3: Implement.** `lane.h`, after `clock_scale()`:

```cpp
    // Slice-groove side channel (spec 2026-07-22): the sampler needs to know
    // where in the phrase a fire sits and how long a step is in samples.
    int   cur_step() const { return _cur_step; }
    int   steps()    const { return _steps; }
    // Samples per STEP slot at the current rate: one slot is 1/_steps of the
    // cycle and _phase_inc is cycle fraction per sample. 0 when stopped.
    float step_samples() const {
        return _phase_inc > 0.f
            ? 1.f / (_phase_inc * static_cast<float>(_steps)) : 0.f;
    }
```

`super_modulator.h`, after `pitch_sustain()`:

```cpp
    // Slice-groove side channel (spec 2026-07-22), master/PITCH lane only.
    int   pitch_cur_step()     const { return _lanes[LANE_PITCH].cur_step(); }
    int   pitch_steps()        const { return _lanes[LANE_PITCH].steps(); }
    float pitch_step_samples() const { return _lanes[LANE_PITCH].step_samples(); }
```

- [ ] **Step 4: Run tests**

Run: `./build/spky_tests -tc="lane: step clock accessors*"`
Expected: PASS. Also run the full suite once (`./build/spky_tests`) — accessors must not disturb anything.

- [ ] **Step 5: Commit**

```bash
git add engine/mod/lane.h engine/mod/super_modulator.h tests/test_lane.cpp
git commit -m "feat(mod): expose step clock and slot for the sampler slice groove"
```

---

### Task 4: Engine owns a SliceMap (record / load / clear)

**Files:**
- Modify: `engine/sampler/sample_buffer.h` (one accessor)
- Modify: `engine/sampler/sampler_engine.h` (member + seam)
- Modify: `engine/sampler/sampler_engine.cpp` (`init`, `process_in`, `load_sample`, `clear` path)
- Test: `tests/test_sampler_engine.cpp` (append)

**Interfaces:**
- Consumes: `SliceMap` (Tasks 1–2).
- Produces:
  - `size_t SampleBuffer::write_head() const`
  - `int SamplerEngine::slice_count() const` — observation seam for tests/hosts

- [ ] **Step 1: Write the failing test** (append to `tests/test_sampler_engine.cpp`; the `Rig` at the top of the file provides the harness)

```cpp
// Click-train content for the slice tests: n clicks evenly spaced, 5 ms decay.
static void feed_clicks(Rig& g, size_t content, int n) {
    std::vector<float> l(content, 0.f), r;
    const size_t gap = content / size_t(n);
    for (int c = 0; c < n; ++c)
        for (size_t i = 0; i < 240 && size_t(c) * gap + i < content; ++i)
            l[size_t(c) * gap + i] = std::exp(-float(i) / 60.f);
    r = l;
    g.e.load_sample(l.data(), r.data(), content);
}

TEST_CASE("sampler: load_sample scans the material into slices") {
    Rig g(0);                        // no preloaded content
    feed_clicks(g, 48000, 8);
    CHECK(g.e.slice_count() == 8);
}

TEST_CASE("sampler: recording detects slices as it writes") {
    Rig g(0);
    g.e.set_recording(true);
    // 1 s of input with 4 clicks, fed through process_in like a host would
    for (int i = 0; i < 48000; ++i) {
        float x = 0.f;
        for (int c = 0; c < 4; ++c) {
            const int at = c * 12000;
            if (i >= at && i < at + 240) x = std::exp(-float(i - at) / 60.f);
        }
        g.e.process_in(x, x);
        float a, b; g.e.process(a, b);
    }
    g.e.set_recording(false);
    CHECK(g.e.slice_count() == 4);
}

TEST_CASE("sampler: clear drops the slices with the content") {
    Rig g(0);
    feed_clicks(g, 48000, 8);
    REQUIRE(g.e.slice_count() == 8);
    g.e.clear();
    CHECK(g.e.slice_count() == 0);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `./build/spky_tests -tc="sampler: load_sample scans*,sampler: recording detects*,sampler: clear drops*"`
Expected: FAIL to compile — no `slice_count` on `SamplerEngine`.

- [ ] **Step 3: Implement.**

`sample_buffer.h`, in the queries block (after `raw()`, line 56):

```cpp
    // Where the next write() lands. The slice detector (SamplerEngine::
    // process_in) snapshots this before write() to know which frame the
    // written value landed on.
    size_t write_head() const { return _write_head; }
```

`sampler_engine.h`: add `#include "sampler/slice_map.h"`; in the observation block (near `active_grains()`):

```cpp
    int slice_count() const { return _slices.count(); }
```

private members (after `_buf`):

```cpp
    SliceMap _slices;
```

`sampler_engine.cpp`:

- `init()`: after the `_buf.init(...)` call, add `_slices.init(sample_rate);`
- `process_in()` (line 164) becomes:

```cpp
void SamplerEngine::process_in(float inL, float inR) {
    _in_l = inL;
    _in_r = inR;
    const bool rec = _buf.is_recording();
    const size_t head = _buf.write_head();
    _buf.write(inL, inR);              // no-op unless recording
    // Detect on what actually LANDED (post overdub mix), not on the input --
    // read back the frame the head just covered.
    if (rec && _buf.valid()) {
        const SampleBuffer::Frame& f = _buf.raw()[head];
        _slices.on_write(head, f.l, f.r);
    }
}
```

- `load_sample()` (line 172): after `_buf.set_rec_size(n);` add `_slices.scan(dst, n);`
- `clear()` in the header (line 122) becomes:

```cpp
    void clear() { _kill_all(); _buf.clear(); _slices.clear(); }
```

- [ ] **Step 4: Run tests**

Run: `./build/spky_tests -tc="sampler:*"`
Expected: the 3 new cases PASS, all existing sampler cases still PASS.

- [ ] **Step 5: Commit**

```bash
git add engine/sampler/sample_buffer.h engine/sampler/sampler_engine.h engine/sampler/sampler_engine.cpp tests/test_sampler_engine.cpp
git commit -m "feat(sampler): the engine slices what it records and loads"
```

---

### Task 5: New STEP core — one fire = one ordered slice grain

This task replaces the STEP scheduler. Old behavior removed here: free-running spawns under the gate, the burst release (`kBurstReleaseS`, `_release_ctr`), and `set_gate`'s immediate-spawn arm. Draw order for the new STEP fire is fixed HERE (walk, roll, pan — all always drawn) even though walk lands in Task 6 and roll in Task 7, so the draw contract never changes after this task.

**Files:**
- Modify: `engine/sampler/sampler_engine.h`
- Modify: `engine/sampler/sampler_engine.cpp`
- Modify: `engine/sampler/sampler_config.h` (delete `kBurstReleaseS`)
- Test: `tests/test_sampler_engine.cpp` (append; existing STEP tests will break — fix them in THIS task only if trivial, otherwise mark with `// SLICE-GROOVE AUDIT (Task 9)` and `doctest::skip(true)`, Task 9 settles them)

**Interfaces:**
- Consumes: `SliceMap` queries, Task 3's units (values arrive via the new setters; Part wiring is Task 8 — tests call the setters directly).
- Produces:
  - `void SamplerEngine::set_step_clock(float samples_per_step)` — ignores values `<= 0`
  - `void SamplerEngine::set_phrase_pos(int slot, int steps, float weight)`
  - `int SamplerEngine::last_slice() const`, `float SamplerEngine::step_clock() const`, `int SamplerEngine::retrig_period() const` (observation seams)
  - Internal: `_fire_slice()`, `_spawn_slice(int idx, float pan)`, `_pool_size()`, `_slice_pos(int k, float& pos, float& slice_len)`

- [ ] **Step 1: Write the failing tests** (append)

```cpp
// STEP slice rig: clicky content, STEP mode, phrase context pushed by hand
// the way Part will in Task 8. 8 clicks over 1 s -> 8 slices; step clock
// 6000 samples (an 8th at 120-ish bpm).
struct StepRig : Rig {
    StepRig() : Rig(0) {
        feed_clicks(*this, 48000, 8);
        e.set_flow(false);
        e.set_step_clock(6000.f);
        feed(0.5f);                     // MOTION 0, SIZE 0.5 (slice unity)
    }
    // one composed note: phrase position, latch, gate on
    void fire(int slot, int steps = 8, float weight = 1.f) {
        e.set_phrase_pos(slot, steps, weight);
        e.trigger(0.5f);
        e.set_gate(true);
    }
    void note_off() { e.set_gate(false); }
};

TEST_CASE("sampler STEP: a fire spawns exactly one grain at a slice start") {
    StepRig g;
    const int before = g.e.spawn_count();
    g.fire(0);
    g.render(64);
    CHECK(g.e.spawn_count() == before + 1);
    // spawn position sits on a marker
    bool on_marker = false;
    for (int i = 0; i < g.e.slice_count(); ++i)
        if (std::fabs(g.e.last_spawn_pos() - float(i * 6000)) < 200.f)
            on_marker = true;
    CHECK(on_marker);
}

TEST_CASE("sampler STEP: MOTION 0 walks the slices in order and wraps home") {
    StepRig g;
    std::vector<int> seq;
    for (int cycle = 0; cycle < 2; ++cycle)
        for (int slot = 0; slot < 4; ++slot) {
            g.fire(slot);
            g.render(64);
            seq.push_back(g.e.last_slice());
            g.note_off();
            g.render(64);
        }
    // ascending within a cycle, identical across cycles
    for (int i = 0; i < 3; ++i) CHECK(seq[i + 1] == (seq[i] + 1) % 8);
    for (int i = 0; i < 4; ++i) CHECK(seq[i] == seq[i + 4]);
}

TEST_CASE("sampler STEP: SIZE centre is slice unity, the ends trim and overrun") {
    StepRig g;
    g.fire(0);
    g.render(64);
    const int at_unity = g.e.last_spawn_len();
    CHECK(at_unity == doctest::Approx(6000).epsilon(0.05));
    g.note_off(); g.render(6000);
    g.feed(0.5f, 0.f, 0.f);            // SIZE 0: attack tip
    g.render(96);                       // let the control tick see it
    g.fire(1);
    g.render(64);
    CHECK(g.e.last_spawn_len() < at_unity / 8);
    g.note_off(); g.render(6000);
    g.feed(0.5f, 0.f, 1.f);            // SIZE 1: overrun
    g.render(96);
    g.fire(2);
    g.render(64);
    CHECK(g.e.last_spawn_len() > at_unity * 8);
}

TEST_CASE("sampler STEP: the gate falling releases the grain, no burst tail") {
    StepRig g;
    g.feed(0.5f, 0.f, 1.f);            // long window so it outlives the note
    g.render(96);
    g.fire(0);
    auto v = g.render(2000);
    CHECK(rms(v, 1000, 500) > 0.001f); // sounding under the gate
    g.note_off();
    g.render(48000);                    // far past any release fade
    CHECK(g.e.active_grains() == 0);   // released, not sustained
    const int n = g.e.spawn_count();
    g.render(12000);
    CHECK(g.e.spawn_count() == n);     // and nothing respawns after the gate
}

TEST_CASE("sampler STEP: transientless material falls back to the tempo grid") {
    Rig g;                              // default rig: 441 Hz sine, no clicks
    g.e.set_flow(false);
    g.e.set_step_clock(6000.f);
    REQUIRE(g.e.slice_count() < sampler_cfg::kMinSlices);
    std::vector<float> pos;
    for (int slot = 0; slot < 3; ++slot) {
        g.e.set_phrase_pos(slot, 8, 1.f);
        g.e.trigger(0.5f);
        g.e.set_gate(true);
        g.render(64);
        pos.push_back(g.e.last_spawn_pos());
        g.e.set_gate(false);
        g.render(64);
    }
    // consecutive fires step exactly one step-clock through the material
    CHECK(std::fabs(pos[1] - pos[0] - 6000.f) < 1.f);
    CHECK(std::fabs(pos[2] - pos[1] - 6000.f) < 1.f);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `./build/spky_tests -tc="sampler STEP:*"`
Expected: FAIL to compile — no `set_step_clock` / `set_phrase_pos` / `last_slice`.

- [ ] **Step 3: Implement the header.** `sampler_engine.h`:

Public, after `punch()`:

```cpp
    // --- slice groove (spec 2026-07-22): the Part side channel ---
    // Step duration in samples, pushed at the control tick. <= 0 is ignored
    // (a stopped lane); the default keeps grid fallback sane before the
    // first push.
    void set_step_clock(float samples_per_step);
    // Phrase position of the CURRENT fire, pushed immediately before
    // trigger/trigger_chord. weight is the slot's metric weight
    // (pg_metric_weight): downbeats near 1, offs low -- the roll dice reads
    // it inverted.
    void set_phrase_pos(int slot, int steps, float weight);
```

Observation block:

```cpp
    int   last_slice() const    { return _last_slice; }
    float step_clock() const    { return _step_samples; }
    int   retrig_period() const { return _retrig_period; }
```

Private methods (after `_next_ratio()`):

```cpp
    void _fire_slice();               // STEP: one fire = one slice grain
    void _spawn_slice(int k, float pan);
    int  _pool_size() const;          // live slices, or grid count in fallback
    // Resolve pool index k (cursor + walk, already folded) to a start
    // position and natural slice length. Marker mode reads the SliceMap;
    // grid mode computes from SOURCE/SCAN base + k * _step_samples.
    void _slice_pos(int k, float& pos, float& slice_len) const;
```

Private state (after `_spawn_jitter`):

```cpp
    // --- slice groove state ---
    float _step_samples = 6000.f;     // Part pushes; default = sane grid
    int   _phrase_slot  = 0;
    int   _phrase_steps = 8;
    float _phrase_weight = 1.f;
    int   _cursor = 0;                // slices advanced since the phrase wrap
    float _walk   = 0.f;             // MOTION walk offset, in slice units
    int   _retrig_period = 0;        // samples between roll retriggers; 0 = none
    int   _retrig_ctr    = 0;
    int   _last_slot  = -1;          // wrap detection: slot went backwards
    int   _last_slice = -1;
    float _dec_ref[kGrains] = {};    // dec samples per slot, for gate-fall release
```

Delete the `_release_ctr` member (line 264).

- [ ] **Step 4: Implement the engine.** `sampler_engine.cpp`:

Setters (near `set_scan`):

```cpp
void SamplerEngine::set_step_clock(float samples_per_step) {
    if (samples_per_step > 0.f) _step_samples = samples_per_step;
}

void SamplerEngine::set_phrase_pos(int slot, int steps, float weight) {
    _phrase_slot   = slot;
    _phrase_steps  = steps > 0 ? steps : 1;
    _phrase_weight = clampf(weight, 0.f, 1.f);
}
```

`trigger()` and `trigger_chord()` (lines 185, 194): append as the last line of each:

```cpp
    if (!_flow) _fire_slice();
```

New methods:

```cpp
int SamplerEngine::_pool_size() const {
    const int n = _slices.count();
    if (n >= kMinSlices) return n;
    const int g = static_cast<int>(static_cast<float>(_buf.rec_size()) / _step_samples);
    return g < 1 ? 1 : g;
}

void SamplerEngine::_slice_pos(int k, float& pos, float& slice_len) const {
    const size_t content = _buf.rec_size();
    if (_slices.count() >= kMinSlices) {
        pos = static_cast<float>(_slices.start(k));
        slice_len = static_cast<float>(_slices.length(k, content));
        return;
    }
    // Grid fallback: SOURCE + SCAN place the base, k steps of tempo grid on
    // top, folded into the content.
    const float span = content > 1 ? static_cast<float>(content) - 1.f : 0.f;
    float centre = clampf(_targets[LANE_SOURCE], 0.f, 1.f) * span + _scan_pos
                 + static_cast<float>(k) * _step_samples;
    const float c = static_cast<float>(content);
    while (centre >= c) centre -= c;
    while (centre < 0.f) centre += c;
    pos = centre;
    slice_len = _step_samples;
}

// One fire = one slice grain (spec 2026-07-22).
// --- Rng draw order is contract: walk, roll, pan. All three ALWAYS drawn. ---
// Walk applies from Task 6, roll from Task 7; drawing them from day one means
// the draw contract never shifts once tests pin it.
void SamplerEngine::_fire_slice() {
    if (_buf.is_empty()) return;

    // Phrase wrap: the slot counter went backwards -> cursor goes home.
    if (_phrase_slot < _last_slot) { _cursor = 0; _walk = 0.f; }
    _last_slot = _phrase_slot;

    const float motion = clampf(_targets[LANE_MOTION], 0.f, 1.f);
    const float wdraw = _rng.next_bipolar();     // 1st: walk (applied Task 6)
    const float rdraw = _rng.next_unipolar();    // 2nd: roll (applied Task 7)
    const float pan   = _rng.next_bipolar() * motion;  // 3rd: pan
    (void)wdraw; (void)rdraw;                    // applied in Tasks 6/7

    const int pool = _pool_size();
    int k;
    if (_slices.count() >= kMinSlices) {
        // Marker mode: base slice from SOURCE + SCAN, cursor on top.
        const size_t content = _buf.rec_size();
        const float span = content > 1 ? static_cast<float>(content) - 1.f : 0.f;
        float centre = clampf(_targets[LANE_SOURCE], 0.f, 1.f) * span + _scan_pos;
        const float c = static_cast<float>(content);
        while (centre >= c) centre -= c;
        while (centre < 0.f) centre += c;
        const int base = _slices.index_at(centre);
        k = (base + _cursor) % pool;
    } else {
        // Grid mode: _slice_pos folds base + k steps itself.
        k = _cursor % pool;
    }
    if (k < 0) k += pool;

    _spawn_slice(k, pan);
    ++_cursor;
    _retrig_period = 0;              // rolls land in Task 7
}

void SamplerEngine::_spawn_slice(int k, float pan) {
    // Slot search + density ceiling: the same shape as _spawn_one, and the
    // same drop accounting.
    int slot = -1;
    int live = 0;
    for (int i = 0; i < kGrains; ++i) {
        if (_grains[i].active())      ++live;
        else if (slot < 0)            slot = i;
    }
    const int ceiling = static_cast<int>(std::ceil(_overlap)) + kSpawnHeadroom;
    if (slot < 0 || live >= (ceiling < kGrains ? ceiling : kGrains)) {
        ++_dropped_spawns;
        return;
    }

    float pos, slice_len;
    _slice_pos(k, pos, slice_len);
    if (slice_len < 2.f) slice_len = 2.f;

    const float ratio = _next_ratio();   // latched burst pitch; draws nothing

    // SIZE in STEP: a factor around slice unity at knob centre. exp2f at
    // trigger rate -- the scan_rate() precedent, never per-sample.
    const float f = std::exp2((clampf(_targets[LANE_SIZE], 0.f, 1.f) - 0.5f)
                              * 2.f * kSliceSizeOct);
    float lenf = slice_len * f;
    // Tape keeps its meaning: a fixed amount of MATERIAL, so duration is /ratio.
    if (_tape) lenf /= (ratio > 0.001f ? ratio : 0.001f);
    if (lenf > kGrainLenCeil) lenf = kGrainLenCeil;
    if (lenf < 2.f) lenf = 2.f;
    const int len = static_cast<int>(lenf);

    const float atk_f = lerpf(kWindowHalfMin, kWindowHalfMax, _atk_n);
    const float dec_f = lerpf(kWindowHalfMin, kWindowHalfMax, _dec_n);
    int atk = static_cast<int>(lenf * atk_f);
    int dec = static_cast<int>(lenf * dec_f);
    if (atk < 1) atk = 1;
    if (dec < 1) dec = 1;

    _grains[slot].spawn(pos, ratio, pan, len, atk, dec, _reverse);
    // _size_ref = 0 keeps _trim_running's hands off slice grains: its rescale
    // maps SIZE-in-seconds to grain length, which no longer holds here, and
    // the note end (gate fall) already cuts them. 0 is its "no live grain"
    // sentinel, so the loop skips these slots entirely.
    _size_ref[slot] = 0.f;
    _len_ref[slot]  = static_cast<float>(len);
    _dec_ref[slot]  = static_cast<float>(dec);

    _last_slice = k;
    _last_ratio = ratio;
    _last_pan   = pan;
    _last_pos   = pos;
    _last_len   = len;
    ++_spawn_count;
}
```

`set_gate()` (the block around lines 130–162): replace the STEP branch. The gate rising no longer arms a spawn (the fire itself spawns); the gate falling stops rolls and releases what sounds, over each grain's own DEC:

```cpp
void SamplerEngine::set_gate(bool on) {
    _gate = on;
    if (_flow) return;
    if (!on) {
        _retrig_period = 0;
        for (int i = 0; i < kGrains; ++i) {
            if (!_grains[i].active()) continue;
            int fade = static_cast<int>(_dec_ref[i]);
            if (fade < static_cast<int>(kRecordFade))
                fade = static_cast<int>(kRecordFade);
            _grains[i].release(fade);
        }
    }
}
```

(Keep whatever the existing function does for FLOW; only the `!_flow` behavior changes. Delete the `_release_ctr` arm at lines 145–146 and the `if (!_flow) _release_ctr = 0;` at line 130.)

`process()` scheduling block (lines 695–709) becomes:

```cpp
    // --- scheduling ---
    if (_flow) {
        if (!_hold) {
            _spawn_ctr -= 1.f;
            if (_spawn_ctr <= 0.f) {
                _spawn_one();                    // zieht _spawn_jitter neu
                _spawn_ctr += _next_interval();
            }
        }
    } else if (!_hold && _gate && _retrig_period > 0) {
        // Rolls (Task 7): tempo-locked retriggers while the note holds.
        if (--_retrig_ctr <= 0) {
            _retrig_ctr = _retrig_period;
            const float motion = clampf(_targets[LANE_MOTION], 0.f, 1.f);
            const float wdraw = _rng.next_bipolar();          // walk (Task 6)
            const float pan   = _rng.next_bipolar() * motion; // pan
            (void)wdraw;
            _spawn_slice(_last_slice, pan);
        }
    }
```

`punch()` (line 810): add `_cursor = 0; _walk = 0.f;` — "new gene now" also sends the slice cursor home.

`sampler_config.h`: delete `kBurstReleaseS` (line 274) and its comment.

- [ ] **Step 5: Build; fix or park broken old STEP tests.**

Run: `source env.sh && cmake --build build --target spky_tests && ./build/spky_tests`
Expected: the 5 new `sampler STEP:` cases PASS. Old STEP-dependent cases (anything referencing `kBurstReleaseS`, e.g. `tests/test_sampler_engine.cpp:85`, and STEP cases in `tests/test_sampler_part.cpp`) fail to compile or fail: fix trivially where the assertion still describes wanted behavior; otherwise mark `* doctest::skip(true)` with `// SLICE-GROOVE AUDIT (Task 9)` and move on. FLOW cases must all PASS untouched.

- [ ] **Step 6: Commit**

```bash
git add engine/sampler/sampler_engine.h engine/sampler/sampler_engine.cpp engine/sampler/sampler_config.h tests/test_sampler_engine.cpp tests/test_sampler_part.cpp
git commit -m "feat(sampler): STEP fires slice grains -- ordered cursor, slice-unity SIZE, grid fallback"
```

---

### Task 6: MOTION walk — the order axis

**Files:**
- Modify: `engine/sampler/sampler_engine.cpp` (apply the walk draws from Task 5)
- Test: `tests/test_sampler_engine.cpp` (append)

**Interfaces:**
- Consumes: Task 5's `_fire_slice` / retrigger block (the `wdraw` values already drawn there).

- [ ] **Step 1: Write the failing tests** (append)

```cpp
TEST_CASE("sampler STEP: MOTION 0 is structurally still -- no walk, centered pan") {
    StepRig g;
    for (int slot = 0; slot < 8; ++slot) {
        g.fire(slot);
        g.render(64);
        CHECK(g.e.last_spawn_pan() == 0.f);
        g.note_off();
        g.render(64);
    }
}

TEST_CASE("sampler STEP: MOTION 1 leaves the ordered path") {
    StepRig g;
    g.feed(0.5f, 0.f, 0.5f, 1.f);      // MOTION 1
    g.render(96);
    int deviations = 0;
    int prev = -1;
    for (int cycle = 0; cycle < 4; ++cycle)
        for (int slot = 0; slot < 8; ++slot) {
            g.fire(slot);
            g.render(64);
            const int s = g.e.last_slice();
            if (prev >= 0 && s != (prev + 1) % 8) ++deviations;
            prev = s;
            g.note_off();
            g.render(64);
        }
    // The cubed walk leaves small steps common: some fires still land in
    // order, but across 32 fires a fully-ordered run is out of the question.
    CHECK(deviations > 4);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `./build/spky_tests -tc="sampler STEP: MOTION 1*"`
Expected: FAIL — `deviations == 0` (walk drawn but unapplied).

- [ ] **Step 3: Implement.** In `_fire_slice()`, replace `(void)wdraw;` and the `k` computation's cursor line:

```cpp
    // Walk: cubed like pg_contour_walk (neighbor steps common, leaps rare),
    // scaled by MOTION x pool. MOTION 0 multiplies to exactly 0 -- the
    // kColorGate idiom, structurally silent, no branch.
    _walk += wdraw * wdraw * wdraw * motion * static_cast<float>(pool);
```

and fold the walk into the index (both marker and grid branch):

```cpp
    const int wo = static_cast<int>(_walk);
    k = (base + _cursor + wo) % pool;          // marker branch
    // ...
    k = (_cursor + wo) % pool;                 // grid branch
```

(`pool` must be computed before the walk line; reorder accordingly. Keep `if (k < 0) k += pool;` after both.) In the retrigger block of `process()`, replace `(void)wdraw;` the same way and re-resolve the index instead of reusing `_last_slice` verbatim:

```cpp
            _walk += wdraw * wdraw * wdraw * motion * static_cast<float>(_pool_size());
            int k = _last_slice + static_cast<int>(_walk) - _walk_at_spawn;
            // At MOTION 0 _walk never moves, k == _last_slice, the roll
            // stutters one slice -- the spec's ratchet semantics.
```

Simpler equivalent (choose this): store nothing extra — at spawn time `_walk` was already folded into `_last_slice`, so the retrigger index is `_last_slice` plus only the walk accumulated SINCE the note's first spawn. Implement by snapshotting `float _walk_ref` in `_fire_slice` (add the member next to `_walk` in the header):

```cpp
    _walk_ref = _walk;   // in _fire_slice, right before _spawn_slice
```

retrigger block:

```cpp
            _walk += wdraw * wdraw * wdraw * motion * static_cast<float>(_pool_size());
            int k = (_last_slice + static_cast<int>(_walk - _walk_ref)) % _pool_size();
            if (k < 0) k += _pool_size();
            _spawn_slice(k, pan);
```

- [ ] **Step 4: Run tests**

Run: `./build/spky_tests -tc="sampler STEP:*"`
Expected: all STEP cases PASS (including Task 5's ordered-walk case — MOTION 0 must still be perfectly ordered).

- [ ] **Step 5: Commit**

```bash
git add engine/sampler/sampler_engine.h engine/sampler/sampler_engine.cpp tests/test_sampler_engine.cpp
git commit -m "feat(sampler): MOTION walks the slice pool -- ordered at zero, free at full"
```

---

### Task 7: Rolls — tempo-locked retriggers, weighted by the beat

**Files:**
- Modify: `engine/sampler/sampler_engine.cpp` (apply the roll draw from Task 5)
- Test: `tests/test_sampler_engine.cpp` (append)

**Interfaces:**
- Consumes: Task 5's `rdraw`, `_phrase_weight`, `_overlap`, `_step_samples`, the retrigger block.

- [ ] **Step 1: Write the failing tests** (append)

```cpp
TEST_CASE("sampler STEP: an off-beat at DENS max rolls at exactly step/subdiv") {
    StepRig g;
    g.e.set_overlap(1.f);              // DENS max -> subdiv cap 8
    g.render(96);
    // weight 0 = deepest off-beat -> roll probability 1 at DENS max
    g.fire(1, 8, 0.f);
    const int period = int(6000.f / 8.f);
    REQUIRE(g.e.retrig_period() == period);
    const int start = g.e.spawn_count();
    g.render(period * 4 + 8);
    CHECK(g.e.spawn_count() == start + 4);   // 4 retriggers, sample-exact
    g.note_off();
    g.render(period * 2);
    CHECK(g.e.spawn_count() == start + 4);   // gate fall stops the roll dead
}

TEST_CASE("sampler STEP: DENS min never rolls, whatever the dice say") {
    StepRig g;
    g.e.set_overlap(0.f);              // DENS min -> subdiv cap 1
    g.render(96);
    for (int i = 0; i < 16; ++i) {
        g.fire(i % 8, 8, 0.f);
        CHECK(g.e.retrig_period() == 0);
        g.note_off();
        g.render(128);
    }
}

TEST_CASE("sampler STEP: downbeats mostly hit once, offs roll -- the bias is real") {
    StepRig g;
    g.e.set_overlap(1.f);
    g.render(96);
    int down_rolls = 0, off_rolls = 0;
    for (int i = 0; i < 64; ++i) {
        g.fire(0, 8, 1.f);             // weight 1: downbeat
        if (g.e.retrig_period() > 0) ++down_rolls;
        g.note_off(); g.render(128);
        g.fire(1, 8, 0.2f);            // weight 0.2: off
        if (g.e.retrig_period() > 0) ++off_rolls;
        g.note_off(); g.render(128);
    }
    CHECK(down_rolls == 0);            // p = (1 - 1.0) * dens = 0, structural
    CHECK(off_rolls > 30);             // p = 0.8 at DENS max
}
```

- [ ] **Step 2: Run to verify failure**

Run: `./build/spky_tests -tc="sampler STEP: an off-beat*,sampler STEP: DENS min*,sampler STEP: downbeats*"`
Expected: FAIL — `retrig_period()` is always 0.

- [ ] **Step 3: Implement.** In `_fire_slice()`, replace `(void)rdraw;` and the trailing `_retrig_period = 0;`:

```cpp
    // Rolls: DENS caps the subdivision AND scales the odds; the metric
    // weight biases them -- downbeats (weight 1) structurally never roll
    // (p multiplies to 0, the kColorGate idiom again), deep offs gladly.
    const float dens_n = (_overlap - kOverlapMin) / (kOverlapMax - kOverlapMin);
    const int max_subdiv = static_cast<int>(_overlap + 0.5f);
    const float p_roll = (1.f - _phrase_weight) * dens_n;
    if (max_subdiv >= 2 && rdraw < p_roll) {
        int period = static_cast<int>(_step_samples / static_cast<float>(max_subdiv));
        const int floor_s = static_cast<int>(kSpawnMinSamples);
        _retrig_period = period < floor_s ? floor_s : period;
        _retrig_ctr    = _retrig_period;
    } else {
        _retrig_period = 0;
    }
```

- [ ] **Step 4: Run tests**

Run: `./build/spky_tests -tc="sampler STEP:*"`
Expected: all STEP cases PASS. (Task 5's gate-fall case already asserts rolls stop on note-off.)

- [ ] **Step 5: Commit**

```bash
git add engine/sampler/sampler_engine.cpp tests/test_sampler_engine.cpp
git commit -m "feat(sampler): rolls -- DENS-capped, beat-weighted, locked to step/subdiv"
```

---

### Task 8: Part wiring — the groove reaches the engine

**Files:**
- Modify: `engine/parts/part.cpp` (control tick + fire path)
- Test: `tests/test_sampler_part.cpp` (append)

**Interfaces:**
- Consumes: Task 3 accessors, Task 5 setters, `pg_metric_weight` (`mod/phrase_gen.h:30`, already included via lane.h → part.h chain; add `#include "mod/phrase_gen.h"` to part.cpp if the build says so).
- Produces: nothing new — this closes the loop.

- [ ] **Step 1: Write the failing test** (append to `tests/test_sampler_part.cpp`; mirror that file's existing Part rig — it already builds sampler Parts with injected memory)

```cpp
TEST_CASE("part: a sampler STEP part drives the slice groove end to end") {
    // Rig idiom of this file: Part + injected sampler memory. Load clicks,
    // STEP on, run -- the engine must receive a real step clock and fires
    // must land on slice starts without any test-side set_phrase_pos.
    std::vector<SampleBuffer::Frame> mem(48000 * 4);
    Part p;
    p.init(48000.f, 1234, nullptr, nullptr, mem.data(), mem.size());
    p.set_engine(ENGINE_SAMPLER);
    for (int i = 0; i < 400; ++i) { float a,b,c,d; p.process(a,b,c,d); } // fade+swap
    std::vector<float> l(48000, 0.f);
    for (int c = 0; c < 8; ++c)
        for (int i = 0; i < 240; ++i)
            l[c * 6000 + i] = std::exp(-float(i) / 60.f);
    p.sampler().load_sample(l.data(), l.data(), l.size());
    REQUIRE(p.sampler().slice_count() == 8);
    p.mod().set_rate(0.5f);
    p.set_step(true, 8);
    const float before = p.sampler().step_clock();
    int spawns_on_marker = 0, spawns = 0;
    for (int i = 0; i < 48000 * 4; ++i) {
        float a, b, c, d;
        p.process(a, b, c, d);
        static int last_count = 0;
        if (p.sampler().spawn_count() != last_count) {
            last_count = p.sampler().spawn_count();
            ++spawns;
            const float pos = p.sampler().last_spawn_pos();
            for (int s = 0; s < 8; ++s)
                if (std::fabs(pos - float(s * 6000)) < 200.f) { ++spawns_on_marker; break; }
        }
    }
    CHECK(p.sampler().step_clock() != before);   // Part pushed a real clock
    CHECK(spawns > 4);                            // the phrase actually fired
    CHECK(spawns_on_marker == spawns);            // every fire hit a slice
}
```

- [ ] **Step 2: Run to verify failure**

Run: `./build/spky_tests -tc="part: a sampler STEP part*"`
Expected: FAIL — `step_clock()` never changes from its default and/or spawns miss the markers (engine never got `set_phrase_pos`; note the default `_step_samples` is 6000 which equals this rig's spacing, hence the `!= before` assertion on the clock, not a value match — if the pushed clock happens to equal 6000 exactly, tighten the rig's rate until it differs).

- [ ] **Step 3: Implement.** `part.cpp`:

In `_control_tick()`, directly after `_sampler.set_overlap(_overlap_eff);` (line 229):

```cpp
    // Slice-groove side channel (spec 2026-07-22): the step clock rides the
    // same raster as every other engine push. Same idiom as set_overlap --
    // sampler-only, pushed at _sampler directly.
    if (_engine_id == ENGINE_SAMPLER)
        _sampler.set_step_clock(_mod.pitch_step_samples());
```

In `process()`, inside the `if (fired && !_note_suppressed)` block (line 322), BEFORE the `trigger_chord` call:

```cpp
        if (_engine_id == ENGINE_SAMPLER) {
            const int slot = _mod.pitch_cur_step();
            _sampler.set_phrase_pos(slot, _mod.pitch_steps(),
                                    pg_metric_weight(slot));
        }
```

- [ ] **Step 4: Run tests**

Run: `./build/spky_tests`
Expected: the new part case PASSES; full suite green except tests parked for Task 9.

- [ ] **Step 5: Commit**

```bash
git add engine/parts/part.cpp tests/test_sampler_part.cpp
git commit -m "feat(part): push step clock and phrase position to the sampler"
```

---

### Task 9: Test audit + the new STEP golden vector

**Files:**
- Modify: `tests/test_sampler_engine.cpp`, `tests/test_sampler_part.cpp` (settle every test parked in Task 5)
- Test: `tests/test_sampler_engine.cpp` (new golden vector)

- [ ] **Step 1: Settle the parked tests.** `grep -n "SLICE-GROOVE AUDIT" tests/*.cpp`. For each: if the property it pinned still exists in the slice world, rewrite it against the new behavior; if the property is gone WITH the old scheduler (burst release length, free-running STEP spawn rate), delete it and note what replaced it in the commit message. The FLOW golden vector at `tests/test_sampler_engine.cpp:848` must be verified: open it, confirm it runs `set_flow(true)`. If it exercises STEP anywhere, stop and flag for the author — do not regenerate it silently.

- [ ] **Step 2: Write the new STEP golden vector.** Mirror the FLOW golden vector's structure (`tests/test_sampler_engine.cpp:818-940`): a `StepRig` with fixed seed 4242, MOTION 0.5 (all three draws active), DENS max, 12 fires across two phrase cycles, recording `{slice, pos, pan, len}` per spawn. First run: print the table (`MESSAGE`), paste it into the test as the golden array, tolerance `0.01` on pos, `0.00002` on pan, exact on slice/len. Add the draw-order contract comment:

```cpp
// --- STEP golden vector: the slice-groove Rng draw order is a hard contract:
// per fire: walk, roll, pan. Per roll retrigger: walk, pan. A fire that hits
// a full pool still draws all three (the drop happens in _spawn_slice, after
// the draws). Any change to this order re-pins this table AND the spec's
// Determinism section -- never regenerate silently.
```

- [ ] **Step 3: Full suite**

Run: `./build/spky_tests`
Expected: everything green, zero skipped slice-groove tests remaining.

- [ ] **Step 4: Commit**

```bash
git add tests/test_sampler_engine.cpp tests/test_sampler_part.cpp
git commit -m "test(sampler): settle the STEP audit, pin the slice-groove golden vector"
```

---

### Task 10: Render scenarios, docs, host check

**Files:**
- Create: `host/render/scenarios/assets/gen_clicks.py`
- Create: `host/render/scenarios/sampler_slice_drums.json`
- Create: `host/render/scenarios/sampler_slice_field.json`
- Modify: `README.md` (one paragraph in the sampler section, matching its voice)

- [ ] **Step 1: Generate the drum asset.** `host/render/scenarios/assets/gen_clicks.py` (stdlib only):

```python
"""Writes in_clicks.wav: 8 s of a dry click pattern at 120 bpm -- kick-ish
sine thumps on the beat, noise ticks on the offs. Material for the
slice-groove render scenarios."""
import math, random, struct, wave

SR = 48000
random.seed(7)
n = SR * 8
buf = [0.0] * n
for beat in range(16):                      # 8th grid at 120 bpm = 250 ms
    at = int(beat * 0.25 * SR)
    if beat % 2 == 0:                       # thump: 60 Hz sine, 80 ms decay
        for i in range(int(0.12 * SR)):
            buf[at + i] += 0.8 * math.sin(2 * math.pi * 60 * i / SR) \
                           * math.exp(-i / (0.03 * SR))
    else:                                   # tick: noise burst, 15 ms decay
        for i in range(int(0.03 * SR)):
            buf[at + i] += 0.5 * (random.random() * 2 - 1) \
                           * math.exp(-i / (0.005 * SR))
with wave.open("host/render/scenarios/assets/in_clicks.wav", "wb") as w:
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(SR)
    frames = bytearray()
    for s in buf:
        v = max(-1.0, min(1.0, s))
        frames += struct.pack("<hh", int(v * 32767), int(v * 32767))
    w.writeframes(bytes(frames))
print("wrote in_clicks.wav")
```

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && python host/render/scenarios/assets/gen_clicks.py`
Expected: `wrote in_clicks.wav`.

- [ ] **Step 2: Write the scenarios.** `sampler_slice_drums.json` — follow `sampler_solo.json`'s shape exactly (it is the template: silence part A, sampler on part B, record then play):

```json
{
  "_comment": "Slice-groove listening aid: drum material. Record 1-9s, STEP from 10s. MOTION (via MOD, see part.cpp sampler MOTION note) opens 14/22/30 -- loop-true first, chop-kit last. DENS max from 26s lets the offs roll.",
  "sample_rate": 48000,
  "bpm": 120,
  "duration_s": 40.0,
  "input_wav": "host/render/scenarios/assets/in_clicks.wav",
  "init": [
    { "action": "set_engine", "part": 1, "value": "sampler" },
    { "action": "set_target_base", "part": 0, "slot": 4, "value": 0.0 },
    { "action": "set_target_active", "part": 0, "slot": 4, "flag": false },
    { "action": "set_target_base", "part": 1, "slot": 4, "value": 1.0 },
    { "action": "set_depth", "part": 1, "value": 0.0 }
  ],
  "events": [
    { "t": 1.0,  "action": "sampler_record", "part": 1, "flag": true },
    { "t": 9.0,  "action": "sampler_record", "part": 1, "flag": false },
    { "t": 10.0, "action": "set_step", "part": 1, "flag": true, "ivalue": 8 },
    { "t": 14.0, "action": "set_depth", "part": 1, "value": 0.3 },
    { "t": 22.0, "action": "set_depth", "part": 1, "value": 0.6 },
    { "t": 26.0, "action": "set_sampler_overlap", "part": 1, "value": 1.0 },
    { "t": 30.0, "action": "set_depth", "part": 1, "value": 1.0 }
  ]
}
```

Check `host/render/scenario.cpp`'s action table for the exact names (`set_depth`, `set_sampler_overlap` — grep them; if an action is missing from the table, use the nearest existing one and note it in the JSON `_comment`). `sampler_slice_field.json`: identical timeline but `"input_wav": "host/render/scenarios/assets/in_drone.wav"` and a `_comment` naming the point — grid fallback grooving on transientless material.

- [ ] **Step 3: Render both, listen-check by numbers.**

Run:
```bash
cmake --build build --target render
./build/render host/render/scenarios/sampler_slice_drums.json renders/sampler_slice_drums.wav
./build/render host/render/scenarios/sampler_slice_field.json renders/sampler_slice_field.wav
```
Expected: both render without error; renders are sanity checks, not bit-gates (project rule). Confirm non-silence after t=10 s (any RMS tool or the render's own CSV output).

- [ ] **Step 4: README + host note.** Add one short paragraph to `README.md`'s sampler section: STEP now plays transient slices through the phrase engine; MOTION orders/disorders; DENS caps rolls; FLOW unchanged. Also verify `host/vcv/` builds: check `host/vcv/build-local.sh`'s source list for an explicit mention of `sampler_engine.cpp` — if sources are listed one by one, add `engine/sampler/slice_map.cpp`; then run `./build-local.sh` from `host/vcv/` (NEVER a hand-rolled g++ line — project rule).

- [ ] **Step 5: Commit**

```bash
git add host/render/scenarios/assets/gen_clicks.py host/render/scenarios/sampler_slice_drums.json host/render/scenarios/sampler_slice_field.json README.md host/vcv/build-local.sh
git commit -m "feat(sampler): slice-groove render scenarios and docs"
```

---

## Plan Self-Review (done at write time)

- **Spec coverage:** SliceMap detector + preroll + refractory (T1), overdub sweep (T2), side channel (T3, T8), record/load/clear integration (T4), one-fire-one-slice + cursor + wrap + SIZE unity + grid fallback + gate-fall DEC release + burst-release removal (T5), MOTION order axis incl. ratchet-walk semantics (T6), rolls with DENS cap + weight bias + sample-exact interval (T7), determinism/golden vector + FLOW untouched (T9), two render scenarios (T10). Strength is stored and tested (T1) but unconsumed — the spec's accent follow-up owns it.
- **Type consistency:** `set_step_clock(float)` / `set_phrase_pos(int,int,float)` / `slice_count()` / `last_slice()` / `step_clock()` / `retrig_period()` used identically in T5–T9; SliceMap API of T1 consumed unchanged in T2/T4/T5.
- **Known judgment calls an executor must NOT "fix" silently:** `_size_ref = 0` opting slice grains out of `_trim_running`; downbeats structurally never rolling (p=0 at weight 1); the map ignoring onsets when full; grid fallback threshold `kMinSlices = 4`.

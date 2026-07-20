# Rhythm-fed delay taps — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the DUST grain cloud with two read taps per part whose offsets are the sample distances between the last onsets of the *other* bank's PITCH lane, latched once per source cycle.

**Architecture:** `ModLane` records the sample gaps between its own gated boundaries and publishes a `RhythmView` snapshot at each cycle wrap. `Instrument` reads each part's view at the control tick and pushes derived offsets to the *other* part's `Flux`. `Flux` owns a `TapBank` (two mono tape reads, one one-pole each, morph gains, dip-on-relatch) instead of a `DustCloud`. The grain pool, scheduler, beat plumbing, seed chain and EchoDelay erosion remnants are deleted.

**Tech Stack:** C++17, doctest, CMake + Ninja (desktop tests), DaisySP (vendored), VCV Rack SDK (plugin host).

## Global Constraints

- Spec: `docs/superpowers/specs/2026-07-20-rhythm-fed-delay-taps-design.md` (rev 2). It is the authority; this plan implements it.
- Branch: `dust-explore`. **Never bump `plugin.json`. Never create a `v*` tag.**
- Commit trailer, every commit: `Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>`
- **Never `git add -A`.** Stage explicit paths only.
- **Hard gate:** no existing PITCH, groove, CHOKE or Center test may be edited. The sole exceptions are tests that die with their feature (named per task). If a melodic-path test needs touching, **stop and report** — the design is violated.
- `NDEBUG` is defined nowhere; `assert()` is live in the audio path. Do not add asserts to per-sample code.
- No allocation, no `std::` math beyond what the file already includes, in any per-sample path.
- **Layering:** the mod layer must not include fx or synth headers (see `part.cpp:8-9` for the existing precedent). This is why `RhythmView` lives in `engine/mod/`, not in `fx/taps.h` as the spec's prose implies. Deliberate deviation, recorded in Task 1.
- Every new test must be **mutation-tested**: break the implementation deliberately, confirm the test fails, restore. A test that passes against a broken implementation is not a test. Report the mutation and its result in the task's completion notes.
- **Observability requirement — this project's dominant defect class.** Task 1 alone produced three tests that passed against the very mutation they existed to catch. Every one failed the same way: the *assertion* was right, but the *setup* put the sample point somewhere the property could not be seen — a lane config whose onset count had already saturated, a uniform pattern where two different behaviours produce identical numbers, an assertion satisfied by a neighbouring line of the fix. So for every test in this plan, before running it, answer explicitly: **at the moment this assertion runs, what value would the broken code produce here, and is it different from the correct one?** If the answer is "the same", the test is worthless however it reads — change the *configuration*, not the assertion. The test code in this plan is a starting point and has already been wrong three times; it is not evidence that a configuration is sound.
- Where a test depends on a timing window ("just past the first wrap", "no wrap in this span"), **assert the precondition itself** (e.g. `REQUIRE(l.phase() < 0.1f);`). Otherwise a later change to phase arithmetic turns the test vacuous silently, and the mutation that used to break it stops breaking it.
- Build and test:
  ```bash
  source env.sh
  cmake -S . -B build -G Ninja
  cmake --build build
  ./build/spky_tests
  ```
  Single case: `./build/spky_tests -tc="<exact test case name>"`

---

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `engine/mod/rhythm_view.h` | **New.** `RhythmView` POD — the onset gaps a lane publishes. Mod layer, no fx dependency. | 1 |
| `engine/mod/lane.h` / `lane.cpp` | **Modify.** Onset-gap ring, cycle latch, `rhythm()` accessor. | 1 |
| `engine/fx/taps.h` / `taps.cpp` | **New.** `TapeTap` (moved from `dust.h`), `derive_offsets()`, `tap_tuning` constants, `TapBank`. | 2, 3 |
| `engine/mod/super_modulator.h` | **Modify.** Forward `rhythm()` for `LANE_PITCH`; reset the ring in `reset_phases()`. | 1, 5 |
| `engine/fx/flux.h` / `flux.cpp` | **Modify.** Owns `TapBank`; `sync_beat` and seed removed; erosion remnants removed. | 4, 6 |
| `engine/fx/part_fx.h` / `part_fx.cpp` | **Modify.** `set_tap_offsets`; `sync_beat` and `dust_seed` removed. | 4, 5 |
| `engine/instrument.cpp` | **Modify.** Cross-feed at the control tick; beat forwarding removed. | 4, 5 |
| `engine/center/center.h` / `center.cpp` | **Modify.** `beat_edge`/`beat_samples` removed. | 4 |
| `engine/parts/part.cpp` | **Modify.** Seed argument dropped from `_fx.init`. | 4 |
| `engine/fx/dust.h` / `dust.cpp` | **Delete.** | 4 |
| `tests/test_rhythm_ring.cpp` | **New.** Ring, latch, validity, reset. | 1 |
| `tests/test_taps.cpp` | **New.** `derive_offsets` property test; `TapBank` audio behaviour. | 2, 3 |
| `tests/test_dust.cpp` | **Delete.** | 4 |
| `tests/test_flux.cpp` | **Modify.** Dust/takeover/erosion cases removed; tap cases added. | 4, 6 |
| `tests/test_center.cpp` | **Modify.** Beat-plumbing cases removed (lines 439-482 only). | 4 |
| `tests/test_instrument.cpp` | **Modify.** Cross-feed integration case added. | 5 |
| `CMakeLists.txt`, `host/vcv/Makefile` | **Modify.** Source lists. | 4 |
| `host/vcv/src/Spotymod.cpp` | **Modify.** `DustQuantity` / `RotQuantity` tooltips. | 7 |
| `bench/workloads_dust.cpp` | **Delete**, replaced by `bench/workloads_taps.cpp`. | 7 |

---

### Task 1: The onset-gap ring in `ModLane`

**Files:**
- Create: `engine/mod/rhythm_view.h`
- Modify: `engine/mod/lane.h`, `engine/mod/lane.cpp`
- Modify: `engine/mod/super_modulator.h:60`
- Test: `tests/test_rhythm_ring.cpp` (new)
- Modify: `CMakeLists.txt` (add the test file)

**Interfaces:**
- Produces: `struct spky::RhythmView { int32_t gap[2]; bool valid; }`; `const RhythmView& ModLane::rhythm() const`; `const RhythmView& SuperModulator::rhythm() const` (PITCH lane).
- Consumes: nothing from earlier tasks.

**Why the ring and not a gate-array walk:** only real onsets write it, so groove, rests, DENSE depth and freeze are accounted for by construction. Gaps are in **samples**, so STEP and FLOW need no special case.

**Why `_on_boundary()` and not `_start_note()`:** `_start_note` is called only under `_melodic && _step_mode` (`lane.cpp:175`), so a FLOW lane would never fill the ring.

- [ ] **Step 1: Write the failing test**

Create `tests/test_rhythm_ring.cpp`:

```cpp
#include "doctest.h"
#include "mod/lane.h"
#include <cmath>

using namespace spky;

namespace {
// A melodic STEP lane at a known rate. 4 steps/cycle, cycle rate chosen so a
// step is an exact sample count: rate_hz * clock_scale = cycles/s.
// clock_scale = 8/steps = 2, so phase_inc = rate_hz/sr * 2.
// With rate_hz = 1.0 and sr = 48000: phase_inc = 1/24000, one cycle = 24000
// samples, one step = 6000 samples.
ModLane make_lane(int steps = 4) {
    ModLane l;
    l.init(48000.f, 0xC0FFEEu);
    l.set_melodic(true);
    l.set_step(true, steps);
    l.set_rate_hz(1.f);
    l.set_density(1.f);          // every slot gated: uniform onsets
    l.set_variation(0.f);        // LOOP: no mutation, no drift
    return l;
}
}  // namespace

TEST_CASE("rhythm ring: a uniform STEP pattern reports the step length as both gaps") {
    ModLane l = make_lane(4);
    // Run four full cycles so the ring fills and at least one wrap latches it.
    for (int i = 0; i < 4 * 24000; ++i) l.process();

    const RhythmView& rv = l.rhythm();
    REQUIRE(rv.valid);
    CHECK(rv.gap[0] == doctest::Approx(6000).epsilon(0.01));
    CHECK(rv.gap[1] == doctest::Approx(6000).epsilon(0.01));
}

TEST_CASE("rhythm ring: a rest step lengthens the gap instead of writing an onset") {
    // DENSE below 1 masks the lowest-ranked slots -> fewer onsets per cycle,
    // so at least one gap must exceed a single step length.
    ModLane l = make_lane(8);
    l.set_density(0.5f);         // roughly half the cell notes play
    for (int i = 0; i < 8 * 24000; ++i) l.process();

    const RhythmView& rv = l.rhythm();
    REQUIRE(rv.valid);
    // one step at 8 steps/cycle: cycle = 24000*? -- clock_scale = 8/8 = 1, so
    // phase_inc = 1/48000, cycle = 48000 samples, step = 6000 samples.
    CHECK((rv.gap[0] > 6100 || rv.gap[1] > 6100));
}

TEST_CASE("rhythm ring: invalid until three onsets have been seen") {
    ModLane l = make_lane(4);
    CHECK_FALSE(l.rhythm().valid);          // fresh lane
    // One cycle = 4 onsets, but the view only latches AT a wrap, and the first
    // onset measures from an arbitrary start. After one full cycle the wrap has
    // latched a ring holding two real gaps.
    for (int i = 0; i < 24000; ++i) l.process();
    CHECK(l.rhythm().valid);
}

TEST_CASE("rhythm ring: the view only moves at a cycle wrap") {
    ModLane l = make_lane(4);
    for (int i = 0; i < 2 * 24000; ++i) l.process();
    REQUIRE(l.rhythm().valid);
    const RhythmView before = l.rhythm();

    // Advance most of one cycle -- several onsets, but no wrap.
    for (int i = 0; i < 20000; ++i) l.process();
    CHECK(l.rhythm().gap[0] == before.gap[0]);
    CHECK(l.rhythm().gap[1] == before.gap[1]);
}

TEST_CASE("rhythm ring: FLOW lanes fill the ring from cycle wraps") {
    ModLane l;
    l.init(48000.f, 0xC0FFEEu);
    l.set_melodic(true);
    l.set_step(false, 4);        // FLOW: clock_scale = 1, cycle = 48000 samples
    l.set_rate_hz(1.f);
    l.set_variation(0.f);
    for (int i = 0; i < 4 * 48000; ++i) l.process();

    const RhythmView& rv = l.rhythm();
    REQUIRE(rv.valid);
    CHECK(rv.gap[0] == doctest::Approx(48000).epsilon(0.01));
}

TEST_CASE("rhythm ring: reset invalidates and the ring re-fills from scratch") {
    ModLane l = make_lane(4);
    for (int i = 0; i < 2 * 24000; ++i) l.process();
    REQUIRE(l.rhythm().valid);

    l.reset(0.f);
    CHECK_FALSE(l.rhythm().valid);          // no gap straddles the reset

    for (int i = 0; i < 2 * 24000; ++i) l.process();
    CHECK(l.rhythm().valid);
    CHECK(l.rhythm().gap[0] == doctest::Approx(6000).epsilon(0.01));
}
```

Register it in `CMakeLists.txt` — after the line `tests/test_lane_tick.cpp`, add:

```cmake
    tests/test_rhythm_ring.cpp
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
source env.sh
cmake -S . -B build -G Ninja
cmake --build build
```
Expected: **compile error** — `mod/rhythm_view.h` not found / `ModLane::rhythm` undefined.

- [ ] **Step 3: Create `engine/mod/rhythm_view.h`**

```cpp
#pragma once
#include <cstdint>

namespace spky {

// The rhythm a lane publishes to whoever wants to place events against it:
// the sample distances between its last three gated boundaries, most recent
// first, as latched at the lane's last cycle wrap.
//
// Lives in the mod layer, NOT in fx/taps.h, because the mod layer must not
// include fx headers -- the same layering rule that keeps SynthEngine out of
// ModLane (see the static_assert in parts/part.cpp). fx/taps.h includes this.
//
// `valid` is false until three onsets have been recorded: two gaps need three
// onsets, and the first onset after init/reset measures from an arbitrary
// starting point rather than from a predecessor, so it is not a rhythm.
struct RhythmView {
    int32_t gap[2] = { 0, 0 };
    bool    valid  = false;
};

}  // namespace spky
```

- [ ] **Step 4: Add the ring state to `engine/mod/lane.h`**

Add the include at the top of the file, next to the existing mod-layer includes:

```cpp
#include "mod/rhythm_view.h"
```

Add the public accessor, immediately after `float target() const ...` (`lane.h:50`):

```cpp
    // The lane's own rhythm, latched at the last cycle wrap. See rhythm_view.h.
    const RhythmView& rhythm() const { return _rhythm; }
```

Add the private state, at the end of the member block (after the existing members):

```cpp
    // Onset-gap ring. _since_onset counts samples since the last gated
    // boundary; _gap holds the last two completed gaps, most recent first;
    // _onsets saturates at 3 (the count that makes two gaps real).
    // _rhythm is the snapshot consumers see, copied from the ring at a wrap.
    static constexpr int32_t kSinceOnsetMax = 1 << 24;   // ~5.8 min @ 48 kHz
    int32_t    _since_onset = 0;
    int32_t    _gap[2] = { 0, 0 };
    int        _onsets = 0;
    RhythmView _rhythm;
```

- [ ] **Step 5: Wire the ring in `engine/mod/lane.cpp`**

**5a — count samples.** In `ModLane::process()` (`lane.cpp:269`), immediately after `_fired = false;`:

```cpp
    if (_since_onset < kSinceOnsetMax) ++_since_onset;
```

In `ModLane::tick()` (`lane.cpp:310`), immediately after `_fired = false;`:

```cpp
    if (_since_onset < kSinceOnsetMax) _since_onset += kTickInterval;
```

**5b — record onsets.** In `ModLane::_on_boundary()` (`lane.cpp:167`), inside the `if (gated) {` branch, as its **first** statement (before `_fired = true;`):

```cpp
        _gap[1] = _gap[0];
        _gap[0] = _since_onset;
        _since_onset = 0;
        if (_onsets < 3) ++_onsets;
```

**5c — latch at the wrap.** In `ModLane::_wrap_events()` (`lane.cpp:246`), as its **first** statement (before the `_regen_pending` block):

```cpp
    // Publish the rhythm once per cycle, at the pattern boundary. Between
    // wraps the ring keeps recording but nothing downstream moves -- that is
    // what makes a looping source pattern produce a STANDING tap figure
    // rather than one that rotates on every onset.
    _rhythm.gap[0] = _gap[0];
    _rhythm.gap[1] = _gap[1];
    _rhythm.valid  = _onsets >= 3;
```

**5d — reset.** In `ModLane::reset(float phase)` (`lane.cpp:132`), before `_slew.reset(_target);`:

```cpp
    _since_onset = 0;
    _onsets = 0;
    _gap[0] = _gap[1] = 0;
    _rhythm = RhythmView{};
```

- [ ] **Step 6: Forward it from `SuperModulator`**

In `engine/mod/super_modulator.h`, immediately after `float clock_scale() const ...` (`:66`):

```cpp
    // The master lane's rhythm (mod-plane rhythm source for the FX taps).
    const RhythmView& rhythm() const { return _lanes[LANE_PITCH].rhythm(); }
```

`reset_phases()` (`:60`) already calls `l.reset(0.f)` on every lane, so step 5d covers RST with no change here.

- [ ] **Step 7: Run the tests to verify they pass**

```bash
cmake --build build && ./build/spky_tests -tc="rhythm ring*"
```
Expected: all 6 cases PASS.

Then the full suite:
```bash
./build/spky_tests
```
Expected: **0 failed**. No pre-existing case may change status.

- [ ] **Step 8: Mutation-test**

Run each mutation, confirm the named test **fails**, then restore:

| Mutation | Must break |
|---|---|
| In 5b, move the ring update out of the `if (gated)` branch (record on every boundary) | "a rest step lengthens the gap instead of writing an onset" |
| In 5c, set `_rhythm.valid = _onsets >= 2` | "invalid until three onsets have been seen" |
| In 5c, move the latch from `_wrap_events()` into `_on_boundary()` | "the view only moves at a cycle wrap" |
| In 5d, delete the `_onsets = 0;` line | "reset invalidates and the ring re-fills from scratch" |

If a mutation does **not** break its test, the test is weak — fix the test, not the mutation, and report it.

- [ ] **Step 9: Commit**

```bash
git add engine/mod/rhythm_view.h engine/mod/lane.h engine/mod/lane.cpp \
        engine/mod/super_modulator.h tests/test_rhythm_ring.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
mod: lanes publish their onset rhythm, latched per cycle

A lane now records the sample distance between its own gated boundaries
and publishes the last two as a RhythmView at each cycle wrap. Only real
onsets write the ring, so groove, rests, DENSE and freeze are accounted
for by construction; gaps are in samples, so STEP and FLOW need no
special case.

The hook is the gated branch of _on_boundary(), not _start_note() --
the latter runs only under _melodic && _step_mode, so a FLOW lane would
never have filled the ring.

Latching at the wrap rather than per onset is what makes a looping source
pattern produce a standing figure downstream instead of one that rotates
every few hundred milliseconds.

RhythmView lives in engine/mod/, not fx/taps.h as the spec's prose says:
the mod layer must not include fx headers.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 2: `derive_offsets` — the rule, and the property test that guards it

**Files:**
- Create: `engine/fx/taps.h`, `engine/fx/taps.cpp`
- Modify: `engine/fx/dust.h` (drop `TapeTap`, include `fx/taps.h` instead)
- Test: `tests/test_taps.cpp` (new)
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `spky::RhythmView` (Task 1).
- Produces: `spky::TapeTap` (moved verbatim from `dust.h:82-98`); `namespace spky::tap_tuning` constants; `void spky::derive_offsets(const RhythmView&, int32_t tape_len, int32_t out[2])`.

**This is the heart of the spec.** Zone S failed twice because evenly spaced taps *are* a delay. The property test below makes non-uniformity a proven property of `derive_offsets`, not an intention.

- [ ] **Step 1: Write the failing test**

Create `tests/test_taps.cpp`:

```cpp
#include "doctest.h"
#include "fx/taps.h"
#include <cmath>

using namespace spky;

namespace {
constexpr int32_t kTapeLen = 262144;    // Flux::kMaxSamples

RhythmView view(int32_t g0, int32_t g1) {
    RhythmView rv;
    rv.gap[0] = g0;
    rv.gap[1] = g1;
    rv.valid = true;
    return rv;
}

// The same +-2% test the guard uses, applied to the SPACINGS the listener
// hears: the dry signal sits at t = 0, so the spacings are out[0] and
// out[1] - out[0].
bool spacings_uniform(const int32_t out[2]) {
    const float a = static_cast<float>(out[0]);
    const float b = static_cast<float>(out[1] - out[0]);
    const float mean = 0.5f * (a + b);
    if (mean <= 0.f) return false;
    const float tol = tap_tuning::kUniformTol * mean;
    return std::fabs(a - mean) <= tol && std::fabs(b - mean) <= tol;
}
}  // namespace

TEST_CASE("derive_offsets: taps sit on the previous two onsets") {
    int32_t out[2];
    derive_offsets(view(6000, 9000), kTapeLen, out);
    CHECK(out[0] == 6000);
    CHECK(out[1] == 15000);
}

TEST_CASE("derive_offsets: an invalid view mutes both taps") {
    RhythmView rv = view(6000, 9000);
    rv.valid = false;
    int32_t out[2];
    derive_offsets(rv, kTapeLen, out);
    CHECK(out[0] == tap_tuning::kMuted);
    CHECK(out[1] == tap_tuning::kMuted);
}

TEST_CASE("derive_offsets: uniform gaps are spread into a limp") {
    int32_t out[2];
    derive_offsets(view(6000, 6000), kTapeLen, out);
    CHECK(out[0] == 6000);
    CHECK(out[1] == 6000 + 4500);     // second gap becomes 0.75 * 6000
    CHECK_FALSE(spacings_uniform(out));
}

TEST_CASE("derive_offsets: gaps within the tolerance still count as uniform") {
    int32_t out[2];
    derive_offsets(view(6000, 6060), kTapeLen, out);   // 1% apart
    CHECK(out[1] - out[0] == 4500);                    // the guard fired
}

TEST_CASE("derive_offsets: gaps outside the tolerance are left alone") {
    int32_t out[2];
    derive_offsets(view(6000, 6600), kTapeLen, out);   // 10% apart
    CHECK(out[1] - out[0] == 6600);                    // untouched
}

TEST_CASE("derive_offsets: an offset past the tape mutes that tap, never clamps") {
    int32_t out[2];
    // gap[0] fits, the cumulative sum does not.
    derive_offsets(view(200000, 200000), kTapeLen, out);
    CHECK(out[0] == 200000);
    CHECK(out[1] == tap_tuning::kMuted);
    // Both past the tape: both muted, and they must NOT collapse onto one
    // another (that would double an echo instead of dropping it).
    derive_offsets(view(300000, 300000), kTapeLen, out);
    CHECK(out[0] == tap_tuning::kMuted);
    CHECK(out[1] == tap_tuning::kMuted);
}

TEST_CASE("derive_offsets: sub-musical gaps mute rather than produce a buzz") {
    int32_t out[2];
    // NON-uniform on purpose. A uniform sub-musical pair like (4, 4) would be
    // caught by the guard's own `g1 < kMinGap` bail-out even with the entry
    // guard deleted, so the test would survive its own mutation and prove
    // nothing. This pair reaches the tail only if the entry guard is gone.
    derive_offsets(view(4, 100), kTapeLen, out);
    CHECK(out[0] == tap_tuning::kMuted);
    CHECK(out[1] == tap_tuning::kMuted);
    derive_offsets(view(100, 4), kTapeLen, out);
    CHECK(out[0] == tap_tuning::kMuted);
    CHECK(out[1] == tap_tuning::kMuted);
}

// THE property test: the reason this spec exists. Zone S was evenly spaced by
// construction and therefore was a delay. Over every gap pair on a musically
// reachable grid -- 240 to 96000 samples (5 ms to 2 s at 48 kHz), 32 geometric
// steps, both gaps independently -- the resulting spacings must never be
// uniform.
TEST_CASE("derive_offsets: no gap pair ever yields evenly spaced taps") {
    constexpr int kSteps = 32;
    constexpr float lo = 240.f, hi = 96000.f;
    const float ratio = std::pow(hi / lo, 1.f / static_cast<float>(kSteps - 1));

    int checked = 0;
    for (int i = 0; i < kSteps; ++i) {
        const int32_t g0 = static_cast<int32_t>(lo * std::pow(ratio, static_cast<float>(i)));
        for (int j = 0; j < kSteps; ++j) {
            const int32_t g1 = static_cast<int32_t>(lo * std::pow(ratio, static_cast<float>(j)));
            int32_t out[2];
            derive_offsets(view(g0, g1), kTapeLen, out);
            if (out[0] == tap_tuning::kMuted || out[1] == tap_tuning::kMuted) continue;
            INFO("g0=" << g0 << " g1=" << g1 << " out={" << out[0] << "," << out[1] << "}");
            CHECK_FALSE(spacings_uniform(out));
            ++checked;
        }
    }
    // Guard against the test vacuously passing because everything was muted.
    CHECK(checked > 500);
}
```

Register in `CMakeLists.txt` — after `tests/test_flux.cpp`, add:

```cmake
    engine/fx/taps.cpp
    tests/test_taps.cpp
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build build
```
Expected: **compile error** — `fx/taps.h` not found.

- [ ] **Step 3: Create `engine/fx/taps.h`**

```cpp
#pragma once
#include <cstdint>
#include "mod/rhythm_view.h"

namespace spky {

// Read-only view over the FLUX tape. Moved verbatim from fx/dust.h, which is
// deleted in task 4. There is only a `mask` -- no size/mask pair that could
// disagree; the power-of-two contract is checked once at compile time in
// taps.cpp against Flux::kMaxSamples, not per construction.
struct TapeTap {
    const float* l = nullptr;
    const float* r = nullptr;
    int32_t write_ptr = 0;
    int32_t mask = 0;

    int32_t size() const { return mask + 1; }

    // `offset` is samples BEHIND the write head; the head decrements, so a
    // constant offset is exactly 1x forward playback of material that old.
    float read(bool right, int32_t offset) const {
        int32_t i = (write_ptr + offset) & mask;
        return (right ? r : l)[i];
    }
};

namespace tap_tuning {

// A muted tap. 0 is safe as the sentinel because a sounding offset is always
// >= kMinGap; an offset of 0 would read at the write head and is never wanted.
constexpr int32_t kMuted = 0;

// Below this a "gap" is not a rhythm, it is a buzz -- and 0.75 * g would round
// toward a second gap equal to the first, defeating the uniformity guard.
constexpr int32_t kMinGap = 32;

// Gaps count as uniform when both lie within this fraction of their mean. A
// fraction, not an absolute count: at 240 samples a 2-sample jitter must not
// read as non-uniform, at 30000 a 50-sample drift must not read as uniform.
constexpr float kUniformTol = 0.02f;

// The spread applied when the guard fires: the MOTION lane's x3/4 ratio, a
// polyrhythm the instrument already runs. Cumulative offsets become g*{1,1.75}
// -- a limp, not a grid.
constexpr float kUniformSpread = 0.75f;

}  // namespace tap_tuning

// Turn a lane's published rhythm into two tape offsets, in samples behind the
// write head. Pure: no state, no sample rate, no tape. This is where the rule
// that Zone S lacked lives, and it is unit-testable on its own.
//
// out[i] == tap_tuning::kMuted means "this tap does not sound".
void derive_offsets(const RhythmView& rv, int32_t tape_len, int32_t out[2]);

}  // namespace spky
```

- [ ] **Step 4: Create `engine/fx/taps.cpp`**

```cpp
#include "fx/taps.h"
#include "fx/flux.h"
#include <cmath>

using namespace spky;

// The power-of-two contract TapeTap's AND-mask read depends on, checked once
// here rather than on every construction in the audio path.
static_assert((Flux::kMaxSamples & (Flux::kMaxSamples - 1)) == 0,
              "TapeTap's mask read requires a power-of-two tape");

void spky::derive_offsets(const RhythmView& rv, int32_t tape_len, int32_t out[2]) {
    out[0] = out[1] = tap_tuning::kMuted;
    if (!rv.valid) return;

    int32_t g0 = rv.gap[0];
    int32_t g1 = rv.gap[1];
    if (g0 < tap_tuning::kMinGap || g1 < tap_tuning::kMinGap) return;

    // Uniformity guard: evenly spaced taps ARE a delay (the diagnosis that
    // killed zone S twice). Counting the dry signal at t = 0, the spacings the
    // listener hears are exactly {g0, g1} -- so testing the gaps IS testing
    // the spacings.
    const float mean = 0.5f * (static_cast<float>(g0) + static_cast<float>(g1));
    const float tol  = tap_tuning::kUniformTol * mean;
    if (std::fabs(static_cast<float>(g0) - mean) <= tol &&
        std::fabs(static_cast<float>(g1) - mean) <= tol) {
        g1 = static_cast<int32_t>(tap_tuning::kUniformSpread * static_cast<float>(g0));
        if (g1 < tap_tuning::kMinGap) return;   // too short to spread audibly
    }

    const int32_t limit = tape_len - 2;
    const int32_t o0 = g0;
    const int32_t o1 = g0 + g1;
    // Mute, never clamp: clamping would put two taps at the same position,
    // turning a missing echo into a doubled one.
    if (o0 <= limit) out[0] = o0;
    if (o1 <= limit) out[1] = o1;
}
```

- [ ] **Step 5: Point `dust.h` at the moved struct**

In `engine/fx/dust.h`, delete the `struct TapeTap { ... };` block (`dust.h:82-98`) **and its preceding comment paragraph** (`dust.h:75-81`), and add near the top with the other includes:

```cpp
#include "fx/taps.h"
```

`dust.cpp` may carry its own `static_assert` on `Flux::kMaxSamples`; if so, delete it — `taps.cpp` now owns that check. Both files are removed entirely in Task 4; this step exists only to keep the build green in between.

- [ ] **Step 6: Run to verify the tests pass**

```bash
cmake --build build && ./build/spky_tests -tc="derive_offsets*"
```
Expected: all 8 cases PASS, including the property test.

```bash
./build/spky_tests
```
Expected: **0 failed** — the existing dust and flux suites must be untouched by the `TapeTap` move.

- [ ] **Step 7: Mutation-test**

| Mutation | Must break |
|---|---|
| Delete the whole uniformity-guard `if` block | "no gap pair ever yields evenly spaced taps" **and** "uniform gaps are spread into a limp" |
| Change `kUniformSpread` to `1.0f` | "no gap pair ever yields evenly spaced taps" |
| Replace the mute with a clamp: `out[1] = o1 <= limit ? o1 : limit;` | "an offset past the tape mutes that tap, never clamps" |
| Change `kUniformTol` to `0.0001f` | "gaps within the tolerance still count as uniform" |
| Delete the `g0 < kMinGap` guard | "sub-musical gaps mute rather than produce a buzz" |

The first mutation is the important one: it reproduces the zone S defect exactly. If the property test does not fail against it, the test is worthless — fix it and report.

- [ ] **Step 8: Commit**

```bash
git add engine/fx/taps.h engine/fx/taps.cpp engine/fx/dust.h \
        tests/test_taps.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
fx: derive_offsets turns a lane's rhythm into two tape offsets

The rule zone S lacked, as a pure function: cumulative offsets onto the
previous two onsets, a uniformity guard that spreads evenly spaced gaps
by the MOTION lane's x3/4 ratio, mute-never-clamp at the tape edge.

Guarded by an exhaustive property test -- every gap pair on a 32x32
geometric grid from 240 to 96000 samples must yield non-uniform spacings.
Deleting the guard fails it, which is the point: evenly spaced taps ARE a
delay, and that is what killed zone S twice.

TapeTap moves here from dust.h ahead of the cloud's removal.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 3: `TapBank` — the audio path

**Files:**
- Modify: `engine/fx/taps.h`, `engine/fx/taps.cpp`
- Test: `tests/test_taps.cpp` (append)

**Interfaces:**
- Consumes: `TapeTap`, `tap_tuning` (Task 2).
- Produces: `class spky::TapBank` with `init(float sr)`, `set_dust(float)`, `set_rot(float)`, `set_offsets(const int32_t[2])`, `bool active() const`, `void process(const TapeTap&, float& l, float& r)`, `int reads() const`.

Not wired into `Flux` yet — Task 4 does that. This task delivers a testable bank in isolation.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_taps.cpp`:

```cpp
namespace {
// A tape with a single impulse at a known distance behind the write head, so
// a tap reading offset N produces a non-zero sample exactly when N matches.
struct FakeTape {
    static constexpr int32_t kLen = 262144;
    std::vector<float> l = std::vector<float>(kLen, 0.f);
    std::vector<float> r = std::vector<float>(kLen, 0.f);
    int32_t wp = 1000;

    TapeTap view() const { return TapeTap{ l.data(), r.data(), wp, kLen - 1 }; }
    void poke(int32_t offset, float v) {
        const int32_t i = (wp + offset) & (kLen - 1);
        l[i] = v;
        r[i] = v;
    }
};

TapBank make_bank(float dust = 1.f, float rot = 0.f) {
    TapBank b;
    b.init(48000.f);
    b.set_rot(rot);
    b.set_dust(dust);
    return b;
}

// Run the bank until its gain slews settle, returning the last output pair.
void settle(TapBank& b, const TapeTap& t, float& l, float& r, int n = 8000) {
    for (int i = 0; i < n; ++i) { l = 0.f; r = 0.f; b.process(t, l, r); }
}
}  // namespace

TEST_CASE("tap bank: dust 0 from init is silent and performs no reads") {
    FakeTape tape;
    tape.poke(6000, 1.f);
    TapBank b = make_bank(0.f);
    const int32_t off[2] = { 6000, 10500 };
    b.set_offsets(off);

    CHECK_FALSE(b.active());
    float l = 0.f, r = 0.f;
    settle(b, tape.view(), l, r);
    CHECK(l == 0.f);
    CHECK(r == 0.f);
    CHECK(b.reads() == 0);
}

TEST_CASE("tap bank: dropping DUST to 0 rides the taps out instead of cutting them") {
    // Flux takes its bit-exact bypass on !active(). If active() went false the
    // instant the knob hit 0, a full-level tap sum would vanish in one sample.
    FakeTape tape;
    for (int32_t o = 0; o < 40000; ++o) tape.poke(o, 0.5f);   // DC everywhere
    TapBank b = make_bank(1.f);
    const int32_t off[2] = { 6000, 10500 };
    b.set_offsets(off);
    float l = 0.f, r = 0.f;
    settle(b, tape.view(), l, r);
    REQUIRE(std::fabs(l) > 1e-3f);

    b.set_dust(0.f);
    CHECK(b.active());                  // still riding out, not cut

    float prev = l;
    for (int i = 0; i < 8000; ++i) {
        l = 0.f; r = 0.f;
        b.process(tape.view(), l, r);
        CHECK(std::fabs(l - prev) < 0.01f);   // no step anywhere in the decay
        prev = l;
    }
    CHECK_FALSE(b.active());            // settled: the bypass is now safe
    CHECK(b.reads() == 0);
}

TEST_CASE("tap bank: dust morphs tap 0 in over the first half, tap 1 over the second") {
    FakeTape tape;
    TapBank b = make_bank(0.5f);
    const int32_t off[2] = { 6000, 10500 };
    b.set_offsets(off);
    float l = 0.f, r = 0.f;
    settle(b, tape.view(), l, r);
    CHECK(b.reads() == 1);              // tap 0 only at DUST 0.5

    b.set_dust(1.f);
    settle(b, tape.view(), l, r);
    CHECK(b.reads() == 2);              // both taps at DUST 1
}

TEST_CASE("tap bank: a muted offset costs no read") {
    FakeTape tape;
    TapBank b = make_bank(1.f);
    const int32_t off[2] = { 6000, tap_tuning::kMuted };
    b.set_offsets(off);
    float l = 0.f, r = 0.f;
    settle(b, tape.view(), l, r);
    CHECK(b.reads() == 1);
}

TEST_CASE("tap bank: a tap reads its own offset and nothing else") {
    FakeTape tape;
    tape.poke(6000, 1.f);               // material only at offset 6000
    TapBank b = make_bank(1.f, 0.f);    // ROT 0: filters effectively open
    const int32_t off[2] = { 6000, 10500 };
    b.set_offsets(off);

    // Settle the gain slew with the write head parked, then read once.
    float l = 0.f, r = 0.f;
    settle(b, tape.view(), l, r);
    CHECK(std::fabs(l) > 1e-3f);        // tap 0 (reads L, panned left) hears it

    // Move the impulse somewhere neither tap looks; output must collapse.
    FakeTape empty;
    float l2 = 0.f, r2 = 0.f;
    settle(b, empty.view(), l2, r2, 2000);
    CHECK(std::fabs(l2) < 1e-4f);
}

TEST_CASE("tap bank: a re-latch dips instead of crossfading -- reads never exceed live taps") {
    FakeTape tape;
    tape.poke(6000, 1.f);
    TapBank b = make_bank(1.f);
    int32_t off[2] = { 6000, 10500 };
    b.set_offsets(off);
    float l = 0.f, r = 0.f;
    settle(b, tape.view(), l, r);

    off[0] = 20000;                     // far more than kRelatchMin
    off[1] = 31000;
    b.set_offsets(off);
    // Through the whole dip, the bank must never read more than two positions.
    for (int i = 0; i < 1000; ++i) {
        l = 0.f; r = 0.f;
        b.process(tape.view(), l, r);
        CHECK(b.reads() <= 2);
    }
}

TEST_CASE("tap bank: an offset change below kRelatchMin does not dip") {
    FakeTape tape;
    for (int32_t o = 5900; o < 6100; ++o) tape.poke(o, 1.f);   // flat region
    TapBank b = make_bank(1.f);
    int32_t off[2] = { 6000, 10500 };
    b.set_offsets(off);
    float l = 0.f, r = 0.f;
    settle(b, tape.view(), l, r);
    const float before = l;

    off[0] = 6000 + tap_tuning::kRelatchMin - 1;
    b.set_offsets(off);
    l = 0.f; r = 0.f;
    b.process(tape.view(), l, r);
    CHECK(l == doctest::Approx(before).epsilon(1e-4)); // no envelope movement
}

TEST_CASE("tap bank: a re-latch produces no discontinuity") {
    FakeTape tape;
    for (int32_t o = 0; o < 40000; ++o) tape.poke(o, 0.5f);    // DC everywhere
    TapBank b = make_bank(1.f);
    int32_t off[2] = { 6000, 10500 };
    b.set_offsets(off);
    float l = 0.f, r = 0.f;
    settle(b, tape.view(), l, r);

    off[0] = 30000;
    b.set_offsets(off);
    float prev = l;
    for (int i = 0; i < 1000; ++i) {
        l = 0.f; r = 0.f;
        b.process(tape.view(), l, r);
        CHECK(std::fabs(l - prev) < 0.05f);     // no step, only the dip's ramp
        prev = l;
    }
}

TEST_CASE("tap bank: ROT separates the two taps spectrally") {
    // Tap 0 is low-passed, tap 1 high-passed. Feed white-ish alternating
    // content: at ROT 1 tap 0's contribution must shrink and tap 1's survive.
    FakeTape tape;
    for (int32_t o = 0; o < 40000; ++o) tape.poke(o, (o & 1) ? 1.f : -1.f);

    TapBank open = make_bank(1.f, 0.f);
    TapBank split = make_bank(1.f, 1.f);
    const int32_t off[2] = { 6000, 10500 };
    open.set_offsets(off);
    split.set_offsets(off);

    float ol = 0.f, orr = 0.f, sl = 0.f, sr = 0.f;
    settle(open, tape.view(), ol, orr);
    settle(split, tape.view(), sl, sr);

    // Nyquist content through a 400 Hz LP is essentially gone; through the
    // open 18 kHz LP it largely survives.
    CHECK(std::fabs(sl) < std::fabs(ol) * 0.5f);
}
```

Add `#include <vector>` to the top of `tests/test_taps.cpp`.

- [ ] **Step 2: Run to verify they fail**

```bash
cmake --build build
```
Expected: **compile error** — `TapBank` undefined.

- [ ] **Step 3: Add `TapBank` to `engine/fx/taps.h`**

Extend `namespace tap_tuning` with:

```cpp
constexpr int   kTaps        = 2;
// Taste constant for the play test: full DUST sits at parity with a direct
// tape read. The grain cloud's 7.27 dB window/pan makeup died with the cloud.
constexpr float kTapGain     = 0.7f;
// Below this, a jump is inaudible against the tape's own band-limit (64
// samples = 1.3 ms at 48 kHz) and dipping for it would be pure cost.
constexpr int32_t kRelatchMin = 64;
constexpr float kDipSeconds  = 0.002f;   // each side of the jump
constexpr float kGainSlewS   = 0.02f;
// Filter endpoints, ROT 0 -> ROT 1, interpolated geometrically.
constexpr float kLpOpenHz    = 18000.f;
constexpr float kLpSplitHz   = 400.f;
constexpr float kHpOpenHz    = 20.f;
constexpr float kHpSplitHz   = 1500.f;
// Equal-power pan at +-22.5 degrees: a spread, not a hard split.
constexpr float kPanNear     = 0.92388f;
constexpr float kPanFar      = 0.38268f;
```

Then, after `derive_offsets`'s declaration:

```cpp
// Two read taps on the FLUX tape, placed by the other bank's rhythm.
//
// Replaces DustCloud. There is no grain pool, no scheduler, no anchor and no
// RNG: the bank is deterministic, and its worst case is constant -- two mono
// reads and two one-poles, whatever the material does. That constancy is the
// point on an instrument already near its block budget.
class TapBank {
public:
    void init(float sample_rate);

    void set_dust(float d);                     // 0..1 morph (gain, tap count)
    void set_rot(float r);                      // 0..1 spectral spread
    void set_offsets(const int32_t off[tap_tuning::kTaps]);

    // True while the bank still has anything to contribute. Deliberately NOT
    // `_dust > 0`: Flux takes its bit-exact bypass when this is false, so
    // reporting inactive the instant the knob hits zero would drop a
    // full-level tap sum in one sample -- a click, defeating the very gain
    // slew that exists to prevent it. Staying active until the slews have
    // snapped to 0 lets the taps ride out, and the bypass is then reached
    // with nothing left to lose.
    bool active() const {
        if (_dust > 0.f) return true;
        for (const auto& t : _t) if (t.gain > 0.f) return true;
        return false;
    }

    // Reads the tape as it stands at the START of the sample; adds into l/r.
    void process(const TapeTap& tape, float& l, float& r);

    // Test/telemetry: tape reads performed on the last process() call. The
    // "a silent tap costs nothing" claim is otherwise unobservable.
    int reads() const { return _reads; }

private:
    struct OnePoleLp {
        float z = 0.f, a = 1.f;
        float process(float x) { z += a * (x - z); return z; }
        void  reset() { z = 0.f; }
    };

    enum class Dip { run, out, in };

    struct Tap {
        int32_t  off = tap_tuning::kMuted;
        int32_t  next_off = tap_tuning::kMuted;
        Dip      dip = Dip::run;
        int32_t  dip_ctr = 0;
        float    gain = 0.f, gain_target = 0.f;
        OnePoleLp lp;
    };

    void _update_filters();

    Tap   _t[tap_tuning::kTaps];
    float _sr = 48000.f;
    float _dust = 0.f;
    float _rot = -1.f;          // forces the first set_rot to compute
    int32_t _dip_len = 96;
    float _gain_coef = 1.f;
    int   _reads = 0;
};
```

- [ ] **Step 4: Implement `TapBank` in `engine/fx/taps.cpp`**

Add `#include "util/math.h"` at the top, then append:

```cpp
void TapBank::init(float sample_rate) {
    _sr = sample_rate > 0.f ? sample_rate : 48000.f;
    _dip_len = static_cast<int32_t>(tap_tuning::kDipSeconds * _sr);
    if (_dip_len < 1) _dip_len = 1;
    _gain_coef = 1.f / (tap_tuning::kGainSlewS * _sr);
    if (_gain_coef > 1.f) _gain_coef = 1.f;
    _dust = 0.f;
    _rot = -1.f;
    _reads = 0;
    for (auto& t : _t) t = Tap{};
    set_rot(0.f);
}

void TapBank::set_dust(float d) {
    _dust = clampf(d, 0.f, 1.f);
    // Tap 0 ramps over the knob's first half, tap 1 over the second: the
    // intermediate positions are an accent hierarchy (strong/weak), which is
    // the groove dimension a stepped tap count could not give.
    for (int i = 0; i < tap_tuning::kTaps; ++i) {
        const float g = clampf(2.f * _dust - static_cast<float>(i), 0.f, 1.f);
        _t[i].gain_target = g * tap_tuning::kTapGain;
    }
}

void TapBank::set_rot(float r) {
    const float v = clampf(r, 0.f, 1.f);
    if (v == _rot) return;      // the powf pair below must not run per tick
    _rot = v;
    _update_filters();
}

void TapBank::_update_filters() {
    // Geometric interpolation: the sweep is even by ear, not by hertz.
    const float lp_hz = tap_tuning::kLpOpenHz
        * std::pow(tap_tuning::kLpSplitHz / tap_tuning::kLpOpenHz, _rot);
    const float hp_hz = tap_tuning::kHpOpenHz
        * std::pow(tap_tuning::kHpSplitHz / tap_tuning::kHpOpenHz, _rot);
    constexpr float two_pi = 6.2831853f;
    _t[0].lp.a = 1.f - std::exp(-two_pi * lp_hz / _sr);
    _t[1].lp.a = 1.f - std::exp(-two_pi * hp_hz / _sr);
    if (_t[0].lp.a > 1.f) _t[0].lp.a = 1.f;
    if (_t[1].lp.a > 1.f) _t[1].lp.a = 1.f;
}

void TapBank::set_offsets(const int32_t off[tap_tuning::kTaps]) {
    for (int i = 0; i < tap_tuning::kTaps; ++i) {
        Tap& t = _t[i];
        const int32_t want = off[i];
        if (want == t.off && t.dip == Dip::run) continue;
        const int32_t d = want > t.off ? want - t.off : t.off - want;
        if (d < tap_tuning::kRelatchMin && t.dip == Dip::run) continue;
        // Dip, never crossfade: at no point may a tap read two positions.
        // Doubling a bank's reads whenever the source pattern changes is a
        // data-dependent worst case, which is the disease this design cures.
        t.next_off = want;
        t.dip = Dip::out;
        t.dip_ctr = _dip_len;
    }
}

void TapBank::process(const TapeTap& tape, float& l, float& r) {
    _reads = 0;
    float sum_l = 0.f, sum_r = 0.f;
    for (int i = 0; i < tap_tuning::kTaps; ++i) {
        Tap& t = _t[i];

        daisysp::fonepole(t.gain, t.gain_target, _gain_coef);
        // Snap: a one-pole approaches 0 asymptotically, so without this the
        // read would never actually be skipped and CPU would never follow the
        // knob down.
        if (t.gain_target == 0.f && t.gain < 1e-4f) t.gain = 0.f;

        float env = 1.f;
        switch (t.dip) {
            case Dip::out:
                env = hann_value_at(static_cast<float>(t.dip_ctr)
                                    / static_cast<float>(_dip_len));
                if (--t.dip_ctr <= 0) { t.off = t.next_off; t.dip = Dip::in; t.dip_ctr = 0; }
                break;
            case Dip::in:
                env = hann_value_at(static_cast<float>(t.dip_ctr)
                                    / static_cast<float>(_dip_len));
                if (++t.dip_ctr >= _dip_len) t.dip = Dip::run;
                break;
            case Dip::run:
                break;
        }

        if (t.gain <= 0.f || t.off == tap_tuning::kMuted) continue;

        // One MONO read per tap: tap 0 off the left tape, tap 1 off the right.
        // The echo path is effectively mono, so this is decorrelation, not
        // information loss -- and it halves the SDRAM traffic.
        const bool right = (i == 1);
        const float s = tape.read(right, t.off);
        ++_reads;

        // Tap 0 low-passes, tap 1 high-passes (x - lp(x)). At ROT 0 both are
        // effectively open and the bank is a plain two-tap delay on purpose.
        const float lp = t.lp.process(s);
        const float v = (i == 0 ? lp : s - lp) * t.gain * env;

        if (i == 0) { sum_l += v * tap_tuning::kPanNear; sum_r += v * tap_tuning::kPanFar; }
        else        { sum_l += v * tap_tuning::kPanFar;  sum_r += v * tap_tuning::kPanNear; }
    }

    l += sum_l;
    r += sum_r;
}
```

Add `#include "fx/fx_util.h"` (for `hann_value_at`) and `#include "Utility/dsp.h"` (for `daisysp::fonepole`) to `taps.cpp`.

- [ ] **Step 5: Run to verify they pass**

```bash
cmake --build build && ./build/spky_tests -tc="tap bank*"
```
Expected: all 8 cases PASS.

```bash
./build/spky_tests
```
Expected: **0 failed**.

- [ ] **Step 6: Mutation-test**

| Mutation | Must break |
|---|---|
| Delete the `if (t.gain <= 0.f \|\| t.off == kMuted) continue;` line | "a muted offset costs no read" and "dust 0 from init is silent and performs no reads" |
| Make `active()` return `_dust > 0.f` only | "dropping DUST to 0 rides the taps out instead of cutting them" |
| Delete the gain snap (`if (t.gain_target == 0.f && ...)`) | "dust morphs tap 0 in over the first half..." **and** "dropping DUST to 0..." (without the snap, `active()` never goes false again) |
| Replace the dip with a crossfade (read both `t.off` and `t.next_off`, sum them, `++_reads` twice) | "a re-latch dips instead of crossfading -- reads never exceed live taps" |
| Apply the new offset immediately in `set_offsets` (`t.off = want;` with no dip) | "a re-latch produces no discontinuity" |
| Change `kRelatchMin` to `1` | "an offset change below kRelatchMin does not dip" |
| Make tap 1 use `lp` instead of `s - lp` | "ROT separates the two taps spectrally" |

- [ ] **Step 7: Commit**

```bash
git add engine/fx/taps.h engine/fx/taps.cpp tests/test_taps.cpp
git commit -m "$(cat <<'EOF'
fx: TapBank -- two mono tape reads with a constant worst case

Two taps, one mono read and one one-pole each: tap 0 low-passed and
panned left off the left tape, tap 1 high-passed and panned right off the
right. ROT interpolates the cutoffs geometrically and only on change; DUST
morphs the two gains so intermediate positions are an accent hierarchy
rather than a dead zone.

A re-latch DIPS -- fade out, jump, fade in -- and never reads two
positions at once. A crossfade would double a bank's reads exactly when
the source pattern changes: a data-dependent spike, which is the disease
this rework exists to cure. A tap at gain 0 skips its read entirely, so
CPU follows the knob down to zero.

No pool, no scheduler, no anchor, no RNG.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 4: Swap the bank into `Flux`; delete the cloud and the beat plumbing

**Files:**
- Modify: `engine/fx/flux.h`, `engine/fx/flux.cpp`
- Modify: `engine/fx/part_fx.h`, `engine/fx/part_fx.cpp`
- Modify: `engine/parts/part.cpp:33`
- Modify: `engine/instrument.cpp:67-82`
- Modify: `engine/center/center.h`, `engine/center/center.cpp`
- Delete: `engine/fx/dust.h`, `engine/fx/dust.cpp`, `tests/test_dust.cpp`
- Modify: `tests/test_flux.cpp`, `tests/test_center.cpp`
- Modify: `CMakeLists.txt`, `host/vcv/Makefile`

**Interfaces:**
- Consumes: `TapBank` (Task 3).
- Produces: `void Flux::set_tap_offsets(const int32_t off[2])`, `void PartFx::set_tap_offsets(const int32_t off[2])`, `bool Flux::taps_active() const`. `Flux::init` loses its `seed` parameter; `PartFx::init` loses `dust_seed`.

This task is atomic: splitting it leaves the build broken.

- [ ] **Step 1: Delete the cloud and its tests**

```bash
git rm engine/fx/dust.h engine/fx/dust.cpp tests/test_dust.cpp
```

In `CMakeLists.txt`, remove both `engine/fx/dust.cpp` lines (in `spky_tests` and in `render`) and the `tests/test_dust.cpp` line. In `host/vcv/Makefile:41`, remove the `$(REPO)/engine/fx/dust.cpp \` line. Add `$(REPO)/engine/fx/taps.cpp \` in its place, and add `engine/fx/taps.cpp` to the `render` target's source list in `CMakeLists.txt` (it is already in `spky_tests` from Task 2).

- [ ] **Step 2: Rewrite `Flux`'s DUST surface in `engine/fx/flux.h`**

Replace `#include "fx/dust.h"` with `#include "fx/taps.h"`.

Replace the declarations at `flux.h:196-217` (the `init` comment block through `dust_active`) with:

```cpp
    void init(float sample_rate, float* buf_l, float* buf_r);
    void set_on(bool on, bool immediate = false) { _sw.set_on(on, immediate); }
    bool is_on() const { return _sw.is_on(); }
    bool engaged() const {
        return _buf_ok && (_sw.is_on() || !_sw.is_idle());
    }
    bool has_buffers() const { return _buf_ok; }
    void set_bpm(float bpm);
    void set_rate(int slice_idx);
    float delay_time() const { return _delay_time; }
    void set_feedback(float norm);
    void set_mix(float norm);
    void set_dust(float norm);                           // 0..1 tap morph
    void set_rot(float norm);                            // 0..1 spectral spread
    // Tap offsets in samples behind the write head, from the OTHER bank's
    // rhythm. Pushed at control rate by Instrument; see fx/taps.h.
    void set_tap_offsets(const int32_t off[tap_tuning::kTaps]);
    bool taps_active() const { return _taps.active(); }
    void process(float& l, float& r);
```

Replace the member `DustCloud _dust;` with `TapBank _taps;`.

Update the `_dust_norm`/`_rot_norm` comment (it names `DustCloud::_remap()`): the guard now exists because `TapBank::set_rot` runs two `powf` calls.

- [ ] **Step 3: Rewrite the DUST path in `engine/fx/flux.cpp`**

`Flux::init` — drop the `seed` parameter and the long seed comment (`flux.cpp:21-38`), and replace `_dust.init(sample_rate, seed);` with `_taps.init(sample_rate);`:

```cpp
void Flux::init(float sample_rate, float* buf_l, float* buf_r) {
    _sw.init(sample_rate);
    _sr = sample_rate;
    _buf_ok = (buf_l != nullptr && buf_r != nullptr);
    if (!_buf_ok) return;
    _echo_l.Init(sample_rate, buf_l);
    _echo_r.Init(sample_rate, buf_r);
    // short slew: click-free division changes, locks to grid (~30 ms lag)
    _dt_coef = daisysp::fmin(1.f / (0.03f * sample_rate), 1.f);
    _rate_idx = 3;               // boot "1/4"
    _bpm = 120.f;
    _taps.init(sample_rate);
    recompute_time(true);        // snap the boot delay time
    set_feedback(0.45f);
    set_mix(0.5f);
}
```

Replace `_dust.set_dust(d);` with `_taps.set_dust(d);` and `_dust.set_rot(r);` with `_taps.set_rot(r);`.

Delete `Flux::sync_beat` entirely (`flux.cpp:97-100`) and add:

```cpp
void Flux::set_tap_offsets(const int32_t off[tap_tuning::kTaps]) {
    if (!_buf_ok) return;
    _taps.set_offsets(off);
}
```

Replace `Flux::process`'s dust branch (`flux.cpp:113-145`) with:

```cpp
    if (!_taps.active()) {       // DUST = 0: bit-exact with the pre-DUST path
        l += _echo_l.Process(l * send, ds) * _mix_lin;
        r += _echo_r.Process(r * send, ds) * _mix_lin;
        return;
    }

    // The taps read the tape as it stands at the START of this sample -- built
    // before Process() advances the write head. Both channels share one write
    // pointer (they are written in lockstep).
    const TapeTap tape{_echo_l.line(), _echo_r.line(), _echo_l.write_ptr(),
                       static_cast<int32_t>(kMaxSamples) - 1};
    float tl = 0.f, tr = 0.f;
    _taps.process(tape, tl, tr);

    const float e_l = _echo_l.Process(l * send, ds);
    const float e_r = _echo_r.Process(r * send, ds);

    // Taps join BEFORE _mix_lin: FLUX MIX stays the single wet control for
    // everything coming off the tape. Tap reads deliberately skip the
    // band-pass and tanh -- the taps are rawer and brighter than the echo.
    // They are scaled by `send` (the SoftSwitch fade level) like the dry
    // signal feeding the echo: a raw read has no decay envelope of its own
    // and would otherwise hold full level right up to is_idle()'s hard cut.
    // The taps sit BESIDE the echo, never in its place -- there is no head
    // takeover any more.
    l += (e_l + tl * send) * _mix_lin;
    r += (e_r + tr * send) * _mix_lin;
```

- [ ] **Step 4: Drop the seed through `PartFx` and `Part`**

`engine/fx/part_fx.h`: change the `init` declaration and its comment to

```cpp
    void init(float sample_rate, float* echo_l, float* echo_r);
```

Replace `void sync_beat(float beat_samples) { _flux.sync_beat(beat_samples); }` (`part_fx.h:52`) with

```cpp
    void set_tap_offsets(const int32_t off[tap_tuning::kTaps]) {
        _flux.set_tap_offsets(off);
    }
```

`engine/fx/part_fx.cpp`: update `PartFx::init`'s signature and its `_flux.init(...)` call to drop `dust_seed`.

`engine/parts/part.cpp:28-33`: delete the dust-seed comment block and change the call to

```cpp
    _fx.init(sample_rate, echo_l, echo_r);
```

If `seed_base` becomes unused in that function, leave the parameter (the mod plane and synth drift still use it) — verify by compiling.

- [ ] **Step 5: Remove the beat plumbing**

`engine/instrument.cpp` — replace the whole `if (_center.beat_edge()) { ... }` block (`:71-79`) with nothing for now; Task 5 puts the cross-feed in its place. The block becomes:

```cpp
        if (_ctrl_ctr == 0) {                 // control-rate center update (per 96 samples)
            _center.update(_parts[PART_A].mod(), _parts[PART_B].mod(),
                           _parts[PART_A], _parts[PART_B]);
            _ctrl_ctr = Center::kCtrlInterval;
        }
```

`engine/center/center.h` — delete `beat_edge()` (`:42`), `beat_samples()` (`:43`), their comment block (`:35-41`), and the members `_beat_phase_prev` and `_beat_edge` with their comment (`:68-75`).

`engine/center/center.cpp` — in `Center::update`, replace the edge block (`:73-77`) with just `_transport.tick();`.

`tests/test_center.cpp` — delete lines 439-482 (the comment header and the two beat-plumbing `TEST_CASE`s). **Delete nothing else in this file.**

`tests/test_transport.cpp:39` — the comment mentions `Center::beat_samples()`. Reword it to reference the transport's own consumers; do not delete the test.

- [ ] **Step 6: Strip the dead cases from `tests/test_flux.cpp`**

Delete these `TEST_CASE`s entirely (they test the cloud, which no longer exists):
- `"flux: dust 0 is bit-exact with the pre-DUST path at any rot"` (:359) — **replaced** in step 7, do not simply drop the coverage
- `"flux: dust returning to 0 clears the grain pool for the next rise"` (:429)
- `"flux: dust makes sound and the head fades at the top"` (:514)
- `"flux: dust at full recirculates without running away (writeback still pending Task 5)"` (:590)
- `"flux: same seed reproduces the grain stream, different seeds diverge"` (:623)

Leave every other case untouched. The erosion/freeze cases (:172, :194, :216, :241, :271) are removed in **Task 6**, not here.

- [ ] **Step 7: Add the replacement flux-level tests**

Append to `tests/test_flux.cpp`:

```cpp
TEST_CASE("flux: dust 0 is bit-exact with the pre-DUST path at any rot") {
    // The DUST = 0 bypass must remain byte-identical, whatever ROT says --
    // ROT only configures filters that the bypass never reaches.
    static float a_l[Flux::kMaxSamples], a_r[Flux::kMaxSamples];
    static float b_l[Flux::kMaxSamples], b_r[Flux::kMaxSamples];

    Flux a, b;
    a.init(48000.f, a_l, a_r);
    b.init(48000.f, b_l, b_r);
    a.set_on(true, true);
    b.set_on(true, true);
    a.set_dust(0.f); a.set_rot(0.f);
    b.set_dust(0.f); b.set_rot(1.f);

    for (int i = 0; i < 20000; ++i) {
        const float x = std::sin(static_cast<float>(i) * 0.01f);
        float al = x, ar = x, bl = x, br = x;
        a.process(al, ar);
        b.process(bl, br);
        REQUIRE(al == bl);          // exact ==, not Approx
        REQUIRE(ar == br);
    }
}

TEST_CASE("flux: taps sound only once offsets have been pushed") {
    static float buf_l[Flux::kMaxSamples], buf_r[Flux::kMaxSamples];
    Flux f;
    f.init(48000.f, buf_l, buf_r);
    f.set_on(true, true);
    f.set_dust(1.f);
    f.set_rot(0.f);

    // Prime the tape with signal, offsets still muted.
    double quiet = 0.0;
    for (int i = 0; i < 30000; ++i) {
        const float x = std::sin(static_cast<float>(i) * 0.03f);
        float l = x, r = x;
        f.process(l, r);
        if (i > 25000) quiet += std::fabs(static_cast<double>(l - x));
    }

    const int32_t off[2] = { 6000, 10500 };
    f.set_tap_offsets(off);

    double loud = 0.0;
    for (int i = 30000; i < 60000; ++i) {
        const float x = std::sin(static_cast<float>(i) * 0.03f);
        float l = x, r = x;
        f.process(l, r);
        if (i > 55000) loud += std::fabs(static_cast<double>(l - x));
    }
    CHECK(loud > quiet);
}
```

Add `#include <cmath>` to `tests/test_flux.cpp` if it is not already there.

- [ ] **Step 8: Build and run**

```bash
cmake --build build && ./build/spky_tests
```
Expected: **0 failed**. Confirm the total case count dropped by exactly the cases deleted in steps 5 and 6, and rose by the two added in step 7 — report both numbers.

Also build the render host:
```bash
cmake --build build --target render
```
Expected: links clean.

**Determinism (spec test 10).** Render two scenarios twice each from the same
build and compare bytes:

```bash
for s in $(ls host/render/scenarios/*.json | head -2); do
  ./build/render "$s" /tmp/a.wav && ./build/render "$s" /tmp/b.wav
  cmp /tmp/a.wav /tmp/b.wav && echo "OK $s"
done
```
Expected: `OK` for both. (Check `./build/render --help` for the actual argument
form first; the invocation above is the shape, not necessarily the flags.)
Renders will differ from pre-change ones — that is expected and fine per the
project's bit-exactness policy; only *same-build* double renders must match.

- [ ] **Step 9: Mutation-test**

| Mutation | Must break |
|---|---|
| Make `Flux::set_tap_offsets` a no-op (early `return;`) | "flux: taps sound only once offsets have been pushed" |
| In `Flux::process`, remove the `!_taps.active()` early return so the tap path always runs | "flux: dust 0 is bit-exact with the pre-DUST path at any rot" |
| In `Flux::process`, drop the `* send` on the tap sum | neither — **this is expected**, and it is why the DUST=0 bit-exactness case uses exact `==`: report that the mutation survives rather than inventing a test for it |

- [ ] **Step 10: Commit**

```bash
git add -u engine/ tests/ CMakeLists.txt host/vcv/Makefile
git status --short          # verify NOTHING unexpected is staged
git commit -m "$(cat <<'EOF'
fx: TapBank replaces DustCloud; beat plumbing and seed chain deleted

Flux owns two read taps instead of eight grains. The taps sit beside the
echo rather than taking its place, so the head takeover goes with the
cloud, and the DUST = 0 bypass stays bit-exact at any ROT.

No beat anchor is needed: the write pointer decrements, so a constant
offset behind it is 1x forward playback of that age, continuously. That
fact killed rev 6 of the grain design; here it is the mechanism.
Center::beat_edge/beat_samples, Flux::sync_beat, PartFx::sync_beat and
the instrument forwarding are removed with their tests.

The tap bank has no randomness, so Flux::init's seed, PartFx's dust_seed
and part.cpp's derivation are gone too.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 5: Cross-feed — A hears B, B hears A

> **FOLDED INTO TASK 4 during execution (2026-07-20).** The split was a
> planning error. After Task 4 alone the taps stay muted, because nothing
> pushes real offsets until this task's cross-feed lands — so DUST and ROT go
> inert, which is a genuine behavioural regression against the grain cloud
> (which needed no external anchor to sound) and correctly fails three
> pre-existing forwarding tests. A reviewer cannot reject one of these tasks
> while approving the other, which is precisely the test for whether a split
> is legitimate. The content below is unchanged and was implemented as part of
> Task 4; it is kept here as the specification of that work.
>
> The tempting fixes were both refused: weakening the three forwarding tests,
> and adding an `Instrument::set_tap_offsets` passthrough for the tests to
> call. Offsets are derived internally by design — an external setter would be
> API grown to serve a test, and would let a caller place a bank's taps where
> its partner's rhythm never asked for them. The tests instead run long enough
> for both onset rings to fill and latch, then assert forwarding as before.

**Files:**
- Modify: `engine/instrument.h`, `engine/instrument.cpp`
- Test: `tests/test_instrument.cpp` (append)

**Interfaces:**
- Consumes: `SuperModulator::rhythm()` (Task 1), `derive_offsets` (Task 2), `PartFx::set_tap_offsets` (Task 4).
- Produces: `const RhythmView& Instrument::rhythm(int p) const`.

`Instrument` is the only scope where both parts are visible. No `Part` receives a pointer to its sibling.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_instrument.cpp`:

```cpp
TEST_CASE("instrument cross-feed: a bank's taps are placed by the OTHER bank's rhythm") {
    // H2, as an automated observable. Offsets are not readable off Flux, so
    // this drives the same pure rule the instrument drives, with the same
    // views -- and pins WHICH view feeds which part.
    Instrument inst;
    inst.init(48000.f);                     // engine-only init: no FX memory
    inst.set_tempo_bpm(120.f);
    for (int p = 0; p < 2; ++p) {
        inst.set_target_active(p, LANE_PITCH, true);
        inst.set_step(p, true, 8);
        inst.set_rate(p, 0.5f);
        inst.set_variation(p, 0.f);         // LOOP: no drift, so a change is a change
        inst.set_density(p, 1.f);
    }

    std::vector<float> l(64), r(64);
    auto run = [&](int blocks) {
        for (int b = 0; b < blocks; ++b)
            inst.process(nullptr, nullptr, l.data(), r.data(), 64);
    };
    run(4000);                              // let both rings fill and latch

    constexpr int32_t kTapeLen = static_cast<int32_t>(Flux::kMaxSamples);
    // Part A's taps read part B's rhythm -- that is the claim under test.
    int32_t a_taps_before[2];
    derive_offsets(inst.rhythm(PART_B), kTapeLen, a_taps_before);
    REQUIRE(a_taps_before[0] != tap_tuning::kMuted);

    // Changing part A's OWN rhythm must not move part A's taps.
    inst.set_density(PART_A, 0.4f);
    run(4000);
    int32_t a_taps_mid[2];
    derive_offsets(inst.rhythm(PART_B), kTapeLen, a_taps_mid);
    CHECK(a_taps_mid[0] == a_taps_before[0]);
    CHECK(a_taps_mid[1] == a_taps_before[1]);

    // Changing part B's rhythm must move part A's taps.
    inst.set_density(PART_B, 0.4f);
    run(4000);
    int32_t a_taps_after[2];
    derive_offsets(inst.rhythm(PART_B), kTapeLen, a_taps_after);
    CHECK((a_taps_after[0] != a_taps_before[0] ||
           a_taps_after[1] != a_taps_before[1]));
}
```

Add `#include "fx/taps.h"` to `tests/test_instrument.cpp`.

**Note:** this case pins the *routing* (which view feeds which part), which is what the mutation in step 5 breaks. It does not observe `Flux`'s internal offsets — `TapBank::reads()` is the only tap telemetry and it is per-bank, not per-offset. That is deliberate: adding an offset getter to `Flux` purely for a test would be test-driven API growth on the audio path.

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build build && ./build/spky_tests -tc="instrument cross-feed*"
```
Expected: FAIL (or a compile error naming the accessor you must correct per the note above).

- [ ] **Step 3: Wire the cross-feed**

In `engine/instrument.h`, add a read-only accessor next to the existing
`pitch_gate(int p) const` (`:94`), following the same idiom:

```cpp
    // The bank's own published rhythm (see mod/rhythm_view.h). Read by the
    // control tick to place the OTHER bank's taps -- and by tests.
    const RhythmView& rhythm(int p) const { return _parts[p].mod().rhythm(); }
```

In `engine/instrument.cpp`, add at the top:

```cpp
#include "fx/taps.h"
```

Inside the `if (_ctrl_ctr == 0)` block, after `_center.update(...)`:

```cpp
            // Cross-feed: each bank's taps are placed by the OTHER bank's
            // groove. Instrument is the only scope where both parts are
            // visible; no Part gets a pointer to its sibling. The views only
            // change once per source-lane cycle, so this is a handful of
            // integer ops at 500 Hz.
            constexpr int32_t kTapeLen = static_cast<int32_t>(Flux::kMaxSamples);
            int32_t off_a[tap_tuning::kTaps], off_b[tap_tuning::kTaps];
            derive_offsets(_parts[PART_B].mod().rhythm(), kTapeLen, off_a);
            derive_offsets(_parts[PART_A].mod().rhythm(), kTapeLen, off_b);
            _parts[PART_A].fx().set_tap_offsets(off_a);
            _parts[PART_B].fx().set_tap_offsets(off_b);
```

- [ ] **Step 4: Run to verify it passes**

```bash
cmake --build build && ./build/spky_tests -tc="instrument cross-feed*"
./build/spky_tests
```
Expected: PASS; full suite **0 failed**.

- [ ] **Step 5: Mutation-test**

| Mutation | Must break |
|---|---|
| Swap the two `derive_offsets` lines so each part hears itself | "instrument cross-feed: bank A's taps follow bank B's rhythm, not its own" |

If self-feed does **not** break the test, the test is not testing cross-feed — fix it and report.

- [ ] **Step 6: Commit**

```bash
git add engine/instrument.h engine/instrument.cpp tests/test_instrument.cpp
git commit -m "$(cat <<'EOF'
instrument: cross-feed the banks' rhythms into each other's taps

Part A's tap offsets come from part B's PITCH-lane rhythm and vice versa.
Instrument is the only scope where both parts are visible, so the coupling
lives there and no Part receives a pointer to its sibling.

This is the whole idea under test as H2: change MELO or DENSE on one bank
and the other bank's delay figure follows at the next pattern boundary.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 6: Remove the EchoDelay erosion remnants and the head takeover

**Files:**
- Modify: `engine/fx/flux.h`
- Modify: `tests/test_flux.cpp`

**Interfaces:** none produced; this is pure removal.

These were zone-R groundwork. With zone R cut there is no production caller — only `test_flux.cpp` exercises them. Doing it as its own task keeps the removal reviewable against Task 4's rewrite.

- [ ] **Step 1: Confirm there is no production caller**

```bash
grep -rn "set_wear\|set_freeze\|WriteBlend\|Advance()\|frozen()" engine/ host/ --include=*.h --include=*.cpp
```
Expected: hits only inside `engine/fx/flux.h` itself. **If any other production file appears, stop and report** — the spec's premise is wrong and the removal must not proceed.

- [ ] **Step 2: Delete the members**

In `engine/fx/flux.h`:
- `DeLine::Advance()` (`:61-65`) and `DeLine::WriteBlend()` (`:67-74`), with their comments.
- `EchoDelay::set_freeze` / `frozen()` / `set_wear` (`:139-143`), with their comments.
- The members `bool frozen_` and `float wear_` (`:180-181`), and their initialisation in `Init` (`:132-133`).
- The `wb` parameter and its comment (`:148-155`), so the signature becomes `float Process(float in, float delay_samples)`.
- The frozen/erosion branches in `Process`, so the store becomes:

```cpp
        delay_line_.Write(out * feedback_ + in);
        return out;
```

- [ ] **Step 3: Delete the tests that die with it**

In `tests/test_flux.cpp`, delete these `TEST_CASE`s entirely:
- `"echo: zero writeback is bit-exact with the one-arg store"` (:172)
- `"echo: freeze stops writing but keeps the pointer moving"` (:194)
- `"echo: frozen with wear < 1 decays the loop, bounded"` (:216)
- `"echo: writeback stays bounded under sustained full scale"` (:241)
- `"echo: frozen writeback overdubs the tape in the direction of wb"` (:271)

Keep `"deline: N samples behind the head reads the sample written N steps ago"` (:333) — it pins the decrementing-head fact this whole design rests on.

- [ ] **Step 4: Build and run**

```bash
cmake --build build && ./build/spky_tests
```
Expected: **0 failed**. Report the new case count.

- [ ] **Step 5: Commit**

```bash
git add engine/fx/flux.h tests/test_flux.cpp
git commit -m "$(cat <<'EOF'
fx: drop EchoDelay's freeze/erosion remnants with zone R

set_freeze, set_wear, WriteBlend, Advance and Process()'s wb parameter
were groundwork for the grain cloud's zone R. With the cloud gone there is
no production caller -- only test_flux.cpp exercised them, and those cases
go too. This also retires the unresolved question of what wear_ >= 1 means
on two different store paths, which would have had to be settled before
any knob could be wired to it.

The deline "N samples behind the head" case stays: it pins the
decrementing-write-head fact the whole tap design rests on.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 7: Host tooltips and the bench workload

> **Step 1 (the `Spotymod.cpp` tooltip rewrite) was pulled forward into Task 4
> during execution (2026-07-20).** Task 4 deletes `engine/fx/dust.h`, which
> `Spotymod.cpp` includes for the zone constants — leaving the edit here would
> have made the VCV plugin unbuildable for three tasks. Since the plugin is
> what the listening gate is played on, a branch where every commit builds is
> worth more than a tidy task boundary. Step 1 below is therefore already
> done; this task covers the bench workload, plus **verifying** that the
> plugin still builds (which Task 4 could not do — that build needs the Rack
> SDK and a separate mingw toolchain outside its gate, and Task 4 declared it
> unverified).

**Files:**
- Modify: `host/vcv/src/Spotymod.cpp:50-71`
- Delete: `bench/workloads_dust.cpp`
- Create: `bench/workloads_taps.cpp`
- Modify: `bench/Makefile`, `bench/anchor.cpp`

**Interfaces:** none produced.

- [ ] **Step 1: Rewrite the tooltips**

In `host/vcv/src/Spotymod.cpp`, remove the `#include "fx/dust.h"` (it is what pulled in `dust_tuning`) and replace `RotQuantity` and `DustQuantity` (`:50-71`) with:

```cpp
// ROT tooltip: how far apart the two taps are spread spectrally. 0 = both
// filters open and the taps read as a plain two-tap delay; 1 = tap 0 dark,
// tap 1 bright, which is what stops them sounding like the echo.
struct RotQuantity : ParamQuantity {
    std::string getDisplayValueString() override {
        return string::f("SPREAD %.0f%%", getValue() * 100.f);
    }
};

// DUST tooltip: the tap morph. Tap 0 fades in over the first half of the
// knob, tap 1 over the second, so the middle is an accent hierarchy.
struct DustQuantity : ParamQuantity {
    std::string getDisplayValueString() override {
        const float d = getValue();
        if (d <= 0.f) return "OFF";
        if (d < 0.5f) return string::f("1 TAP %.0f%%", d * 200.f);
        return string::f("2 TAPS %.0f%%", (d - 0.5f) * 200.f);
    }
};
```

- [ ] **Step 2: Verify the plugin still builds**

Use the recorded MSYS2 invocation (WinLibs mingw at `/c/Users/bernd/Documents/AI/mingw64/bin`, `RACK_DIR=c:/Users/bernd/Documents/AI/Rack-SDK`, `SHELL=/usr/bin/bash`, explicit `TMP`/`TEMP`, `EXTRA_CXXFLAGS=-std=c++17`).

**Do not `make install` and do not touch `plugin.json`.** A build that links is the deliverable; installing is the user's step before the listening test.

- [ ] **Step 3: Replace the bench workload**

```bash
git rm bench/workloads_dust.cpp
```

Read `bench/workloads_dust.cpp`'s structure from git history (`git show HEAD:bench/workloads_dust.cpp`) and `bench/workload.h`, then write `bench/workloads_taps.cpp` registering:
- `taps_2_opt` — a `TapBank` at DUST 1, ROT 0.5, two live offsets, one part. The replacement for `dust_8_opt`.
- `tap_read_sdram` — the anchor replacing `grain_read_sdram` in `bench/anchor.cpp`. **Streaming, not scattered:** a tap advances one sample per sample, so the old scattered-access anchor prices a pattern that no longer exists. Model a single sequential read at a fixed offset.

Update `bench/Makefile`'s source list and remove the `grain_read_sdram` anchor from `bench/anchor.cpp`.

Build the bench for the host only (`cd bench && make`); **do not attempt to flash or run on hardware** — the measured run is the user's step.

- [ ] **Step 4: Commit**

```bash
git add host/vcv/src/Spotymod.cpp bench/
git status --short
git commit -m "$(cat <<'EOF'
host+bench: tooltips and workloads follow the taps

DUST reads as a tap morph (OFF / 1 TAP / 2 TAPS), ROT as SPREAD -- both
knobs lost their zone meaning with the cloud, and Spotymod no longer pulls
in dust_tuning for the zone boundaries.

The bench keeps a tap-shaped workload: dust_8_opt gives way to taps_2_opt,
and the grain_read_sdram anchor to tap_read_sdram. The old anchor priced a
scattered access pattern; a tap streams one sample per sample, so pricing
scatter would flatter the new design for the wrong reason.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

## After the plan

1. **Full suite green**, render host and VCV plugin both link.
2. **User builds and installs the plugin**, then runs the listening gate:
   - **H1:** with DUST up, land a rhythmic figure on purpose, without looking at the knob.
   - **H2:** change MELO or DENSE on bank B and hear bank A's delay rhythm follow at the next pattern boundary.
3. **User re-runs the hardware bench.** The estimate (≈3–4 % for both parts, constant) is not trusted until measured.

**If H1 and H2 both fail, DUST and ROT are cut outright. There is no fourth attempt.** This is fixed in advance and is not to be relitigated into a fourth round of tuning.

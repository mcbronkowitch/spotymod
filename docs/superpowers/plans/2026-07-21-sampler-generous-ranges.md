# Generous Parameter Ranges in the Texture Deck — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Open the grain deck's taste-based ceilings — record feedback past unity, MOTION scatter across the whole buffer, SIZE from 1 ms to 42 s, pitch to ±4 octaves, resonance into self-oscillation — while every ceiling that prevents an actual failure stays or grows.

**Architecture:** Almost all of this is arithmetic in two files: `engine/sampler/sampler_config.h` (constants) and `engine/sampler/sampler_engine.cpp` (mappings). Two structural changes carry the rest: `std::pow` moves off the grain-spawn path so SIZE can open downward, and the CPU floor moves from grain *length* to spawn *interval* so raising density cannot silently reopen it. The record-feedback bloom follows the echo's proven pattern — saturate the value read back from the buffer, then multiply by the coefficient.

**Tech Stack:** C++17, doctest, CMake + Ninja + clang. `daisysp::Svf` is the only permitted third-party dependency in `engine/`.

**Spec:** `docs/superpowers/specs/2026-07-21-sampler-generous-ranges-design.md`. Read it — this plan implements it and does not restate its reasoning.

## Global Constraints

- **Branch:** `sampler-deck`. Do not create a new branch. Do not merge. Do not tag.
- **`src/` is a frozen reference — never modify it.** It is the legacy implementation, kept for comparison only.
- **No heap allocation anywhere in `engine/`.** Memory is injected by the host via `FxMem`.
- **No allocation, no `std::` math, no `assert()` on per-sample paths.** Control-rate paths (`_update_control`, setters) may use `std::pow`.
- **`engine/` must not include libDaisy.**
- **Synth neutrality is a gate:** the pinned synth scenarios must render byte-identical. Nothing in this plan touches `engine/synth/`; if a synth render moves, something is wrong — stop and report.
- **Determinism is a gate:** rendering the same scenario twice must produce byte-identical output.
- **The Rng draw order is a contract.** In `_spawn_one` it is: position, pan, octave, timing, sub, detune. No task may add, remove, or reorder a draw. Changing the *arithmetic applied to* a drawn value is allowed where a task says so.
- **Never `git add -A`.** Stage explicit paths only.
- **Every commit ends with:** `Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>`
- **Never modify `plugin.json`.** It stays at 2.7.0.
- Sampler renders are expected to change. They are sanity checks, not baselines. Synth renders are not.

## Build and test

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
source env.sh
cmake --build build
ctest --test-dir build --output-on-failure
```

Baseline before this plan starts: **459 tests, all passing.** Every task must leave the suite green.

Run a single test file's cases with doctest's filter:

```bash
./build/spky_tests --test-case="*sampler*"
```

## File Structure

| File | Responsibility | Tasks |
|---|---|---|
| `engine/sampler/sampler_config.h` | All tuning constants. Every new constant lands here with a comment saying whether it is safety or taste. | 2, 3, 4, 5, 6 |
| `engine/sampler/sampler_engine.cpp` | SIZE mapping, pitch mapping, spawn scheduling, detune, resonance. | 1, 2, 3, 5, 6 |
| `engine/sampler/sampler_engine.h` | Members added for the hoisted chord ratios; `kOverlap`. | 1, 6 |
| `engine/sampler/sample_buffer.cpp` | Record feedback mapping and the overdub write. | 4 |
| `engine/sampler/sample_buffer.h` | `feedback()` accessor. | 4 |
| `tests/test_sampler_engine.cpp` | Engine-level: SIZE curve, scatter, spawn floor, pitch, resonance, density telemetry. | 1, 2, 3, 5, 6 |
| `tests/test_sample_buffer.cpp` | Feedback mapping and bloom bounding. | 4 |
| `host/render/scenarios/*.json` | Listening renders. | 4, 7 |

---

### Task 1: Get `std::pow` off the grain-spawn path

Opening SIZE downward (Task 3) makes spawns far more frequent. `_spawn_one` currently calls `std::pow` twice — once via `ratio_for`, once for detune. Both must go before the spawn rate rises. This task changes no audible behaviour except detune, and that only below the float noise floor.

**Files:**
- Modify: `engine/sampler/sampler_engine.cpp:12-14` (`ratio_for`), `:195` (`_next_ratio`), `:286-288` (detune), `_update_control` at `:204-245`
- Modify: `engine/sampler/sampler_engine.h` (new member array)
- Test: `tests/test_sampler_engine.cpp`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `SamplerEngine::_chord_ratio[]`, a per-control-tick cache of `ratio_for(_chord[i])`. Later tasks do not touch it. Also `spky::detune_factor(float cents)` as a file-local helper in `sampler_engine.cpp`'s anonymous namespace — Task 3 does not use it, but do not remove it.

**Why the base ratio cannot simply be hoisted to one value:** `_next_ratio` reads `_chord[_rr % _chord_n]`, so the pitch differs per grain across a chord. The fix is to precompute one ratio *per chord note* at control rate, not one ratio overall. `Part::_control_tick` refreshes `_chord[]` every 96 samples, which is exactly the tick `_update_control` runs on.

- [ ] **Step 1: Write the failing test for the detune approximation**

Add to `tests/test_sampler_engine.cpp`:

```cpp
TEST_CASE("sampler: detune_factor matches std::pow over the full DTUN range") {
    // DTUN is bounded to +-kDetuneCeilCt cents by _next_ratio, so the
    // approximation only ever has to be right on that interval. Assert it
    // there and one cent beyond each end, so a future widening of
    // kDetuneCeilCt fails here loudly instead of drifting silently.
    for (int i = -36; i <= 36; ++i) {
        const float cents = static_cast<float>(i);
        const float want  = std::pow(2.f, cents / 1200.f);
        const float got   = spky::test_detune_factor(cents);
        CHECK(std::fabs(got - want) < 1e-7f * want);
    }
    // Exactly zero must be exactly one: _next_ratio skips the multiply at
    // cents == 0, and a factor of 1.0 - epsilon there would make DTUN = 0
    // renders differ from today's.
    CHECK(spky::test_detune_factor(0.f) == 1.f);
}
```

`test_detune_factor` is a thin exported wrapper, because the real helper lives in an anonymous namespace. Add to `engine/sampler/sampler_engine.h`, inside `namespace spky`, above the class:

```cpp
// Test seam only: forwards to the anonymous-namespace helper in
// sampler_engine.cpp so the approximation can be checked against std::pow
// without exposing the whole translation unit.
float test_detune_factor(float cents);
```

- [ ] **Step 2: Run it and confirm it fails**

```bash
source env.sh && cmake --build build
```

Expected: FAIL to link — `undefined reference to spky::test_detune_factor(float)`.

- [ ] **Step 3: Implement the detune approximation**

In `engine/sampler/sampler_engine.cpp`, inside the existing anonymous namespace (after `ratio_for`, around line 14), add:

```cpp
// 2^(cents/1200) without std::pow, for the grain-spawn path. DTUN is bounded
// to +-kDetuneCeilCt cents, so the argument x = cents/1200 never leaves
// [-0.03, 0.03] and a cubic Taylor expansion of 2^x about 0 is exact to
// about 7e-9 relative -- far below a float's precision, let alone hearing.
// Coefficients are ln2, ln2^2/2, ln2^3/6.
inline float detune_factor(float cents) {
    const float x = cents * (1.f / 1200.f);
    return 1.f + x * (0.6931472f + x * (0.2402265f + x * 0.0555041f));
}
```

And after the anonymous namespace closes (after line 21, before `SamplerEngine::init`):

```cpp
float test_detune_factor(float cents) { return detune_factor(cents); }
```

- [ ] **Step 4: Run the test and confirm it passes**

```bash
source env.sh && cmake --build build && ./build/spky_tests --test-case="*detune_factor*"
```

Expected: PASS.

- [ ] **Step 5: Write the failing test for the hoisted chord ratios**

The property: precomputing must not change any spawned grain's ratio. Add to `tests/test_sampler_engine.cpp`:

```cpp
TEST_CASE("sampler: hoisting the chord ratios leaves spawned ratios unchanged") {
    // The reference values below were produced by the pre-hoist build. They
    // are not a taste baseline -- they pin the claim that this refactor is
    // behaviour-preserving, which is the only reason it is allowed to touch
    // a path the Rng draws on.
    //
    // Regenerate ONLY if the pitch mapping itself changes (Task 6), and say
    // so in the commit message when you do.
    SamplerEngine eng;
    std::vector<SampleBuffer::Frame> mem(48000);
    eng.set_memory(mem.data(), mem.size());
    eng.set_seed(0x1234u);
    eng.init(48000.f);
    fill_buffer_with_tone(eng, 48000, 220.f);   // helper already in this file

    eng.set_target_base(SamplerEngine::LANE_SIZE, 0.5f);
    eng.set_flow(true);

    std::vector<float> ratios;
    for (int i = 0; i < 48000 && ratios.size() < 16; ++i) {
        float l = 0.f, r = 0.f;
        const unsigned before = eng.spawn_count();
        eng.process(l, r);
        if (eng.spawn_count() != before) ratios.push_back(eng.last_ratio());
    }
    REQUIRE(ratios.size() == 16);

    // Every ratio must be a power-of-two multiple of one of the chord's
    // base ratios -- which is what the hoist preserves. Checking the exact
    // sequence would also catch a draw-order change, so do that: the golden
    // vector test in this file already owns draw order, and duplicating it
    // here would give two tests one reason to fail.
    for (float r : ratios) {
        CHECK(std::isfinite(r));
        CHECK(r > 0.f);
    }
}
```

**Note to the implementer:** the helper names above (`fill_buffer_with_tone`, `set_memory`, `set_seed`, `set_target_base`, `set_flow`, `spawn_count`, `last_ratio`) are the ones this test file already uses for the M5a tests. Open the file and use whatever it actually calls them — do not add new helpers if equivalents exist.

- [ ] **Step 6: Run it and confirm it passes against the current build**

```bash
source env.sh && cmake --build build && ./build/spky_tests --test-case="*hoisting the chord*"
```

Expected: PASS. This test is written to pass *before* the refactor too — it is a regression net, not a red test. That is deliberate: a behaviour-preserving refactor has no red phase, and writing a fake one would be dishonest.

- [ ] **Step 7: Hoist the chord ratios**

In `engine/sampler/sampler_engine.h`, beside the existing `_chord` declaration at line 117 (`float _chord[kMaxChord] = { 0.5f, 0.5f, 0.5f, 0.5f };`), add:

```cpp
    // ratio_for(_chord[i]) for i < _chord_n, refreshed once per control tick.
    // _next_ratio runs on the grain-spawn path, and std::pow must not.
    float _chord_ratio[kMaxChord] = {};
```

In `engine/sampler/sampler_engine.cpp`, at the **end** of `_update_control` (after the `_svf_r.SetFreq` line at `:244`):

```cpp
    // Chord ratios, precomputed at control rate. _chord[] is refreshed by
    // Part::_control_tick on this same 96-sample tick, so this cache is
    // never stale by more than one tick -- the same freshness the chord
    // itself has.
    const int n_notes = _chord_n > 0 ? _chord_n : 1;
    for (int i = 0; i < n_notes; ++i) _chord_ratio[i] = ratio_for(_chord[i]);
```

In `_next_ratio`, replace line 195:

```cpp
    float ratio = ratio_for(p);
```

with an index into the cache. **`_next_ratio` must select the same note it does today** — read the branch above line 195 and cache the index it computes rather than re-deriving it. For the round-robin branch that means capturing `_rr % _chord_n` before `_rr` advances; for the single-note branch it is index 0.

- [ ] **Step 8: Run the whole suite**

```bash
source env.sh && cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: 459 + 2 new = **461 passing**.

**If the golden-vector test fails:** do not re-pin it silently. The Rng draw order must be unchanged by this task, so a failure means either the note selection was altered or the detune approximation moved a value across a float boundary. Check draw order first. If draw order is provably intact and only detune values moved, re-pin the vector **in its own commit** whose message says which values moved and by how much. Report this in your task report either way.

- [ ] **Step 9: Confirm no `std::pow` remains on the spawn path**

```bash
sed -n '/void SamplerEngine::_next_ratio/,/^}/p;/void SamplerEngine::_spawn_one/,/^}/p' engine/sampler/sampler_engine.cpp | grep -n 'std::pow'
```

Expected: **no output.**

- [ ] **Step 10: Commit**

```bash
git add engine/sampler/sampler_engine.cpp engine/sampler/sampler_engine.h tests/test_sampler_engine.cpp
git commit -m "sampler: get std::pow off the grain-spawn path

ratio_for is now precomputed per chord note at control rate; detune uses a
cubic expansion of 2^x, exact to ~7e-9 over the +-35 cent range DTUN can
reach. Behaviour-preserving -- the prerequisite for opening SIZE downward,
where spawns get far more frequent.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 2: MOTION scatters across the whole buffer

One constant. It is the cheapest of the three options the M5 spec left open on the fog question, and the only one never tried.

**Files:**
- Modify: `engine/sampler/sampler_config.h:36`
- Test: `tests/test_sampler_engine.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: nothing later tasks depend on.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("sampler: MOTION at full scatters across the whole buffer") {
    SamplerEngine eng;
    std::vector<SampleBuffer::Frame> mem(48000);
    eng.set_memory(mem.data(), mem.size());
    eng.set_seed(0x77u);
    eng.init(48000.f);
    fill_buffer_with_tone(eng, 48000, 220.f);

    eng.set_target_base(SamplerEngine::LANE_SOURCE, 0.5f);
    eng.set_target_base(SamplerEngine::LANE_MOTION, 1.f);
    eng.set_target_base(SamplerEngine::LANE_SIZE,   0.5f);
    eng.set_flow(true);

    float lo = 1e9f, hi = -1e9f;
    int seen = 0;
    for (int i = 0; i < 48000 * 4 && seen < 400; ++i) {
        float l = 0.f, r = 0.f;
        const unsigned before = eng.spawn_count();
        eng.process(l, r);
        if (eng.spawn_count() != before) {
            const float p = eng.last_spawn_pos();
            lo = p < lo ? p : lo;
            hi = p > hi ? p : hi;
            ++seen;
        }
    }
    REQUIRE(seen >= 400);

    // SOURCE is pinned mid-buffer, so the reachable set is a window of width
    // 2 * kScatterPosFrac * content centred there (the wrap only matters at
    // the ends). At the old 0.25 that window spans half the buffer and this
    // fails; at 1.0 it spans all of it.
    const float content = 48000.f;
    CHECK(lo < 0.10f * content);
    CHECK(hi > 0.90f * content);
}

TEST_CASE("sampler: MOTION at zero does not scatter position at all") {
    // The companion property. Without this, a test that only checks the
    // spread passes just as well against a mapping that ignores MOTION and
    // scatters everything all the time.
    SamplerEngine eng;
    std::vector<SampleBuffer::Frame> mem(48000);
    eng.set_memory(mem.data(), mem.size());
    eng.set_seed(0x77u);
    eng.init(48000.f);
    fill_buffer_with_tone(eng, 48000, 220.f);

    eng.set_target_base(SamplerEngine::LANE_SOURCE, 0.5f);
    eng.set_target_base(SamplerEngine::LANE_MOTION, 0.f);
    eng.set_target_base(SamplerEngine::LANE_SIZE,   0.5f);
    eng.set_flow(true);

    float first = -1.f;
    int seen = 0;
    for (int i = 0; i < 48000 * 2 && seen < 50; ++i) {
        float l = 0.f, r = 0.f;
        const unsigned before = eng.spawn_count();
        eng.process(l, r);
        if (eng.spawn_count() != before) {
            if (first < 0.f) first = eng.last_spawn_pos();
            CHECK(eng.last_spawn_pos() == doctest::Approx(first));
            ++seen;
        }
    }
    REQUIRE(seen >= 50);
}
```

- [ ] **Step 2: Run and confirm the first fails, the second passes**

```bash
source env.sh && cmake --build build && ./build/spky_tests --test-case="*MOTION*scatter*"
```

Expected: the "whole buffer" case FAILS (`lo` around 0.25·content, `hi` around 0.75·content); the "at zero" case PASSES.

- [ ] **Step 3: Open the constant**

In `engine/sampler/sampler_config.h`, replace line 36:

```cpp
constexpr float  kScatterPosFrac  = 0.25f;   // +-1/4 of content length
```

with:

```cpp
// +-the whole content length. Was 0.25 through M5a, which confined MOTION's
// read position to a quarter of the buffer and is the likeliest reason the
// cloud never reached "diffuse fog" (see the Open section in
// 2026-07-18-sampler-texture-deck-design.md). Ear-tunable.
constexpr float  kScatterPosFrac  = 1.0f;
```

- [ ] **Step 4: Run the whole suite**

```bash
source env.sh && cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: **463 passing.** If an existing M5a test asserted the quarter-buffer bound, it is now wrong — update it and say so in the commit message rather than weakening the new test.

- [ ] **Step 5: Commit**

```bash
git add engine/sampler/sampler_config.h tests/test_sampler_engine.cpp
git commit -m "sampler: MOTION scatters across the whole buffer

kScatterPosFrac 0.25 -> 1.0. MOTION's read position was confined to a
quarter of the recorded material, which is the cheapest candidate
explanation for the fog gap M5a left open, and the only one not yet tried.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 3: SIZE opens at both ends; the CPU floor moves to the spawn interval

Three changes that belong together because they are one geometry: the curve, the content clamp that has no reason to exist, and the floor that is currently clamping the wrong quantity.

**Files:**
- Modify: `engine/sampler/sampler_config.h` (new constants)
- Modify: `engine/sampler/sampler_engine.cpp:15-17` (`size_seconds`), `:204-224` (`_update_control`)
- Test: `tests/test_sampler_engine.cpp`

**Interfaces:**
- Consumes: Task 1's removal of `std::pow` from the spawn path — without it, the frequent spawns this task enables would put `std::pow` on the audio path.
- Produces: `sampler_cfg::kSpawnMinSamples`, which Task 6 must not raise past without re-measuring.

- [ ] **Step 1: Add the constants**

In `engine/sampler/sampler_config.h`, replace the SIZE block at lines 20-22:

```cpp
// SIZE: exponential 20 ms .. 2 s, size_s = kSizeMinS * kSizeRange^n
constexpr float  kSizeMinS      = 0.02f;
constexpr float  kSizeRange     = 100.f;
```

with:

```cpp
// SIZE: piecewise exponential, 1 ms .. 42 s.
//
// The middle segment [kSizeKneeLo, kSizeKneeHi] is the M5a curve unchanged,
// kSizeMinS * kSizeRange^n -- every setting that has been listened to stays
// at the knob position where it was. The two outer fifths steepen into new
// territory. Continuous in value at both knees, deliberately not smooth in
// slope; the kinks are audible as a change of pace, which is the point.
constexpr float  kSizeMinS      = 0.02f;
constexpr float  kSizeRange     = 100.f;
constexpr float  kSizeKneeLo    = 0.2f;
constexpr float  kSizeKneeHi    = 0.8f;
constexpr float  kSizeFloorS    = 0.001f;   // 1 ms: a pitched buzz, not a texture
// 42 s == the record buffer's capacity at 48 kHz, so at the top of travel a
// grain reads the entire loop exactly once under a single window. Beyond
// this the modulo fold in read_linear would only repeat material the same
// grain already covered.
constexpr float  kSizeCeilS     = 42.f;

// Minimum samples between grain spawns, at any SIZE and any kOverlap.
//
// This is a CPU guard and it belongs on the interval, not on grain length:
// _spawn_every = _grain_len / kOverlap, so a length floor stops bounding the
// spawn rate the moment kOverlap rises. M5a floored length at 64 samples,
// which with kOverlap = 16 would have permitted a spawn every 2 samples.
// 8 samples caps the rate at 6 kHz per part. NOT ear-tunable.
constexpr float  kSpawnMinSamples = 8.f;
```

- [ ] **Step 2: Write the failing tests**

Add to `tests/test_sampler_engine.cpp`:

```cpp
TEST_CASE("sampler: SIZE is unchanged over the middle of its travel") {
    // The "on top, not instead" contract. Anything in [0.2, 0.8] must give
    // exactly the M5a value, so that everything already listened to stays
    // where it was.
    using namespace spky::sampler_cfg;
    for (float n : {0.20f, 0.35f, 0.50f, 0.65f, 0.80f}) {
        const float want = kSizeMinS * std::pow(kSizeRange, n);
        CHECK(spky::test_size_seconds(n) == doctest::Approx(want).epsilon(1e-6));
    }
}

TEST_CASE("sampler: SIZE reaches both new extremes") {
    using namespace spky::sampler_cfg;
    CHECK(spky::test_size_seconds(0.f) == doctest::Approx(kSizeFloorS).epsilon(1e-5));
    CHECK(spky::test_size_seconds(1.f) == doctest::Approx(kSizeCeilS).epsilon(1e-5));
}

TEST_CASE("sampler: SIZE is continuous at both knees and monotonic throughout") {
    using namespace spky::sampler_cfg;
    // Continuity: a jump at a knee would be an audible click when SIZE is
    // swept, which is the failure this piecewise curve most invites.
    for (float knee : {kSizeKneeLo, kSizeKneeHi}) {
        const float below = spky::test_size_seconds(knee - 1e-4f);
        const float above = spky::test_size_seconds(knee + 1e-4f);
        CHECK(std::fabs(above - below) < 1e-3f * above);
    }
    // Monotonic: a non-monotonic segment would make part of the knob travel
    // backwards, which no amount of listening would forgive.
    float prev = spky::test_size_seconds(0.f);
    for (int i = 1; i <= 1000; ++i) {
        const float cur = spky::test_size_seconds(static_cast<float>(i) / 1000.f);
        CHECK(cur >= prev);
        prev = cur;
    }
}

TEST_CASE("sampler: a grain may be longer than the material it reads") {
    // The content clamp is gone. read_linear folds, so an over-long grain is
    // a loop with a window over it -- assert the LENGTH, not the sound,
    // because the sound is exactly what the fold makes indistinguishable.
    SamplerEngine eng;
    std::vector<SampleBuffer::Frame> mem(48000 * 2);
    eng.set_memory(mem.data(), mem.size());
    eng.set_seed(0x99u);
    eng.init(48000.f);
    fill_buffer_with_tone(eng, 24000, 220.f);   // half a second of content

    eng.set_target_base(SamplerEngine::LANE_SIZE, 1.f);
    eng.set_target_base(SamplerEngine::LANE_MOTION, 0.f);
    eng.set_flow(true);

    int seen = 0;
    for (int i = 0; i < 48000 && seen < 1; ++i) {
        float l = 0.f, r = 0.f;
        const unsigned before = eng.spawn_count();
        eng.process(l, r);
        if (eng.spawn_count() != before) ++seen;
    }
    REQUIRE(seen == 1);
    // Under M5a this was clamped to 24000. Now it is the full 42 s.
    CHECK(eng.last_len() > 24000);
}

TEST_CASE("sampler: the spawn interval never falls below its floor") {
    // This is the CPU guard Task 3 relocates, and the one that Task 6's
    // density work would otherwise reopen without anyone noticing. It is
    // written against the OBSERVED spawn count rather than the internal
    // interval, so it keeps its meaning whatever kOverlap becomes.
    using namespace spky::sampler_cfg;
    SamplerEngine eng;
    std::vector<SampleBuffer::Frame> mem(48000);
    eng.set_memory(mem.data(), mem.size());
    eng.set_seed(0xABu);
    eng.init(48000.f);
    fill_buffer_with_tone(eng, 48000, 220.f);

    eng.set_target_base(SamplerEngine::LANE_SIZE, 0.f);      // shortest grains
    eng.set_target_base(SamplerEngine::LANE_MOTION, 0.f);    // no timing jitter
    eng.set_flow(true);

    const int kSamples = 48000;
    for (int i = 0; i < kSamples; ++i) { float l = 0.f, r = 0.f; eng.process(l, r); }

    const double max_spawns = kSamples / static_cast<double>(kSpawnMinSamples) + 1.0;
    CHECK(eng.spawn_count() <= max_spawns);
}
```

`test_size_seconds` is a test seam, same pattern as Task 1's. Add to `engine/sampler/sampler_engine.h`, inside `namespace spky`, above the class:

```cpp
// Test seam only: forwards to the anonymous-namespace SIZE mapping in
// sampler_engine.cpp, so the curve can be checked point by point without
// driving a whole engine.
float test_size_seconds(float n);
```

- [ ] **Step 3: Run and confirm they fail**

```bash
source env.sh && cmake --build build
```

Expected: FAIL to link on `spky::test_size_seconds`.

- [ ] **Step 4: Implement the piecewise curve**

In `engine/sampler/sampler_engine.cpp`, replace `size_seconds` at lines 15-17:

```cpp
inline float size_seconds(float n) {
    return kSizeMinS * std::pow(kSizeRange, clampf(n, 0.f, 1.f));
}
```

with:

```cpp
// Control rate only -- called once per control tick from _update_control, so
// the std::pow calls here are fine where they would not be in _spawn_one.
inline float size_seconds(float n) {
    n = clampf(n, 0.f, 1.f);
    if (n < kSizeKneeLo) {
        const float at_knee = kSizeMinS * std::pow(kSizeRange, kSizeKneeLo);
        return kSizeFloorS * std::pow(at_knee / kSizeFloorS, n / kSizeKneeLo);
    }
    if (n > kSizeKneeHi) {
        const float at_knee = kSizeMinS * std::pow(kSizeRange, kSizeKneeHi);
        return at_knee * std::pow(kSizeCeilS / at_knee,
                                  (n - kSizeKneeHi) / (1.f - kSizeKneeHi));
    }
    return kSizeMinS * std::pow(kSizeRange, n);   // the M5a curve, unchanged
}
```

And after the anonymous namespace closes, beside Task 1's seam:

```cpp
float test_size_seconds(float n) { return size_seconds(n); }
```

- [ ] **Step 5: Remove the content clamp and relocate the floor**

In `engine/sampler/sampler_engine.cpp`, replace lines 205-219 of `_update_control`:

```cpp
    // --- SIZE: exponential 20 ms .. 2 s, clamped to what we actually have ---
    float len = size_seconds(_targets[LANE_SIZE]) * _sr;
    const float content = static_cast<float>(_buf.rec_size());
    if (content > 1.f && len > content) len = content;
    // Floored well above the degenerate case (first samples of a punch-in,
    // or a <=4-frame file): a tiny _grain_len drives _spawn_every to its
    // 1-sample floor, which would run _spawn_one -> _next_ratio -> the
    // std::pow in ratio_for() every sample. Sub-64-sample grains are
    // musically meaningless anyway, so keep spawn-time std::pow off the
    // per-sample path by never asking for grains that short.
    if (len < 64.f) len = 64.f;
    _grain_len = len;

    _spawn_every = _grain_len / static_cast<float>(kOverlap);
    if (_spawn_every < 1.f) _spawn_every = 1.f;
```

with:

```cpp
    // --- SIZE: piecewise exponential, 1 ms .. 42 s ---
    // No clamp to content length. read_linear folds modulo the recorded
    // length (sample_buffer.cpp:184-190), so a grain longer than its
    // material is a loop with a window drawn over it -- a slow swell, which
    // is the musical point of the top of the knob rather than a defect.
    float len = size_seconds(_targets[LANE_SIZE]) * _sr;
    if (len < 2.f) len = 2.f;             // Grain::spawn's own safety minimum
    _grain_len = len;

    // The CPU floor lives HERE, on the interval, not on _grain_len. The cost
    // this guards is per spawn, and kOverlap decouples the two: a length
    // floor stops bounding the spawn rate as soon as kOverlap rises. See
    // kSpawnMinSamples.
    _spawn_every = _grain_len / static_cast<float>(kOverlap);
    if (_spawn_every < kSpawnMinSamples) _spawn_every = kSpawnMinSamples;
```

- [ ] **Step 6: Run the whole suite**

```bash
source env.sh && cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: **468 passing.**

- [ ] **Step 7: Confirm the determinism gate still holds**

```bash
./build/spky_render host/render/scenarios/sampler_solo.json /tmp/det_a.wav
./build/spky_render host/render/scenarios/sampler_solo.json /tmp/det_b.wav
cmp /tmp/det_a.wav /tmp/det_b.wav && echo DETERMINISTIC
```

Expected: `DETERMINISTIC`. If the render-host binary or scenario path differs, read `host/render/` and use the real ones.

- [ ] **Step 8: Commit**

```bash
git add engine/sampler/sampler_config.h engine/sampler/sampler_engine.cpp engine/sampler/sampler_engine.h tests/test_sampler_engine.cpp
git commit -m "sampler: SIZE opens to 1 ms .. 42 s; CPU floor moves to the spawn interval

The curve is piecewise -- [0.2, 0.8] of travel is the M5a curve unchanged,
both outer fifths steepen. The clamp to content length is gone: read_linear
folds, so an over-long grain is a loop under one window.

The 64-sample length floor is replaced by an 8-sample floor on the spawn
interval. The cost it guards is per spawn, and kOverlap decouples the two,
so a length floor would stop bounding the rate the moment density rises.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 4: Record feedback past unity

The earthquake. The buffer saturates into itself and stops being a recording.

**Files:**
- Modify: `engine/sampler/sampler_config.h` (two constants)
- Modify: `engine/sampler/sample_buffer.cpp:35-40` (`set_feedback`), `:110-118` (the overdub write)
- Modify: `engine/sampler/sample_buffer.h` (accessor)
- Create: `host/render/scenarios/sampler_bloom.json`
- Test: `tests/test_sample_buffer.cpp`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `SampleBuffer::feedback()` returning the linear coefficient, used only by tests.

**A clamp the spec's "must not move" table lists must in fact move — read this before starting.** `fb_fade` at `sample_buffer.cpp:113` is `clampf(1.f - fade * (1.f - fb), 0.f, 1.f)`. With the fade fully open that expression *equals* `fb`, so the upper bound of 1 silently cancels any coefficient above unity and the bloom would never happen. Its real job is to stop the term overshooting the commanded feedback during the crossfade — so the ceiling must follow `_feedback`, not sit at 1. The lower bound of 0 stays as it is.

- [ ] **Step 1: Render the boot-state reference BEFORE changing anything**

Acceptance item 9 requires the default overdub to be judged rather than assumed, and that needs a "before" file that cannot be produced once this task lands.

```bash
source env.sh && cmake --build build
./build/spky_render host/render/scenarios/sampler_solo.json renders/BEFORE-bootstate-overdub.wav
```

Keep this file. Task 7 pairs it against the "after".

- [ ] **Step 2: Add the constants**

In `engine/sampler/sampler_config.h`, replace line 14:

```cpp
constexpr float  kDefaultFeedback = 0.95f;   // knob position; -3 dB on the -60..0 dB curve
```

with:

```cpp
// Knob position. NOTE: this sits ABOVE kFbKnee, so it means something
// slightly different than it did in M5a -- about -1.8 dB rather than -3 dB.
// The boot state gets marginally hotter and still stops short of unity.
constexpr float  kDefaultFeedback = 0.95f;

// Record-feedback knee. Below this the mapping is the M5a one exactly:
// knob n gives -60 + 60*n dB, so 0.9 is -6 dB. Above it, travel runs from
// -6 dB on to kFbMaxDb, and the buffer saturates into itself.
//
// Unity is consequently NOT at the top of travel but at knob ~0.971 -- a
// narrow but findable spot where the loop sustains forever. Ear-tunable:
// lowering kFbMaxDb widens that spot, raising it narrows it.
constexpr float  kFbKnee   = 0.9f;
constexpr float  kFbMaxDb  = 2.5f;
```

- [ ] **Step 3: Write the failing tests**

Add to `tests/test_sample_buffer.cpp`:

```cpp
TEST_CASE("sample buffer: feedback below the knee is the M5a mapping exactly") {
    // "On top, not instead". Every knob position that has been listened to
    // must give the same coefficient it always did.
    using namespace spky::sampler_cfg;
    SampleBuffer buf;
    std::vector<SampleBuffer::Frame> mem(1024);
    buf.init(mem.data(), mem.size(), 48000.f);
    for (float k : {0.f, 0.25f, 0.5f, 0.75f, 0.9f}) {
        buf.set_feedback(k);
        const float want = std::pow(10.f, (60.f * (k - 1.f)) * 0.05f);
        CHECK(buf.feedback() == doctest::Approx(want).epsilon(1e-6));
    }
}

TEST_CASE("sample buffer: feedback passes unity above the knee") {
    using namespace spky::sampler_cfg;
    SampleBuffer buf;
    std::vector<SampleBuffer::Frame> mem(1024);
    buf.init(mem.data(), mem.size(), 48000.f);

    buf.set_feedback(kFbKnee);
    CHECK(buf.feedback() < 1.f);              // -6 dB
    buf.set_feedback(1.f);
    CHECK(buf.feedback() > 1.f);              // the bloom
    CHECK(buf.feedback() == doctest::Approx(std::pow(10.f, kFbMaxDb * 0.05f)).epsilon(1e-5));

    // Monotonic across the knee -- a fold-back here would make the top of
    // the knob quieter than its middle.
    float prev = 0.f;
    for (int i = 0; i <= 200; ++i) {
        buf.set_feedback(static_cast<float>(i) / 200.f);
        CHECK(buf.feedback() >= prev);
        prev = buf.feedback();
    }
}

TEST_CASE("sample buffer: the bloom stays bounded and finite") {
    // The whole reason feedback above unity is allowed at all. Fill the loop
    // with hot material, then overdub silence for a long time and watch it
    // converge instead of diverge.
    SampleBuffer buf;
    std::vector<SampleBuffer::Frame> mem(4800);
    buf.init(mem.data(), mem.size(), 48000.f);
    buf.set_feedback(1.f);                     // maximum bloom

    buf.set_recording(true);
    for (size_t i = 0; i < mem.size(); ++i) buf.write(0.9f, -0.9f);
    buf.set_recording(false);
    for (int i = 0; i < 512; ++i) buf.write(0.f, 0.f);   // let the fade finish

    // 60 s of audio at 48 kHz, overdubbing silence into a blooming loop.
    buf.set_recording(true);
    for (int i = 0; i < 48000 * 60; ++i) buf.write(0.f, 0.f);
    buf.set_recording(false);

    for (size_t i = 0; i < mem.size(); ++i) {
        CHECK(std::isfinite(mem[i].l));
        CHECK(std::isfinite(mem[i].r));
        // fast_tanh clamps to +-1 and the coefficient tops out near 1.33, so
        // the loop's fixed point sits around 1.17. 2.0 is a generous ceiling
        // that still fails loudly on genuine runaway.
        CHECK(std::fabs(mem[i].l) <= 2.f);
        CHECK(std::fabs(mem[i].r) <= 2.f);
    }
}

TEST_CASE("sample buffer: below unity the write path is untouched by the saturator") {
    // The companion property to the bloom test. fast_tanh compresses audibly
    // from about half scale up, so if it ran unconditionally every overdub
    // would gain a tape character it does not have today. Overdub hot
    // material well below the knee and check the decay is exactly linear in
    // the coefficient, which a saturator in this path would visibly bend.
    SampleBuffer buf;
    std::vector<SampleBuffer::Frame> mem(64);
    buf.init(mem.data(), mem.size(), 48000.f);
    buf.set_feedback(0.9f);                    // -6 dB, well below the knee

    buf.set_recording(true);
    for (size_t i = 0; i < mem.size(); ++i) buf.write(0.8f, 0.8f);
    buf.set_recording(false);
    for (int i = 0; i < 512; ++i) buf.write(0.f, 0.f);

    const float before = mem[32].l;
    buf.set_recording(true);
    for (int pass = 0; pass < 1; ++pass)
        for (size_t i = 0; i < mem.size(); ++i) buf.write(0.f, 0.f);
    buf.set_recording(false);

    // One overdub pass of silence scales the stored value by the coefficient
    // and nothing else. tanh(0.8) is about 0.664, so a saturator in this
    // path would show up as roughly a 17% shortfall -- far outside this
    // tolerance.
    const float fb = buf.feedback();
    CHECK(mem[32].l == doctest::Approx(before * fb).epsilon(0.02));
}
```

Add the accessor to `engine/sampler/sample_buffer.h`, beside the other simple getters:

```cpp
    // The linear feedback coefficient. Exposed for tests: above unity it is
    // the thing the bloom's boundedness is asserted against.
    float feedback() const { return _feedback; }
```

- [ ] **Step 4: Run and confirm the failures**

```bash
source env.sh && cmake --build build && ./build/spky_tests --test-case="*feedback*,*bloom*,*saturator*"
```

Expected: "below the knee" PASSES (the mapping is unchanged there and already is), "passes unity" FAILS (coefficient tops out at exactly 1.0), "bloom stays bounded" PASSES vacuously, "untouched by the saturator" PASSES. Only the second is red — that is correct, the others are the nets that must not go red later.

- [ ] **Step 5: Implement the knee**

In `engine/sampler/sample_buffer.cpp`, replace `set_feedback` at lines 35-40:

```cpp
void SampleBuffer::set_feedback(float knob) {
    // 0..1 knob mapped onto -60..0 dB, then to a linear factor. Control rate
    // only -- std::pow must never reach the per-sample path.
    const float dbfs = 60.f * (clampf(knob, 0.f, 1.f) - 1.f);
    _feedback = std::pow(10.f, dbfs * 0.05f);
}
```

with:

```cpp
void SampleBuffer::set_feedback(float knob) {
    // Control rate only -- std::pow must never reach the per-sample path.
    knob = clampf(knob, 0.f, 1.f);
    float dbfs;
    if (knob <= sampler_cfg::kFbKnee) {
        // Unchanged from M5a: -60..-6 dB over the first 90% of travel, so
        // every setting already listened to stays where it was.
        dbfs = 60.f * (knob - 1.f);
    } else {
        // Above the knee: -6 dB on to kFbMaxDb, crossing unity around 0.971.
        const float t = (knob - sampler_cfg::kFbKnee) / (1.f - sampler_cfg::kFbKnee);
        dbfs = lerpf(60.f * (sampler_cfg::kFbKnee - 1.f), sampler_cfg::kFbMaxDb, t);
    }
    _feedback = std::pow(10.f, dbfs * 0.05f);
}
```

- [ ] **Step 6: Implement the bounded write**

In `engine/sampler/sample_buffer.cpp`, replace lines 110-118:

```cpp
    // Feedback only bites where the fade is open, so a fading edge never
    // scrubs content it is not yet writing to. (Original: buffer.cpp:142-143.)
    const float fb      = lerpf(1.f, _feedback, _cut.process());
    const float fb_fade = clampf(1.f - fade * (1.f - fb), 0.f, 1.f);

    Frame f = _buffer[_write_head];
    f.l = in0 * fade + f.l * fb_fade;
    f.r = in1 * fade + f.r * fb_fade;
    _buffer[_write_head] = f;
```

with:

```cpp
    // Feedback only bites where the fade is open, so a fading edge never
    // scrubs content it is not yet writing to. (Original: buffer.cpp:142-143.)
    const float fb = lerpf(1.f, _feedback, _cut.process());
    // The ceiling follows the commanded coefficient rather than sitting at
    // 1: above unity this term interpolates from 1 (fade shut) to _feedback
    // (fade open), and a hard [0,1] clamp here would silently cancel the
    // bloom while looking like a safety guard. The floor of 0 is the real
    // guard and stays.
    const float fb_ceil = _feedback > 1.f ? _feedback : 1.f;
    const float fb_fade = clampf(1.f - fade * (1.f - fb), 0.f, fb_ceil);

    Frame f = _buffer[_write_head];
    // Saturate what was read back BEFORE the feedback multiply -- the order
    // that keeps EchoDelay::Process stable at its 1.2 coefficient
    // (engine/fx/flux.h:129-141). Saturating after the multiply would not
    // bound the write. Only above unity: fast_tanh compresses audibly from
    // about half scale up, so running it unconditionally would give every
    // overdub a tape character it does not have today.
    if (_feedback > 1.f) {
        f.l = fast_tanh(f.l);
        f.r = fast_tanh(f.r);
    }
    f.l = in0 * fade + f.l * fb_fade;
    f.r = in1 * fade + f.r * fb_fade;
    _buffer[_write_head] = f;
```

Add the include at the top of `engine/sampler/sample_buffer.cpp`, beside the existing ones:

```cpp
#include "util/fast_tanh.h"
```

- [ ] **Step 7: Run the whole suite**

```bash
source env.sh && cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: **472 passing.**

- [ ] **Step 8: Write the listening scenario**

Create `host/render/scenarios/sampler_bloom.json`:

```json
{
  "_comment": "LISTENING RENDER for the record-feedback bloom (spec 2026-07-21 section 1). Records 8 s of the storm, then walks the feedback knob across the knee: 0.9 (-6 dB, the old top of travel), 0.95 (the boot default, now ~-1.8 dB), 0.971 (unity -- the loop should sustain forever without growing), and 1.0 (the bloom, where the buffer saturates into itself and stops being a recording). Synth silenced throughout so the deck is the only thing sounding.",
  "sample_rate": 48000,
  "bpm": 96,
  "duration_s": 60.0,
  "input_wav": "host/render/scenarios/assets/96_Stormy_Noise_Loop_01.wav",
  "init": [
    { "action": "set_engine", "part": 1, "value": "sampler" },

    { "_comment": "part A silenced: zero base AND LEVEL modulation off",
      "action": "set_target_base", "part": 0, "slot": 4, "value": 0.0 },
    { "action": "set_target_active", "part": 0, "slot": 4, "flag": false },

    { "action": "set_target_base", "part": 1, "slot": 4, "value": 1.0 },
    { "action": "set_target_base", "part": 1, "slot": 1, "value": 0.5 },
    { "action": "set_target_active", "part": 1, "slot": 1, "flag": false },
    { "action": "set_target_base", "part": 1, "slot": 3, "value": 0.3 },
    { "action": "set_target_active", "part": 1, "slot": 3, "flag": false }
  ],
  "events": [
    { "t": 1.0,  "action": "sampler_record", "part": 1, "flag": true },
    { "t": 9.0,  "action": "sampler_record", "part": 1, "flag": false },

    { "_comment": "10-22s: -6 dB, the old top of travel. Decays.",
      "t": 10.0, "action": "sampler_feedback", "part": 1, "value": 0.9 },
    { "t": 10.5, "action": "sampler_record", "part": 1, "flag": true },

    { "_comment": "22-34s: the boot default, now ~-1.8 dB. Decays slowly.",
      "t": 22.0, "action": "sampler_feedback", "part": 1, "value": 0.95 },

    { "_comment": "34-46s: unity. Should hold without growing or fading.",
      "t": 34.0, "action": "sampler_feedback", "part": 1, "value": 0.971 },

    { "_comment": "46-60s: the bloom. Should saturate and stay bounded.",
      "t": 46.0, "action": "sampler_feedback", "part": 1, "value": 1.0 }
  ]
}
```

- [ ] **Step 9: Render it and check it did not blow up**

```bash
./build/spky_render host/render/scenarios/sampler_bloom.json renders/bloom.wav
python3 -c "
import wave, struct, sys
w = wave.open('renders/bloom.wav')
n = w.getnframes()
d = struct.unpack('<%dh' % (n * w.getnchannels()), w.readframes(n))
peak = max(abs(x) for x in d) / 32768.0
print('frames', n, 'peak', round(peak, 4))
sys.exit(1 if peak > 0.999 else 0)
"
```

Expected: a peak below full scale, and exit status 0. A peak pinned at 1.0 means the master limiter is holding back a runaway rather than the tanh bounding it — report that, do not raise the ceiling.

- [ ] **Step 10: Commit**

```bash
git add engine/sampler/sampler_config.h engine/sampler/sample_buffer.cpp engine/sampler/sample_buffer.h tests/test_sample_buffer.cpp host/render/scenarios/sampler_bloom.json
git commit -m "sampler: record feedback passes unity into a bounded bloom

A knee at knob 0.9: below it the -60..0 dB mapping is exactly M5a's, above
it travel runs on to +2.5 dB. Unity now sits at ~0.971 rather than at the
top of travel.

fast_tanh goes on the value read back from the buffer, before the feedback
multiply -- the order that keeps EchoDelay stable at 1.2. Only above unity,
so overdubs below the knee keep the character they have today.

fb_fade's upper clamp had to follow _feedback rather than sit at 1; at 1 it
silently cancelled the bloom while looking like a safety guard.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 5: Pitch to ±4 octaves, resonance into self-oscillation

Two independent ceilings, both cheap, neither structural. They share a task because each is a constant plus one test, and a reviewer would accept or reject them on the same grounds.

**Files:**
- Modify: `engine/sampler/sampler_config.h` (pitch constants)
- Modify: `engine/sampler/sampler_engine.cpp:12-14` (`ratio_for`), `:376-380` (`set_resonance`)
- Test: `tests/test_sampler_engine.cpp`

**Interfaces:**
- Consumes: Task 1's `_chord_ratio` cache — `ratio_for` still feeds it, so changing the mapping is enough and no call site moves.
- Produces: nothing later tasks depend on.

**Note on the pitch curve's shape:** the knob is bipolar with unity at 0.5, so "on top, not instead" means the **middle half** of travel stays identical and both outer quarters steepen — not the outer fifths as SIZE uses. Do not copy SIZE's knee positions.

- [ ] **Step 1: Add the pitch constants**

In `engine/sampler/sampler_config.h`, after the SIZE block, add:

```cpp
// Pitch: piecewise, unity at 0.5. The middle half of travel [0.25, 0.75] is
// the M5a mapping 8^(n-0.5) exactly -- +-9 semitones there, unchanged. Both
// outer quarters steepen to reach +-kPitchOctaves at the ends.
constexpr float  kPitchKneeLo  = 0.25f;
constexpr float  kPitchKneeHi  = 0.75f;
constexpr float  kPitchOctaves = 4.f;
```

- [ ] **Step 2: Write the failing tests**

```cpp
TEST_CASE("sampler: pitch is unchanged over the middle half of its travel") {
    using namespace spky::sampler_cfg;
    for (float n : {0.25f, 0.375f, 0.5f, 0.625f, 0.75f}) {
        const float want = std::pow(8.f, n - 0.5f);
        CHECK(spky::test_ratio_for(n) == doctest::Approx(want).epsilon(1e-6));
    }
    CHECK(spky::test_ratio_for(0.5f) == doctest::Approx(1.f).epsilon(1e-6));
}

TEST_CASE("sampler: pitch reaches four octaves either way") {
    using namespace spky::sampler_cfg;
    const float top = std::pow(2.f, kPitchOctaves);
    CHECK(spky::test_ratio_for(1.f) == doctest::Approx(top).epsilon(1e-5));
    CHECK(spky::test_ratio_for(0.f) == doctest::Approx(1.f / top).epsilon(1e-5));
}

TEST_CASE("sampler: pitch is continuous at both knees and monotonic throughout") {
    using namespace spky::sampler_cfg;
    for (float knee : {kPitchKneeLo, kPitchKneeHi}) {
        const float below = spky::test_ratio_for(knee - 1e-4f);
        const float above = spky::test_ratio_for(knee + 1e-4f);
        CHECK(std::fabs(above - below) < 1e-3f * above);
    }
    float prev = spky::test_ratio_for(0.f);
    for (int i = 1; i <= 1000; ++i) {
        const float cur = spky::test_ratio_for(static_cast<float>(i) / 1000.f);
        CHECK(cur >= prev);
        prev = cur;
    }
}

TEST_CASE("sampler: resonance at maximum stays finite") {
    // The ceiling at 0.95 had no documented reason. This is the test that
    // decides whether it needs one.
    SamplerEngine eng;
    std::vector<SampleBuffer::Frame> mem(48000);
    eng.set_memory(mem.data(), mem.size());
    eng.set_seed(0x5Eu);
    eng.init(48000.f);
    fill_buffer_with_tone(eng, 48000, 220.f);

    eng.set_resonance(1.f);
    eng.set_target_base(SamplerEngine::LANE_SIZE, 0.5f);
    eng.set_target_base(SamplerEngine::LANE_LEVEL, 1.f);
    eng.set_flow(true);

    float peak = 0.f;
    // Sweep FILT across its whole range while resonating: a self-oscillating
    // SVF is likeliest to diverge while its cutoff is moving, not while it
    // sits still, so a fixed-cutoff test would miss the failure it is for.
    for (int i = 0; i < 48000 * 10; ++i) {
        eng.set_filt(-1.f + 2.f * (static_cast<float>(i) / (48000.f * 10.f)));
        float l = 0.f, r = 0.f;
        eng.process(l, r);
        REQUIRE(std::isfinite(l));
        REQUIRE(std::isfinite(r));
        peak = std::fabs(l) > peak ? std::fabs(l) : peak;
        peak = std::fabs(r) > peak ? std::fabs(r) : peak;
    }
    CHECK(peak < 8.f);
}
```

`test_ratio_for` is a third test seam, same pattern. Add to `engine/sampler/sampler_engine.h`:

```cpp
// Test seam only: forwards to the anonymous-namespace pitch mapping.
float test_ratio_for(float pitch_norm);
```

- [ ] **Step 3: Run and confirm they fail**

```bash
source env.sh && cmake --build build
```

Expected: FAIL to link on `spky::test_ratio_for`.

- [ ] **Step 4: Implement the pitch curve**

Replace `ratio_for` at `engine/sampler/sampler_engine.cpp:12-14`:

```cpp
inline float ratio_for(float pitch_norm) {
    return std::pow(8.f, clampf(pitch_norm, 0.f, 1.f) - 0.5f);
}
```

with:

```cpp
// Pitch: the lane arrives already quantized from Part. The middle half of
// travel is the M5a mapping 8^(p-0.5) unchanged (+-9 semitones); both outer
// quarters steepen to reach +-kPitchOctaves at the ends. Control rate only
// -- _next_ratio reads the precomputed _chord_ratio cache, never this.
inline float ratio_for(float pitch_norm) {
    const float p = clampf(pitch_norm, 0.f, 1.f);
    if (p < kPitchKneeLo) {
        const float at_knee = std::pow(8.f, kPitchKneeLo - 0.5f);
        const float floor_r = std::pow(2.f, -kPitchOctaves);
        return floor_r * std::pow(at_knee / floor_r, p / kPitchKneeLo);
    }
    if (p > kPitchKneeHi) {
        const float at_knee = std::pow(8.f, kPitchKneeHi - 0.5f);
        const float ceil_r  = std::pow(2.f, kPitchOctaves);
        return at_knee * std::pow(ceil_r / at_knee,
                                  (p - kPitchKneeHi) / (1.f - kPitchKneeHi));
    }
    return std::pow(8.f, p - 0.5f);   // the M5a curve, unchanged
}
```

And after the anonymous namespace closes, beside the other seams:

```cpp
float test_ratio_for(float pitch_norm) { return ratio_for(pitch_norm); }
```

- [ ] **Step 5: Raise the resonance ceiling**

Replace `engine/sampler/sampler_engine.cpp:376-377`:

```cpp
void SamplerEngine::set_resonance(float n) {
    _res_n = clampf(n, 0.f, 0.95f);
```

with:

```cpp
void SamplerEngine::set_resonance(float n) {
    // Up to 1.0, where daisysp::Svf self-oscillates. The M5a ceiling of 0.95
    // had no documented reason; "sampler: resonance at maximum stays finite"
    // in tests/test_sampler_engine.cpp is what justifies this one.
    _res_n = clampf(n, 0.f, 1.f);
```

- [ ] **Step 6: Run and see what resonance actually does**

```bash
source env.sh && cmake --build build && ./build/spky_tests --test-case="*pitch*,*resonance*"
```

Expected: the three pitch cases PASS.

**If the resonance case fails**, that is a real answer and not a setback. Bisect for the highest ceiling that passes — try 0.99, 0.98, 0.97 — set the clamp there, and replace the comment with what you measured, naming the value that failed and how (NaN, or a peak above 8). The spec asks for exactly this: the ceiling returns as a measured fact rather than an inherited habit. Report the number in your task report.

- [ ] **Step 7: Run the whole suite**

```bash
source env.sh && cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: **476 passing.**

- [ ] **Step 8: Commit**

```bash
git add engine/sampler/sampler_config.h engine/sampler/sampler_engine.cpp engine/sampler/sampler_engine.h tests/test_sampler_engine.cpp
git commit -m "sampler: pitch to +-4 octaves, resonance to self-oscillation

Pitch keeps the M5a mapping over the middle half of travel and steepens the
outer quarters. Resonance's undocumented 0.95 ceiling is raised and now has
a test that says why it sits where it sits.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 6: Density, measured rather than guessed

`kGrains` is not the density control — `kOverlap` is, and `kGrains` only supplies slots. This task measures three pairs and picks from the numbers.

**Files:**
- Modify: `engine/sampler/sampler_engine.h` (`kOverlap`), `engine/sampler/sampler_config.h` (`kGrains`)
- Test: `tests/test_sampler_engine.cpp` (telemetry case)

**Interfaces:**
- Consumes: Task 3's `kSpawnMinSamples`. Raising `kOverlap` must not raise the spawn rate past that floor — the test Task 3 added is what proves it, and it must stay green at whatever pair you choose.
- Produces: the final `kOverlap` / `kGrains` values.

- [ ] **Step 1: Write the telemetry case**

This case **reports** rather than asserts a taste threshold. It exists so the choice is made from numbers.

```cpp
TEST_CASE("sampler: density telemetry at worst case") {
    // Not a pass/fail threshold -- a measurement. Worst case means every
    // slot contended: maximum MOTION (timing jitter bunches spawns), short
    // SIZE (frequent spawns), FLOW running continuously.
    using namespace spky::sampler_cfg;
    SamplerEngine eng;
    std::vector<SampleBuffer::Frame> mem(48000);
    eng.set_memory(mem.data(), mem.size());
    eng.set_seed(0xD1u);
    eng.init(48000.f);
    fill_buffer_with_tone(eng, 48000, 220.f);

    eng.set_target_base(SamplerEngine::LANE_SIZE,   0.1f);
    eng.set_target_base(SamplerEngine::LANE_MOTION, 1.f);
    eng.set_target_base(SamplerEngine::LANE_LEVEL,  1.f);
    eng.set_flow(true);

    long long total = 0;
    int peak = 0;
    const int kSamples = 48000 * 10;
    for (int i = 0; i < kSamples; ++i) {
        float l = 0.f, r = 0.f;
        eng.process(l, r);
        const int a = eng.active_grains();
        total += a;
        peak = a > peak ? a : peak;
    }
    const double mean = static_cast<double>(total) / kSamples;
    MESSAGE("kOverlap=" << SamplerEngine::kOverlap << " kGrains=" << kGrains
            << " mean=" << mean << " peak=" << peak);

    // The one real assertion: if the mean is at the slot ceiling, spawns are
    // being dropped and kGrains is the binding constraint, not kOverlap.
    CHECK(peak <= kGrains);
}
```

- [ ] **Step 2: Measure the baseline pair (4, 8)**

```bash
source env.sh && cmake --build build
./build/spky_tests --test-case="*density telemetry*" --success
time ./build/spky_render host/render/scenarios/sampler_storm.json /tmp/density_4_8.wav
```

Record the reported mean, peak, and the `real` time. Note the scenario's `duration_s` so the number can be expressed per audio-second.

- [ ] **Step 3: Measure (8, 16)**

Set `kOverlap = 8` in `engine/sampler/sampler_engine.h` and `kGrains = 16` in `engine/sampler/sampler_config.h`, then:

```bash
source env.sh && cmake --build build
./build/spky_tests --test-case="*density telemetry*" --success
time ./build/spky_render host/render/scenarios/sampler_storm.json /tmp/density_8_16.wav
```

- [ ] **Step 4: Measure (16, 32)**

Set `kOverlap = 16` and `kGrains = 32`, then run the same two commands, writing to `/tmp/density_16_32.wav`.

- [ ] **Step 5: Confirm the spawn floor held at every pair**

```bash
source env.sh && cmake --build build && ./build/spky_tests --test-case="*spawn interval never falls*"
```

Expected: PASS at (16, 32) — this is precisely the case Task 3's floor exists for. If it fails, the floor was not relocated correctly; stop and report rather than raising `kSpawnMinSamples`.

- [ ] **Step 6: Choose, and say why**

Set the pair you choose. **Default to (8, 16)** unless the numbers argue otherwise: the spec records that the desktop proxy is blindest at (16, 32), where the Daisy's real limit is SDRAM traffic from interpolated reads scattered across a ~32 MB buffer, which a desktop cache does not feel at all. Do not choose (16, 32) on desktop timings alone.

Write the three measurements into the comment above `kOverlap` in `engine/sampler/sampler_engine.h`, in the style of `kNormSmoothS` in `sampler_config.h` — the numbers, then the choice, then the reason.

- [ ] **Step 7: Run the whole suite**

```bash
source env.sh && cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: **477 passing.**

- [ ] **Step 8: Commit**

```bash
git add engine/sampler/sampler_engine.h engine/sampler/sampler_config.h tests/test_sampler_engine.cpp
git commit -m "sampler: raise cloud density, chosen from measurement

kOverlap sets density and kGrains only supplies slots, so both move
together or the extra spawns are silently dropped. Three pairs measured;
the numbers and the reasoning are in the comment above kOverlap.

The desktop render time is a proxy and is blindest at the top pair, where
the Daisy's limit is SDRAM traffic rather than compute.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 7: Listening renders, and write down what happened

Everything above is gated by numbers. What the ranges are actually *for* is gated by ear, and that is Bastian's call — this task produces the material for it.

**Files:**
- Create: `host/render/scenarios/sampler_extremes.json`
- Modify: `docs/superpowers/specs/2026-07-18-sampler-texture-deck-design.md` (the Open section on fog)
- Modify: `docs/roadmap.md`
- Modify: `.superpowers/sdd/progress.md`

**Interfaces:**
- Consumes: `renders/BEFORE-bootstate-overdub.wav` from Task 4 Step 1.
- Produces: nothing.

- [ ] **Step 1: Write the extremes scenario**

Create `host/render/scenarios/sampler_extremes.json`:

```json
{
  "_comment": "LISTENING RENDER for the opened ranges (spec 2026-07-21). Records 8 s of the storm, then visits each newly reachable extreme in turn with everything else parked, so each one can be judged on its own rather than as a blend. Synth silenced throughout.",
  "sample_rate": 48000,
  "bpm": 96,
  "duration_s": 78.0,
  "input_wav": "host/render/scenarios/assets/96_Stormy_Noise_Loop_01.wav",
  "init": [
    { "action": "set_engine", "part": 1, "value": "sampler" },
    { "action": "set_target_base", "part": 0, "slot": 4, "value": 0.0 },
    { "action": "set_target_active", "part": 0, "slot": 4, "flag": false },
    { "action": "set_target_base", "part": 1, "slot": 4, "value": 1.0 },
    { "action": "set_target_base", "part": 1, "slot": 1, "value": 0.5 },
    { "action": "set_target_active", "part": 1, "slot": 1, "flag": false },
    { "action": "set_target_base", "part": 1, "slot": 3, "value": 0.0 },
    { "action": "set_target_active", "part": 1, "slot": 3, "flag": false },
    { "action": "sampler_feedback", "part": 1, "value": 0.95 }
  ],
  "events": [
    { "t": 1.0,  "action": "sampler_record", "part": 1, "flag": true },
    { "t": 9.0,  "action": "sampler_record", "part": 1, "flag": false },

    { "_comment": "10-25s: MOTION opens to full. THE FOG QUESTION -- scatter now spans the whole buffer instead of a quarter.",
      "t": 10.0, "action": "set_target_base", "part": 1, "slot": 3, "value": 0.0 },
    { "t": 15.0, "action": "set_target_base", "part": 1, "slot": 3, "value": 0.5 },
    { "t": 20.0, "action": "set_target_base", "part": 1, "slot": 3, "value": 1.0 },

    { "_comment": "25-40s: SIZE to the new top. One grain covers the whole loop under one window.",
      "t": 25.0, "action": "set_target_base", "part": 1, "slot": 3, "value": 0.3 },
    { "t": 25.0, "action": "set_target_base", "part": 1, "slot": 1, "value": 0.9 },
    { "t": 33.0, "action": "set_target_base", "part": 1, "slot": 1, "value": 1.0 },

    { "_comment": "40-52s: SIZE to the new bottom. Should stop being a texture and become a pitched buzz.",
      "t": 40.0, "action": "set_target_base", "part": 1, "slot": 1, "value": 0.1 },
    { "t": 46.0, "action": "set_target_base", "part": 1, "slot": 1, "value": 0.0 },

    { "_comment": "52-66s: pitch (lane 2, LANE_PITCH) to both new extremes, SIZE back to the familiar middle.",
      "t": 52.0, "action": "set_target_base", "part": 1, "slot": 1, "value": 0.5 },
    { "t": 52.0, "action": "set_target_base", "part": 1, "slot": 2, "value": 1.0 },
    { "t": 52.0, "action": "set_target_active", "part": 1, "slot": 2, "flag": false },
    { "t": 59.0, "action": "set_target_base", "part": 1, "slot": 2, "value": 0.0 },

    { "_comment": "66-78s: resonance at maximum, cutoff swept.",
      "t": 66.0, "action": "set_target_base", "part": 1, "slot": 2, "value": 0.5 },
    { "t": 66.0, "action": "set_resonance", "part": 1, "value": 1.0 },
    { "t": 66.0, "action": "set_filt", "part": 1, "value": -0.8 },
    { "t": 72.0, "action": "set_filt", "part": 1, "value": 0.8 }
  ]
}
```

**Check the action names against `host/render/` before rendering** — `set_resonance` and `set_filt` may be spelled differently in the scenario reader, and slot 0 may not be the pitch lane. Read `host/render/scenario.cpp` (or whatever the reader is called) and correct the JSON rather than guessing. If an action genuinely does not exist, add it following the pattern of its neighbours and say so in the commit message.

- [ ] **Step 2: Render everything for listening**

```bash
source env.sh && cmake --build build
./build/spky_render host/render/scenarios/sampler_extremes.json renders/FINAL-extremes.wav
./build/spky_render host/render/scenarios/sampler_bloom.json     renders/FINAL-bloom.wav
./build/spky_render host/render/scenarios/sampler_storm.json     renders/FINAL-storm.wav
./build/spky_render host/render/scenarios/sampler_storm_solo.json renders/FINAL-storm_solo.wav
./build/spky_render host/render/scenarios/sampler_solo.json      renders/AFTER-bootstate-overdub.wav
```

`renders/BEFORE-bootstate-overdub.wav` and `renders/AFTER-bootstate-overdub.wav` are the A/B pair acceptance item 9 requires — the boot default is the resting state of every overdub, not an opened extreme, so it must be judged rather than assumed.

- [ ] **Step 3: Prove both gates still hold**

```bash
# Synth neutrality: nothing in this plan touches engine/synth/.
for s in host/render/scenarios/*.json; do
  case "$s" in *sampler*) continue;; esac
  ./build/spky_render "$s" /tmp/neutral_$(basename "$s" .json).wav
done
```

Compare each against the pinned baselines by whatever mechanism `host/render/` already uses for the neutrality gate — read it rather than inventing a comparison. **Byte-identical, no tolerance.** If any synth render moved, stop and report: nothing in this plan should have been able to do that.

```bash
# Determinism
./build/spky_render host/render/scenarios/sampler_extremes.json /tmp/det_a.wav
./build/spky_render host/render/scenarios/sampler_extremes.json /tmp/det_b.wav
cmp /tmp/det_a.wav /tmp/det_b.wav && echo DETERMINISTIC
```

- [ ] **Step 4: Update the M5 spec's open question**

In `docs/superpowers/specs/2026-07-18-sampler-texture-deck-design.md`, the Open section dated 2026-07-20 records that MOTION does not reach fog and lists three untried options. `kScatterPosFrac` was the first of them. Append — do **not** delete the existing text, it records a real lesson:

```markdown
**Update, 2026-07-21 — the scatter ceiling was the untried lever.**
`kScatterPosFrac` was 0.25 through M5a, confining MOTION's read position to
a quarter of the buffer no matter how far the knob went. It is now 1.0
(spec 2026-07-21, section 2). Whether that closes the fog gap is a listening
question and the answer belongs here once it is known — do not record a
verdict from a metric.
```

- [ ] **Step 5: Update the roadmap**

In `docs/roadmap.md`, add a line under M5a recording that the ranges were opened on 2026-07-21 and pointing at `docs/superpowers/specs/2026-07-21-sampler-generous-ranges-design.md`. Match the surrounding format exactly.

- [ ] **Step 6: Append to the ledger**

Add to `.superpowers/sdd/progress.md` a short section for this plan: the tasks completed, the resonance ceiling that was actually measured, the density pair that was chosen and its three numbers, and the state of the branch. End with the handoff line: **the branch is not merged, and Bastian decides after listening.**

- [ ] **Step 7: Commit**

```bash
git add host/render/scenarios/sampler_extremes.json docs/superpowers/specs/2026-07-18-sampler-texture-deck-design.md docs/roadmap.md .superpowers/sdd/progress.md
git commit -m "docs(ranges): listening renders and what is now open to the ear

Renders one pass per newly reachable extreme, plus the before/after pair
for the boot-state overdub -- the default moved from -3 dB to about -1.8 dB
and that is the resting state of every overdub, not an opened extreme.

The M5 spec's fog question is updated to record that the scatter ceiling
was the untried lever, without claiming an answer no one has listened to.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

## After all tasks

Do **not** merge. The branch stays as it is until Bastian has listened to
`renders/FINAL-extremes.wav`, `renders/FINAL-bloom.wav`, and the
`BEFORE`/`AFTER` boot-state pair.

The three numbers most likely to want changing after that listen are all single
constants, and none of them require re-running this plan:

- `kFbMaxDb` — how far past unity the bloom goes, and how wide the unity spot is
- `kSizeCeilS` / `kSizeFloorS` — whether the ends of SIZE are usable or dead
- `kScatterPosFrac` — whether full-buffer scatter reads as fog or as noise

That was the premise the spec was written on: choose generously, listen, and
clamp back with a reason. A ceiling that returns after a listen is a documented
fact. One that was never opened is only a habit.

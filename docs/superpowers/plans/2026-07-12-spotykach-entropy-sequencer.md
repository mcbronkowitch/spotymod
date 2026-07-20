# Spotykach Entropy Sequencer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give every `ModLane` a looping S&H step buffer plus a bipolar ENTROPY parameter (erode / loop / grow) replacing EVOLVE, so STEP+S&H plays loopable, mutable, erodable melodies instead of unusable per-cycle randomness.

**Architecture:** The lane's single per-cycle random (`_sh_cycle`) becomes a seeded, pre-filled step buffer `_seq[32]` read by step index — melodies loop by construction. A new bipolar `set_entropy()` replaces `set_evolve()`: positive values mutate fired steps with a root-gravity random walk (and drive the existing EVOLVE phase/shape/rate walk), negative values erode fired steps toward 0 (and settle the walk), zero freezes everything. Spec: `docs/superpowers/specs/2026-07-12-spotykach-entropy-sequencer-design.md` (this repo).

**Tech Stack:** C++17, doctest, CMake + Ninja + clang (desktop host), nlohmann/json scenarios.

## Global Constraints

- Work happens in the fork at `c:\Users\bernd\Documents\AI\Spotykach` (Tasks 1–4) and this residency repo (Task 5 docs only).
- No heap, no allocation in the audio path; everything static (`float _seq[32]`, no `std::vector` in `engine/`).
- No libDaisy include anywhere under `engine/`.
- Determinism: all randomness through the lane's seeded `spky::Rng`; identical scenario → bit-identical WAV.
- All engine code in `namespace spky`.
- Build/test: `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake -B build && cmake --build build` — env.sh MUST be sourced first (clang + Ninja, no MSVC).
- Run tests: `./build/spky_tests` (doctest; `-tc="<name>"` filters a single TEST_CASE).
- Commit trailer (always, both repos): `Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>`
- Mutation constants (`kGravity`, `kErode`, `kRootSnap`, dice curve) are behavior-carrying but ear-tunable; tests must not over-pin them (assert convergence/variation, not exact values — except via fixed seeds, which are deterministic).

---

### Task 1: ENTROPY API replaces EVOLVE (bipolar), walk gating + settle

The rename plus the one genuinely new behavior at this layer: entropy < 0 decays the EVOLVE random-walk offsets back toward neutral. The S&H buffer comes in Task 2; `_sh_cycle` stays untouched here.

**Files:**
- Modify: `engine/mod/lane.h` (line 23, line 58)
- Modify: `engine/mod/lane.cpp` (line 34, lines 121–127)
- Modify: `engine/mod/super_modulator.h` (line 23), `engine/mod/super_modulator.cpp` (line 50)
- Modify: `engine/instrument.h` (line 34)
- Modify: `host/render/scenario.cpp` (line 88)
- Modify: `tests/test_evolve.cpp` (all `set_evolve` calls + new settle test)
- Modify: `tests/test_capture.cpp` (lines 187, 275)
- Modify: `host/render/scenarios/*.json` (rename `set_evolve` → `set_entropy`, values unchanged)

**Interfaces:**
- Consumes: existing `ModLane`, `SuperModulator`, `Instrument` APIs.
- Produces: `void ModLane::set_entropy(float e)` (clamped −1..+1, field `_entropy`), `void SuperModulator::set_entropy(float a)`, `void Instrument::set_entropy(int p, float n)`, scenario action `"set_entropy"`. `set_evolve` no longer exists anywhere. Tasks 2–4 rely on `_entropy` and these names exactly.

- [ ] **Step 1: Update tests to the new API and add the settle test**

In `tests/test_evolve.cpp`: rename the helper parameter and every call `l.set_evolve(...)` → `l.set_entropy(...)` (lines 7, 14 — keep values; old 0..1 evolve maps to the same positive entropy). Append at the end of the file:

```cpp
// ENTROPY < 0 settles the EVOLVE walk: after wandering hard, sustained negative
// entropy must bring the cycle length back to nominal (~48000 samples @ 1 Hz).
TEST_CASE("lane ENTROPY < 0: the EVOLVE walk settles back toward neutral") {
    ModLane l;
    l.init(48000.f, 2024);
    l.set_range(1.f); l.set_shape(0.25f); l.set_smooth(0.f);
    l.set_probability(1.f); l.set_rate_hz(1.f);
    l.set_entropy(1.f);
    for (int i = 0; i < 48000 * 10; ++i) l.process();   // wander away
    l.set_entropy(-1.f);
    for (int i = 0; i < 48000 * 40; ++i) l.process();   // settle
    int last = -1;
    std::vector<int> gaps;
    for (int i = 0; i < 48000 * 5; ++i) {
        l.process();
        if (l.fired()) { if (last >= 0) gaps.push_back(i - last); last = i; }
    }
    REQUIRE(!gaps.empty());
    for (int g : gaps) CHECK(std::abs(g - 48000) < 200); // back to ~nominal rate
}
```

In `tests/test_capture.cpp` line 187: `b.set_evolve(1.f);` → `b.set_entropy(1.f);` and line 275: `a.set_evolve(1.f);   b.set_evolve(0.f);` → `a.set_entropy(1.f);   b.set_entropy(0.f);`. Update the surrounding EVOLVE comments to say ENTROPY.

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake -B build && cmake --build build 2>&1 | tail -20`
Expected: compile error — `'class spky::ModLane' has no member named 'set_entropy'`.

- [ ] **Step 3: Implement the rename + settle behavior**

`engine/mod/lane.h` line 23: replace

```cpp
    void set_evolve(float amount);    // 0 = LOOP (deterministic)
```

with

```cpp
    void set_entropy(float e);        // -1..+1: erode / loop (0) / grow
```

and line 58: `float _evolve = 0.f;` → `float _entropy = 0.f;`.

`engine/mod/lane.cpp` line 34: replace

```cpp
void ModLane::set_evolve(float a)     { _evolve = clampf(a, 0.f, 1.f); }
```

with

```cpp
void ModLane::set_entropy(float e)    { _entropy = clampf(e, -1.f, 1.f); }
```

`engine/mod/lane.cpp` lines 121–127 (inside `if (wrapped)`): replace

```cpp
            _sh_cycle = _rng.next_bipolar();            // new S&H value per cycle
            if (_evolve > 0.f) {                        // EVOLVE random walk (live only)
                _ev_phase = clampf(_ev_phase + _rng.next_bipolar() * 0.01f * _evolve, -0.5f, 0.5f);
                _ev_shape = clampf(_ev_shape + _rng.next_bipolar() * 0.02f * _evolve, -0.25f, 0.25f);
                _ev_rate  = clampf(_ev_rate  + _rng.next_bipolar() * 0.01f * _evolve, -0.2f, 0.2f);
            }
```

with

```cpp
            _sh_cycle = _rng.next_bipolar();            // new S&H value per cycle
            if (_entropy > 0.f) {                       // GROW: EVOLVE random walk (live only)
                _ev_phase = clampf(_ev_phase + _rng.next_bipolar() * 0.01f * _entropy, -0.5f, 0.5f);
                _ev_shape = clampf(_ev_shape + _rng.next_bipolar() * 0.02f * _entropy, -0.25f, 0.25f);
                _ev_rate  = clampf(_ev_rate  + _rng.next_bipolar() * 0.01f * _entropy, -0.2f, 0.2f);
            } else if (_entropy < 0.f) {                // ERODE: walk settles toward neutral
                float decay = 1.f + 0.2f * _entropy;    // entropy -1 -> x0.8 per cycle
                _ev_phase *= decay;
                _ev_shape *= decay;
                _ev_rate  *= decay;
            }                                           // entropy 0 (LOOP): walk frozen
```

`engine/mod/super_modulator.h` line 23: `void set_evolve(float a);` → `void set_entropy(float a);`
`engine/mod/super_modulator.cpp` line 50:

```cpp
void SuperModulator::set_entropy(float a)     { for (auto& l : _lanes) l.set_entropy(a); }
```

`engine/instrument.h` line 34 (note: this setter is the one documented exception to "normalized 0..1" — it takes −1..+1):

```cpp
    void set_entropy(int p, float n)         { _parts[p].mod().set_entropy(n); }  // -1..+1
```

`host/render/scenario.cpp` line 88:

```cpp
    else if (a == "set_entropy")       inst.set_entropy(e.part, e.value);
```

Rename the action in every bundled scenario (values unchanged — old positive evolve = positive entropy/grow):

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && sed -i 's/"set_evolve"/"set_entropy"/g' host/render/scenarios/*.json`

- [ ] **Step 4: Run tests to verify they pass**

Run: `source env.sh && cmake --build build && ./build/spky_tests`
Expected: all tests PASS (including the new settle test). Also verify no stragglers: `grep -rn "set_evolve\|_evolve" engine/ host/ tests/` → no matches (README/roadmap mentions are handled in Task 5).

- [ ] **Step 5: Commit**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
git add -A engine host tests
git commit -m "feat(mod): bipolar ENTROPY replaces EVOLVE — negative values settle the walk

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 2: Looping step buffer replaces the per-cycle S&H value

The melody source: `_seq[32]` pre-filled at init, read by step index (STEP) or slot 0 (FLOW), never redrawn on wrap. Includes the `shape_value()` fix so SHAPE = 1 returns the S&H operand *exactly* (today it lerps with weight 0.9999, leaving 0.01% pulse bleed — that would break exact loop equality and the "erodes to exactly 0" contract).

**Files:**
- Modify: `engine/mod/lane.h` (replace `_sh_cycle`, add `_seq`/`kSeqSlots`/`_sh_slot()`)
- Modify: `engine/mod/lane.cpp` (init fill, `_compute_raw()`, remove wrap redraw)
- Modify: `engine/mod/waveforms.h` (lines 18–21: exact S&H at shape 1)
- Create: `tests/test_entropy_seq.cpp`
- Modify: `CMakeLists.txt` (add the new test file after `tests/test_evolve.cpp`)

**Interfaces:**
- Consumes: `_entropy` and `set_entropy()` from Task 1.
- Produces: `static constexpr int ModLane::kSeqSlots = 32`, `float _seq[kSeqSlots]`, `int _sh_slot() const` (STEP: `_cur_step % kSeqSlots`, FLOW: 0). Task 3 mutates `_seq[_sh_slot()]`. Test helpers `fired_targets()` / `make_sh_step_lane()` in `tests/test_entropy_seq.cpp` are reused by Task 3's tests.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_entropy_seq.cpp`:

```cpp
#include <doctest/doctest.h>
#include <algorithm>
#include <vector>
#include "mod/lane.h"
using namespace spky;

// Latched per-step values: run the lane, record target() on every fired step.
static std::vector<float> fired_targets(ModLane& l, int n_samples) {
    std::vector<float> v;
    for (int i = 0; i < n_samples; ++i) {
        l.process();
        if (l.fired()) v.push_back(l.target());
    }
    return v;
}

static ModLane make_sh_step_lane(uint32_t seed, float entropy, float prob, int steps) {
    ModLane l;
    l.init(48000.f, seed);
    l.set_range(1.f); l.set_smooth(0.f);
    l.set_shape(1.f);            // pure S&H end of SHAPE
    l.set_step(true, steps);
    l.set_probability(prob);
    l.set_entropy(entropy);
    l.set_rate_hz(1.f);          // ~48000 samples per cycle
    return l;
}

TEST_CASE("ENTROPY 0: STEP + S&H loops its melody exactly, cycle after cycle") {
    auto l = make_sh_step_lane(42, 0.f, 1.f, 8);
    auto v = fired_targets(l, 48000 * 4 + 24000);        // > 4 cycles
    REQUIRE(v.size() >= 32);
    for (size_t i = 0; i + 8 < 32; ++i) CHECK(v[i] == v[i + 8]);   // exact equality
}

TEST_CASE("melody from the first cycle: buffer pre-filled, deterministic per seed") {
    auto a = make_sh_step_lane(7, 0.f, 1.f, 8);
    auto b = make_sh_step_lane(7, 0.f, 1.f, 8);
    auto va = fired_targets(a, 47000);                   // < one cycle: exactly 8 fires
    auto vb = fired_targets(b, 47000);
    REQUIRE(va.size() == 8);
    bool all_equal = true;
    for (float x : va) if (x != va[0]) all_equal = false;
    CHECK(!all_equal);            // a melody, not one repeated note
    CHECK(va == vb);              // identical seeds -> identical melody
}

TEST_CASE("ENTROPY 0: FLOW + S&H holds one loop-stable value across cycles") {
    ModLane l;
    l.init(48000.f, 42);
    l.set_range(1.f); l.set_smooth(0.f);
    l.set_shape(1.f);
    l.set_step(false, 8);
    l.set_probability(1.f);
    l.set_entropy(0.f);
    l.set_rate_hz(4.f);           // several cycles within one second
    l.process();
    float first = l.target();
    bool constant = true;
    for (int i = 0; i < 48000; ++i) {
        l.process();
        if (l.target() != first) constant = false;
    }
    CHECK(constant);              // no per-cycle redraw anymore
}
```

Add to `CMakeLists.txt` directly after the line `tests/test_evolve.cpp`:

```cmake
    tests/test_entropy_seq.cpp
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `source env.sh && cmake -B build && cmake --build build && ./build/spky_tests -tc="*S&H*,*melody*"`
Expected: the loop test FAILS (today each cycle redraws `_sh_cycle` → `v[i] != v[i+8]`), the melody-from-first-cycle test FAILS (`all_equal` is true — one value per cycle), the FLOW test FAILS (redraw per wrap).

- [ ] **Step 3: Implement the buffer**

`engine/mod/waveforms.h`: replace lines 18–21

```cpp
    float seg = shape * 4.f;
    if (seg >= 3.9999f) seg = 3.9999f;   // keep S&H reachable at shape == 1
    int   i = static_cast<int>(seg);
    float f = seg - i;
```

with

```cpp
    float seg = shape * 4.f;
    int   i = static_cast<int>(seg);
    if (i > 3) i = 3;                    // shape == 1 -> f == 1: pure S&H, no pulse bleed
    float f = seg - i;
```

`engine/mod/lane.h`: replace line 65 (`float _sh_cycle = 0.f;   // per-cycle random for the S&H end of the bank`) with

```cpp
    static constexpr int kSeqSlots = 32;
    float _seq[kSeqSlots] = {};  // looping S&H step buffer — the melody (spec: entropy sequencer)
```

and add to the private method declarations (next to `_compute_raw`):

```cpp
    int   _sh_slot() const;         // which _seq slot the S&H end reads now
```

`engine/mod/lane.cpp`:

In `init()`, replace `_sh_cycle = _rng.next_bipolar();` with:

```cpp
    for (float& v : _seq) v = _rng.next_bipolar();   // a melody exists from cycle one
```

Add after `_compute_raw()`:

```cpp
int ModLane::_sh_slot() const {
    if (!_step_mode) return 0;                 // FLOW: one slot, loop-stable per cycle
    int s = _cur_step < 0 ? 0 : _cur_step;
    return s % kSeqSlots;
}
```

In `_compute_raw()`, replace `return shape_value(ph, sh, _sh_cycle);` with:

```cpp
    return shape_value(ph, sh, _seq[_sh_slot()]);
```

In `process()`, delete the line `_sh_cycle = _rng.next_bipolar();            // new S&H value per cycle` (the wrap block keeps only the entropy walk/settle logic from Task 1).

- [ ] **Step 4: Run the full suite**

Run: `cmake --build build && ./build/spky_tests`
Expected: all PASS — the three new tests plus every existing test (`test_waveforms.cpp`'s S&H checks use `Approx`, so the exact-S&H fix keeps them green; removing the per-wrap draw shifts RNG streams, which no existing test pins).

- [ ] **Step 5: Commit**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
git add engine/mod/lane.h engine/mod/lane.cpp engine/mod/waveforms.h tests/test_entropy_seq.cpp CMakeLists.txt
git commit -m "feat(mod): looping S&H step buffer — melodies loop by construction; exact S&H at shape 1

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 3: Mutation — GROW walk with root gravity, ERODE toward the root

The entropy process itself: on each **fired** step, dice with probability `entropy²`; on success, positive entropy walks the slot from its old value (small intervals often, leaps rare, mild pull to 0), negative entropy shrinks it toward 0 with a snap threshold so it lands exactly.

**Files:**
- Modify: `engine/mod/lane.h` (declare `_mutate_slot`)
- Modify: `engine/mod/lane.cpp` (constants, `_on_boundary()`, `_mutate_slot()`)
- Modify: `tests/test_entropy_seq.cpp` (four new tests)

**Interfaces:**
- Consumes: `_seq` / `_sh_slot()` from Task 2, `_entropy` from Task 1, test helpers `make_sh_step_lane()` / `fired_targets()` from Task 2.
- Produces: final lane behavior; no new public API.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_entropy_seq.cpp`:

```cpp
TEST_CASE("GROW: melody varies over cycles but keeps most of its identity") {
    auto l = make_sh_step_lane(42, 0.4f, 1.f, 8);
    auto v = fired_targets(l, 48000 * 12 + 24000);
    REQUIRE(v.size() >= 96);                     // 12 full cycles of 8
    bool ever_changed = false;
    int  persist_min = 8;
    for (int c = 0; c + 1 < 12; ++c) {
        int same = 0;
        for (int s = 0; s < 8; ++s) {
            if (v[c * 8 + s] == v[(c + 1) * 8 + s]) ++same;
            else ever_changed = true;
        }
        if (same < persist_min) persist_min = same;
    }
    CHECK(ever_changed);                         // it mutates...
    CHECK(persist_min >= 4);                     // ...but never wholesale (walk, not redraw)
}

TEST_CASE("GROW at +1: nearly every fired step mutates") {
    auto l = make_sh_step_lane(42, 1.f, 1.f, 8);
    auto v = fired_targets(l, 48000 * 6 + 24000);
    REQUIRE(v.size() >= 48);
    int changed = 0;
    for (int c = 0; c + 1 < 6; ++c)
        for (int s = 0; s < 8; ++s)
            if (v[c * 8 + s] != v[(c + 1) * 8 + s]) ++changed;
    CHECK(changed >= 35);                        // of 40 slot transitions
}

TEST_CASE("ERODE: sustained -1 converges every step to the root (0)") {
    auto l = make_sh_step_lane(42, -1.f, 1.f, 8);
    (void)fired_targets(l, 48000 * 20);          // 20 cycles of erosion
    auto v = fired_targets(l, 47000);            // one more cycle
    REQUIRE(!v.empty());
    for (float x : v) CHECK(x == 0.f);           // a single repeated note, exactly
}

TEST_CASE("suppressed steps protect the buffer: no fire, no mutation") {
    auto a  = make_sh_step_lane(42, 0.f, 1.f, 8);
    auto va = fired_targets(a, 47000);           // reference: the seed-42 init melody
    REQUIRE(va.size() == 8);

    auto b = make_sh_step_lane(42, 1.f, 0.f, 8); // full entropy, but nothing ever fires
    (void)fired_targets(b, 48000 * 10);          // 10 cycles of suppressed steps
    b.set_entropy(0.f);
    b.set_probability(1.f);
    auto vb = fired_targets(b, 48000);           // the next full cycle (any rotation)
    REQUIRE(vb.size() >= 8);
    std::vector<float> sa(va.begin(), va.begin() + 8), sb(vb.begin(), vb.begin() + 8);
    std::sort(sa.begin(), sa.end());
    std::sort(sb.begin(), sb.end());
    CHECK(sa == sb);                             // buffer untouched by 10 muted cycles
}
```

Note on seeds: all four tests are deterministic. If a threshold assertion (`persist_min >= 4`, `changed >= 35`) fails on seed 42, inspect the values — if the behavior is right but the dice were unlucky for this seed, changing the seed constant is legitimate; loosening the threshold is not.

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build && ./build/spky_tests -tc="GROW*,ERODE*,suppressed*"`
Expected: GROW tests FAIL (`ever_changed` false — nothing mutates yet), ERODE FAILS (values never reach 0), the protection test PASSES vacuously (no mutation exists at all) — that's fine, it becomes load-bearing the moment mutation lands.

- [ ] **Step 3: Implement mutation**

`engine/mod/lane.h`, next to `_sh_slot()`:

```cpp
    void  _mutate_slot(int slot);   // entropy dice + walk/erode on a fired step
```

`engine/mod/lane.cpp`, below the `using namespace spky;` line:

```cpp
// Mutation character — tuned by ear; the spec fixes behavior, not constants.
static constexpr float kGravity  = 0.10f;  // GROW: mild pull toward 0 (the root)
static constexpr float kErode    = 0.60f;  // ERODE: fraction kept per mutation
static constexpr float kRootSnap = 0.02f;  // ERODE: below this, land exactly on 0
```

Replace `_on_boundary()`:

```cpp
void ModLane::_on_boundary() {
    bool fire = _rng.next_unipolar() < _prob;
    _frozen = !fire;
    if (fire) {
        _fired = true;
        if (_entropy != 0.f) _mutate_slot(_sh_slot());  // fired steps only: held
        _target = _compute_raw();   // latch the value at this boundary
    }
    // if !fire: hold the previous _target (frozen) — and the buffer slot with it
}
```

Add `_mutate_slot()`:

```cpp
void ModLane::_mutate_slot(int slot) {
    // Dice: mutation chance grows with |entropy|; squared for fine control near LOOP.
    if (_rng.next_unipolar() >= _entropy * _entropy) return;
    float v = _seq[slot];
    if (_entropy > 0.f) {
        // GROW: random walk from the old value. The cubed draw makes small
        // intervals common and leaps rare; width opens with entropy; the
        // (1 - kGravity) factor is the tonic gravity keeping lines anchored.
        float r = _rng.next_bipolar();
        float delta = r * r * r * lerpf(0.5f, 2.f, _entropy);
        v = clampf((v + delta) * (1.f - kGravity), -1.f, 1.f);
    } else {
        // ERODE: pull the note toward 0 (root / base value); snap when close
        // so sustained erosion lands exactly on a single repeated root note.
        v *= kErode;
        if (std::fabs(v) < kRootSnap) v = 0.f;
    }
    _seq[slot] = v;
}
```

(`clampf`/`lerpf` come from `util/math.h`, already included via lane.cpp's headers; `std::fabs` via the existing `<cmath>` include.)

- [ ] **Step 4: Run the full suite**

Run: `cmake --build build && ./build/spky_tests`
Expected: all PASS, including all seven `test_entropy_seq.cpp` tests and every pre-existing test (mutation draws consume RNG only on fired steps with entropy ≠ 0, so entropy-0 paths are stream-identical to Task 2).

- [ ] **Step 5: Commit**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
git add engine/mod/lane.h engine/mod/lane.cpp tests/test_entropy_seq.cpp
git commit -m "feat(mod): entropy mutation — GROW root-gravity walk, ERODE to the root, fired steps only

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 4: Demo scenarios + end-to-end verification

Rework the three melody demos to showcase the entropy arc, render them, and confirm the bit-determinism invariant on a scenario that exercises both entropy signs.

**Files:**
- Modify: `host/render/scenarios/demo_step_melody.json` (full rewrite below)
- Modify: `host/render/scenarios/capture_pentatonic.json`
- Modify: `host/render/scenarios/capture_duet.json`

**Interfaces:**
- Consumes: scenario action `"set_entropy"` (Task 1), full lane behavior (Tasks 2–3).
- Produces: rendered WAVs under `build/renders/` for listening; no code.

- [ ] **Step 1: Rewrite `demo_step_melody.json` as the entropy showcase**

Replace the whole file with:

```json
{
  "sample_rate": 48000,
  "bpm": 110,
  "duration_s": 32,
  "init": [
    {"_comment":"ENTROPY showcase: a looping S&H melody (0-10s), grown by positive entropy (10-20s), eroded toward the root by negative entropy (20-28s), then frozen (28-32s)."},
    {"action":"set_engine","part":0,"value":"test_tone"},
    {"action":"set_engine","part":1,"value":"test_tone"},
    {"action":"set_sync_mode","part":0,"value":"sync"},
    {"action":"set_rate","part":0,"value":0.5},
    {"action":"set_step","part":0,"flag":true,"ivalue":16},
    {"action":"set_shape","part":0,"value":0.95},
    {"action":"set_range","part":0,"value":0.8},
    {"action":"set_smooth","part":0,"value":0.1},
    {"action":"set_probability","part":0,"value":0.8},
    {"action":"set_entropy","part":0,"value":0.0},
    {"action":"set_depth","part":0,"value":1.0},
    {"action":"set_target_active","part":0,"slot":2,"flag":true},
    {"action":"set_target_active","part":0,"slot":4,"flag":true},
    {"action":"set_target_base","part":0,"slot":2,"value":0.4},
    {"action":"set_target_base","part":0,"slot":4,"value":0.8}
  ],
  "events": [
    {"_comment":"10s: GROW - the loop starts mutating in walk-sized variations."},
    {"t":10.0,"action":"set_entropy","part":0,"value":0.5},
    {"_comment":"20s: ERODE - the melody simplifies toward the root, note by note."},
    {"t":20.0,"action":"set_entropy","part":0,"value":-0.8},
    {"_comment":"28s: LOOP - freeze whatever remains."},
    {"t":28.0,"action":"set_entropy","part":0,"value":0.0}
  ]
}
```

- [ ] **Step 2: Make the capture demos melodic (S&H zone + living variation before capture)**

`capture_pentatonic.json`, in `init`:
- `{"action":"set_shape","part":0,"value":0.62}` → `{"action":"set_shape","part":0,"value":0.9}`
- Directly under part 0's `set_probability` line, add: `{"action":"set_entropy","part":0,"value":0.3},`
- (Part 1's `set_entropy` 0.35 was already renamed in Task 1 — leave it.)

`capture_duet.json`, in `init`:
- `{"action":"set_shape","part":0,"value":0.7}` → `{"action":"set_shape","part":0,"value":0.9}`
- `{"action":"set_shape","part":1,"value":0.55}` → `{"action":"set_shape","part":1,"value":0.85}`
- Under part 0's `set_probability` line add `{"action":"set_entropy","part":0,"value":0.25},` and under part 1's `set_probability` line add `{"action":"set_entropy","part":1,"value":0.25},`

- [ ] **Step 3: Render all melody demos**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build
mkdir -p build/renders
for s in demo_step_melody capture_pentatonic capture_duet melody_then_drift dorian_melody pentatonic_melody; do
  ./build/render host/render/scenarios/$s.json build/renders/$s.wav build/renders/$s.csv
done
```

Expected: six WAVs written, no scenario errors.

- [ ] **Step 4: Verify the bit-determinism invariant on an entropy-exercising scenario**

```bash
./build/render host/render/scenarios/demo_step_melody.json build/renders/det_a.wav build/renders/det_a.csv
./build/render host/render/scenarios/demo_step_melody.json build/renders/det_b.wav build/renders/det_b.csv
cmp build/renders/det_a.wav build/renders/det_b.wav && echo BIT-IDENTICAL
```

Expected: `BIT-IDENTICAL`. Also run the full suite once more: `./build/spky_tests` → all PASS.

- [ ] **Step 5: Listen check + commit**

Tell the user where the WAVs are (`build/renders/`) — the ear verdict on `demo_step_melody.wav` (loop → grow → erode arc audible?) and the two capture demos (melody, not note salad?) is theirs; constants `kGravity`/`kErode`/dice curve are the tuning levers if it sounds wrong. Then:

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
git add host/render/scenarios
git commit -m "feat(render): entropy showcase demo + melodic capture scenarios

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 5: Documentation — fork docs + master-spec touch-ups

**Files:**
- Modify (fork): `README.md` (lines 41, 101, 104), `docs/roadmap.md` (lines 48, 61, 78, 145)
- Modify (residency repo): `docs/superpowers/specs/2026-07-10-spotykach-modulation-first-synth-design.md`

**Interfaces:** none — prose only.

- [ ] **Step 1: Update the fork's EVOLVE mentions**

`README.md`:
- Line 41: `slowly mutate over time (**EVOLVE**).` → `grow, loop, or erode over time (**ENTROPY**).`
- Line 101: `for plotting. Ideal for *seeing* what FLOW / STEP / EVOLVE actually do.` → `for plotting. Ideal for *seeing* what FLOW / STEP / ENTROPY actually do.`
- Line 104: `identically, then slowly mutates as EVOLVE is dialled in.` → `identically, then grows or erodes as ENTROPY is dialled off center.`

`docs/roadmap.md` (same spirit — exact current lines from grep):
- Line 48: `**FLOW** (smooth LFO), **STEP** (clock-quantized sample & hold), **EVOLVE**` → `**FLOW** (smooth LFO), **STEP** (clock-quantized sample & hold), **ENTROPY**`
- Line 61: `- **Tests** (`tests/`, doctest) — lane STEP quantization, LOOP-vs-EVOLVE` → `- **Tests** (`tests/`, doctest) — lane STEP quantization, ENTROPY loop/grow/erode`
- Line 78: `glides step through scale notes and EVOLVE walks the scale.` → `glides step through scale notes and ENTROPY grows or erodes the melody.`
- Line 145: `thins live, SMOOTH glides, TUNE transposes, EVOLVE affects only live lanes.` → `thins live, SMOOTH glides, TUNE transposes, ENTROPY affects only live lanes.`

Verify: `grep -rn "EVOLVE\|evolve" README.md docs/ --include="*.md"` → only historical/changelog-style mentions remain, no behavioral claims.

Commit (fork):

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
git add README.md docs/roadmap.md
git commit -m "docs: EVOLVE -> ENTROPY wording (erode / loop / grow)

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

- [ ] **Step 2: Master-spec touch-ups (residency repo)**

In `docs/superpowers/specs/2026-07-10-spotykach-modulation-first-synth-design.md`:

Replace the macro-parameter bullet (lines 112–114):

```markdown
- **LOOP / EVOLVE** (toggle) — LOOP: deterministic, every cycle identical.
  EVOLVE: per cycle, shape/phase/rate wander slightly via random walk.
```

with:

```markdown
- **ENTROPY** (bipolar, via panel switch 2) — 0 (LOOP): deterministic, every
  cycle identical. Positive (GROW): fired steps of the S&H melody buffer
  mutate via a root-gravity random walk, and shape/phase/rate wander via the
  EVOLVE random walk. Negative (ERODE): fired steps pull toward the root and
  the walk settles back to neutral — the melody simplifies down to one note.
  (See `2026-07-12-spotykach-entropy-sequencer-design.md`.)
```

Replace the STEP/FLOW bullet's trailing clause (line 118–119) `waves become sequences.` with `waves become sequences; at the S&H end of SHAPE the sequence is a looping melody buffer mutated by ENTROPY — not a free random.`

Replace the panel-switch table row (line 316):

```markdown
| 2      | LOOP | EVOLVE subtle     | EVOLVE strong |
```

with

```markdown
| 2      | ERODE | LOOP             | GROW          |
```

Replace line 246 `- **EVOLVE** affects only the live lanes; the captured loop itself never` with `- **ENTROPY** affects only the live lanes; the captured loop itself never` and line 298 `EVOLVE random walks re-centered` with `entropy walks re-centered` (TAP settle). Also update the Decisions bullet on line 38 (`fixed target slots across engines...` block: the line `- Modulator bank (2026-07-10, second pass)...` stays; add below the decisions list):

```markdown
- Entropy sequencer (2026-07-12): looping S&H melody buffer per lane; bipolar
  ENTROPY (erode / loop / grow) replaces LOOP/EVOLVE; switch 2 remapped.
```

Commit (residency repo):

```bash
cd "/c/Users/bernd/Documents/AI/Synthux Design Residency"
git add docs/superpowers/specs/2026-07-10-spotykach-modulation-first-synth-design.md
git commit -m "docs: master-spec touch-ups for the entropy sequencer (ENTROPY replaces LOOP/EVOLVE, switch 2 remap)

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

- [ ] **Step 3: Final verification**

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && ./build/spky_tests && git status --short`
Expected: all tests PASS; both working trees clean.

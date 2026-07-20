# Spotykach Capture Sequencer (M3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Freeze the PITCH lane's last-heard cycle into a loop that replays through the identical downstream chain, swapping only the lane's source — "my sequence vs. the dice."

**Architecture:** A dumb fixed-size buffer class `CaptureLoop` (header-only, two 192-slot arrays) records the PITCH lane's pre-smooth `_target` + trigger pattern into a rolling ring; `capture_now()` freezes ring→loop. `ModLane` gains a narrow replay hook: when replaying, boundaries come from the recorded fired slots and `_target` comes from the loop; the live PROBABILITY dice, SMOOTH, and RANGE keep acting. `SuperModulator` owns exactly one `CaptureLoop` and wires it to `LANE_PITCH` only; `Instrument` delegates per-part. `Part`'s base/depth/TUNE/quantizer are untouched running code that re-voices the loop live. This is approach B from the design brainstorm; `Part` needs no change.

**Tech Stack:** C++17, `namespace spky`, doctest (vendored `third_party/doctest/doctest.h`), CMake + Ninja + clang, nlohmann/json for scenarios. Engine is portable (no libDaisy, no heap, no allocation in the audio path).

Spec: `docs/superpowers/specs/2026-07-12-spotykach-capture-sequencer-design.md`.
Fork working copy: `c:\Users\bernd\Documents\AI\Spotykach` (this is the repo root for all paths below).

## Global Constraints

- **Namespace:** all engine code in `namespace spky`. Copied verbatim from existing files.
- **No heap / no allocation in the audio path:** `CaptureLoop` is two fixed C arrays of 192 `Slot` structs, static storage. No `new`, no `std::vector`, no libDaisy include in `engine/`.
- **Bit-determinism invariant:** identical scenario → bit-identical WAV. Recording must NOT consume the RNG (`record()` is a pure write); replay dice use the same `_rng.next_unipolar()` call already used by `_on_boundary()`.
- **Capture stores the pre-smooth lane value** `_target` (bipolar `[-1,1]`), NOT the quantized pitch — the downstream `Part` chain (base/depth/TUNE/quantizer) re-voices the loop live, so re-scaling re-voices the captured melody.
- **Volatile:** no persistence/preset wiring. Power cycle → generative, no loop.
- **`kSlots = 192`** (divides step grids 8/12/16/24/32 evenly).
- **Build/test commands** (from repo root, once per shell): `source env.sh` then the CMake commands below. `env.sh` sets `CC=clang CXX=clang++ CMAKE_GENERATOR=Ninja`.
- **Co-author trailer** on every commit: `Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>`.

Standard build + test cycle used throughout:

```bash
source env.sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Run one test case only (faster iteration): `./build/spky_tests.exe --test-case="*capture*"`.

---

### Task 1: `CaptureLoop` buffer + isolation tests

The dumb fixed buffer, testable with no lane. Header-only (mirrors `rng.h`/`range.h`), so no `CMakeLists.txt` source entry is needed for the class itself — only for the new test file.

**Files:**
- Create: `engine/mod/capture.h`
- Create: `tests/test_capture.cpp`
- Modify: `CMakeLists.txt` (add `tests/test_capture.cpp` to the `spky_tests` sources)

**Interfaces:**
- Consumes: nothing.
- Produces: `class spky::CaptureLoop` with:
  - `static constexpr int kSlots = 192;`
  - `void reset();` — init state: value 0 everywhere, fired flag on slot 0 only, `valid()==false`.
  - `void record(int slot, float value, bool fired);` — pure write of one ring slot (both fields overwritten). Does not touch RNG.
  - `void capture_now();` — copy ring→loop, set valid.
  - `float value(int slot) const;` / `bool fired(int slot) const;` — read the frozen loop.
  - `bool valid() const;`

- [ ] **Step 1: Write the failing tests**

Create `tests/test_capture.cpp` with the full include block (used by all later tasks too) and the Task 1 cases:

```cpp
#include <doctest/doctest.h>
#include <cmath>
#include "mod/capture.h"
#include "mod/lane.h"
#include "mod/super_modulator.h"
#include "mod/lane_id.h"
#include "instrument.h"
#include "render/scenario.h"
using namespace spky;

TEST_CASE("CaptureLoop: init state is centered, invalid, fired only on slot 0") {
    CaptureLoop loop;
    loop.reset();
    CHECK(loop.valid() == false);
    // capture the (unrecorded) init ring so we can read it back through the loop
    loop.capture_now();
    CHECK(loop.valid() == true);
    CHECK(loop.value(0) == doctest::Approx(0.f));
    CHECK(loop.value(100) == doctest::Approx(0.f));
    CHECK(loop.fired(0) == true);
    CHECK(loop.fired(1) == false);
    CHECK(loop.fired(191) == false);
}

TEST_CASE("CaptureLoop: record then capture_now copies ring -> loop") {
    CaptureLoop loop;
    loop.reset();
    loop.record(5, 0.25f, true);
    loop.record(6, -0.5f, false);
    // before capture, the frozen loop still holds the init state
    CHECK(loop.value(5) == doctest::Approx(0.f));
    loop.capture_now();
    CHECK(loop.value(5) == doctest::Approx(0.25f));
    CHECK(loop.fired(5) == true);
    CHECK(loop.value(6) == doctest::Approx(-0.5f));
    CHECK(loop.fired(6) == false);
}

TEST_CASE("CaptureLoop: freezing captures the current window, incl. stale slots ahead") {
    CaptureLoop loop;
    loop.reset();
    // pass 1: fill every slot with a ramp value + fire on slot 0
    for (int s = 0; s < CaptureLoop::kSlots; ++s)
        loop.record(s, static_cast<float>(s) / CaptureLoop::kSlots, s == 0);
    // pass 2: overwrite only the first half (playhead at slot 96), new value +1 offset
    for (int s = 0; s < 96; ++s)
        loop.record(s, 1.f + static_cast<float>(s) / CaptureLoop::kSlots, s == 0);
    loop.capture_now();   // freeze mid-cycle: [0,96) = pass 2, [96,192) = pass 1
    CHECK(loop.value(10)  == doctest::Approx(1.f + 10.f / CaptureLoop::kSlots)); // fresh
    CHECK(loop.value(150) == doctest::Approx(150.f / CaptureLoop::kSlots));      // stale pass-1
}

TEST_CASE("CaptureLoop: kSlots is 192") {
    CHECK(CaptureLoop::kSlots == 192);
}
```

Add `tests/test_capture.cpp` to the `spky_tests` target. In `CMakeLists.txt`, change:

```cmake
    engine/instrument.cpp
    tests/test_instrument.cpp
    tests/test_wav.cpp
```

to:

```cmake
    engine/instrument.cpp
    tests/test_instrument.cpp
    tests/test_capture.cpp
    tests/test_wav.cpp
```

- [ ] **Step 2: Run tests to verify they fail**

Run:

```bash
source env.sh
cmake -S . -B build
cmake --build build
```

Expected: FAIL to compile — `fatal error: 'mod/capture.h' file not found`.

- [ ] **Step 3: Write `engine/mod/capture.h`**

```cpp
#pragma once

namespace spky {

// Two phase-indexed slot buffers for the PITCH capture loop. A dumb fixed
// buffer with no phase or timing of its own — ModLane drives slot indexing,
// calls record() during generative playback, and capture_now() to freeze the
// rolling ring into the frozen loop. Stores the lane's pre-smooth _target
// (bipolar [-1,1]) so the full downstream chain (SMOOTH, RANGE in the lane;
// base/depth/TUNE/quantizer in Part) re-voices the loop live. ~2 KB, static.
class CaptureLoop {
public:
    static constexpr int kSlots = 192;   // divides 8/12/16/24/32 evenly

    // Init state: value 0 (bipolar center) everywhere, a single fired flag on
    // slot 0. Capturing before one full cycle has ever elapsed then yields a
    // held root-ish note, harmless.
    void reset() {
        for (int i = 0; i < kSlots; ++i) {
            _ring[i] = Slot{ 0.f, i == 0 };
            _loop[i] = Slot{ 0.f, i == 0 };
        }
        _valid = false;
    }

    // Rolling record (generative): overwrite one ring slot. Pure write — never
    // touches the RNG, so attaching a loop cannot change the bitstream.
    void record(int slot, float value, bool fired) {
        _ring[slot].value = value;
        _ring[slot].fired = fired;
    }

    // Freeze: copy the rolling ring -> the frozen loop, mark valid.
    void capture_now() {
        for (int i = 0; i < kSlots; ++i) _loop[i] = _ring[i];
        _valid = true;
    }

    float value(int slot) const { return _loop[slot].value; }
    bool  fired(int slot) const { return _loop[slot].fired; }
    bool  valid()         const { return _valid; }

private:
    struct Slot { float value = 0.f; bool fired = false; };
    Slot _ring[kSlots];
    Slot _loop[kSlots];
    bool _valid = false;
};

} // namespace spky
```

- [ ] **Step 4: Run tests to verify they pass**

Run:

```bash
cmake --build build
./build/spky_tests.exe --test-case="CaptureLoop*"
```

Expected: PASS (4 test cases). Then run the full suite to confirm nothing else broke:

```bash
ctest --test-dir build --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 5: Commit**

```bash
git add engine/mod/capture.h tests/test_capture.cpp CMakeLists.txt
git commit -m "feat(m3): CaptureLoop fixed-buffer for PITCH capture loop

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 2: `ModLane` rolling record hook

The PITCH lane records its `_target` + fired events into the ring every generative sample. No replay yet — this task only fills the ring and lets `capture_now()` freeze a real recording.

**Files:**
- Modify: `engine/mod/lane.h` (forward-declare `CaptureLoop`; add `set_capture_loop`, private `_phase_slot`/`_record_slot`, members)
- Modify: `engine/mod/lane.cpp` (include `capture.h`; init resets; helpers; one insertion in `process()`)
- Modify: `tests/test_capture.cpp` (append cases)

**Interfaces:**
- Consumes: `CaptureLoop` (Task 1).
- Produces: `void ModLane::set_capture_loop(CaptureLoop* loop);` — wires the ring target once at init; `nullptr` (the default, and the four non-PITCH lanes) = no recording, behaves exactly as today. Recording writes `_target` and the accumulated fired flag into `CaptureLoop` slot `floor(phase * 192)`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_capture.cpp`:

```cpp
// Configure a STEP PITCH-style lane with a capture loop attached.
static void configure_step_capture(ModLane& l, CaptureLoop& loop,
                                   int steps = 8, float prob = 1.f) {
    loop.reset();
    l.init(48000.f, 4242);
    l.set_capture_loop(&loop);
    l.set_range(1.f);
    l.set_shape(0.9f);          // pulse/S&H region: distinct step values
    l.set_smooth(0.f);
    l.set_step(true, steps);
    l.set_probability(prob);
    l.set_rate_hz(1.f);         // 1 cycle/sec = 48000 samples/cycle
}

TEST_CASE("ModLane record: fired slots line up with STEP boundaries") {
    ModLane l; CaptureLoop loop;
    configure_step_capture(l, loop, 8, 1.f);
    for (int i = 0; i < 48000 + 500; ++i) l.process();  // > one full cycle
    loop.capture_now();
    // 8 steps over 192 slots => a boundary fires roughly every 24 slots.
    int fired_slots = 0;
    for (int s = 0; s < CaptureLoop::kSlots; ++s) if (loop.fired(s)) ++fired_slots;
    CHECK(fired_slots >= 6);   // ~8, tolerant of phase drift at the seam
    CHECK(fired_slots <= 10);
}

TEST_CASE("ModLane record: recorded value equals the lane target at that slot") {
    ModLane l; CaptureLoop loop;
    configure_step_capture(l, loop, 4, 1.f);
    // run to a known phase inside step 1 (phase ~0.30 => slot ~57), read target
    for (int i = 0; i < 48000; ++i) l.process();   // align to cycle start-ish
    for (int i = 0; i < 14400; ++i) l.process();   // +0.30 cycle
    float tgt = l.target();
    int   slot = static_cast<int>(l.phase() * CaptureLoop::kSlots);
    loop.capture_now();
    CHECK(loop.value(slot) == doctest::Approx(tgt));
}

TEST_CASE("ModLane record: deterministic loop is identical one cycle apart") {
    ModLane l; CaptureLoop loop;
    configure_step_capture(l, loop, 8, 1.f);   // prob 1, evolve 0 => metronomic
    for (int i = 0; i < 48000 * 3; ++i) l.process();   // settle
    loop.capture_now();
    float a[CaptureLoop::kSlots];
    for (int s = 0; s < CaptureLoop::kSlots; ++s) a[s] = loop.value(s);
    for (int i = 0; i < 48000; ++i) l.process();       // one more full cycle
    loop.capture_now();
    for (int s = 0; s < CaptureLoop::kSlots; ++s)
        CHECK(loop.value(s) == doctest::Approx(a[s]));
}

TEST_CASE("ModLane record: a lane with no capture loop is unaffected") {
    ModLane a; a.init(48000.f, 99);
    a.set_step(true, 8); a.set_shape(0.9f); a.set_rate_hz(1.f);
    ModLane b; b.init(48000.f, 99);
    b.set_step(true, 8); b.set_shape(0.9f); b.set_rate_hz(1.f);
    CaptureLoop loop; loop.reset();
    b.set_capture_loop(&loop);
    // recording must not consume RNG => identical output streams
    for (int i = 0; i < 48000 * 2; ++i)
        CHECK(a.process() == doctest::Approx(b.process()));
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run:

```bash
cmake --build build
```

Expected: FAIL to compile — `'set_capture_loop' is not a member of 'spky::ModLane'`.

- [ ] **Step 3: Implement the record hook**

In `engine/mod/lane.h`, add a forward declaration before the class (after the includes, before `class ModLane`):

```cpp
namespace spky {

class CaptureLoop;   // engine/mod/capture.h — wired to the PITCH lane only

// One modulation lane: wavetable core -> probability -> step/flow -> smooth
```

In `engine/mod/lane.h`, add the public setter right after `void reset(float phase = 0.f);`:

```cpp
    void reset(float phase = 0.f);

    // M3 capture: wired once at init on the PITCH lane only (nullptr elsewhere).
    void set_capture_loop(CaptureLoop* loop) { _capture_loop = loop; }
```

In `engine/mod/lane.h`, add the private helper declarations after `float _compute_raw() const;`:

```cpp
    void  _update_slew();
    void  _on_boundary();
    float _compute_raw() const;
    int   _phase_slot() const;      // floor(phase * kSlots), clamped
    void  _record_slot();           // roll _target + fired into the ring
```

In `engine/mod/lane.h`, add the members after the EVOLVE block (after `_ev_rate`):

```cpp
    float _ev_phase = 0.f;   // EVOLVE random-walk offsets: shape / phase / rate (Task 7)
    float _ev_shape = 0.f;
    float _ev_rate  = 0.f;

    // M3 capture (recording state; replay state added in Task 3)
    CaptureLoop* _capture_loop = nullptr;
    int          _rec_slot = -1;    // last ring slot written this pass
    bool         _rec_fired = false;// a boundary has fired since entering _rec_slot
```

In `engine/mod/lane.cpp`, add the include at the top after the existing includes:

```cpp
#include "mod/lane.h"
#include "mod/waveforms.h"
#include "mod/range.h"
#include "mod/capture.h"
#include "util/math.h"
#include <cmath>
```

In `engine/mod/lane.cpp`, reset the recording state in `init()` — change:

```cpp
    _ev_phase = 0.f;
    _ev_shape = 0.f;
    _ev_rate  = 0.f;
    _update_slew();
```

to:

```cpp
    _ev_phase = 0.f;
    _ev_shape = 0.f;
    _ev_rate  = 0.f;
    _rec_slot = -1;
    _rec_fired = false;
    _update_slew();
```

In `engine/mod/lane.cpp`, add the two helpers after `_on_boundary()` (before `process()`):

```cpp
int ModLane::_phase_slot() const {
    int s = static_cast<int>(_phase * CaptureLoop::kSlots);
    return s >= CaptureLoop::kSlots ? CaptureLoop::kSlots - 1 : s;
}

void ModLane::_record_slot() {
    int slot = _phase_slot();
    if (slot != _rec_slot) { _rec_fired = false; _rec_slot = slot; } // new slot: clear
    if (_fired) _rec_fired = true;                                   // latch a fire
    _capture_loop->record(slot, _target, _rec_fired);
}
```

In `engine/mod/lane.cpp`, insert the record call in `process()` — change:

```cpp
    } else {
        if (wrapped) _on_boundary();
        if (!_frozen) _target = _compute_raw();        // continuous in FLOW
    }

    float smoothed = _slew.process(_target);
```

to:

```cpp
    } else {
        if (wrapped) _on_boundary();
        if (!_frozen) _target = _compute_raw();        // continuous in FLOW
    }

    if (_capture_loop) _record_slot();                 // roll into the ring

    float smoothed = _slew.process(_target);
```

- [ ] **Step 4: Run tests to verify they pass**

Run:

```bash
cmake --build build
./build/spky_tests.exe --test-case="ModLane record*"
ctest --test-dir build --output-on-failure
```

Expected: the four new cases PASS; full suite stays green (recording does not touch the RNG, so existing `test_lane`/`test_super_modulator` determinism holds).

- [ ] **Step 5: Commit**

```bash
git add engine/mod/lane.h engine/mod/lane.cpp tests/test_capture.cpp
git commit -m "feat(m3): ModLane rolling record into CaptureLoop ring

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 3: `ModLane` replay rule

Swap the source. When replaying, the phase still advances (RATE drives loop speed) but boundaries come from the recorded fired slots; the live PROBABILITY dice, SMOOTH, and RANGE keep acting; EVOLVE is ignored on this lane.

**Files:**
- Modify: `engine/mod/lane.h` (add `set_replay`/`replaying`, private `_replaying`/`_replay_step`, members)
- Modify: `engine/mod/lane.cpp` (init reset; helpers; replace `process()`)
- Modify: `tests/test_capture.cpp` (append cases)

**Interfaces:**
- Consumes: `CaptureLoop`, the record hook (Task 2).
- Produces:
  - `void ModLane::set_replay(bool on);` — turns replay on/off; on-transition resets the play slot cursor so the current slot is re-evaluated (phase-synchronous, no jump).
  - `bool ModLane::replaying() const;` — effective state: replay requested AND a valid loop exists.
  - Replay behavior: while `replaying()`, `_target` = loop value at the current slot (held when frozen); entering a slot whose loop fired flag is set rolls the live PROBABILITY dice — pass → `_fired`, freeze lifts; fail → frozen (hold). Recording pauses; the RNG is only consumed by the dice roll (same call as generative).

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_capture.cpp`:

```cpp
// Capture a metronomic STEP loop, then return the lane in replay mode.
static void capture_and_replay(ModLane& l, CaptureLoop& loop,
                               int steps = 8, float prob = 1.f) {
    configure_step_capture(l, loop, steps, prob);
    for (int i = 0; i < 48000 * 2; ++i) l.process();  // record >= 2 cycles
    loop.capture_now();
    l.set_replay(true);
    CHECK(l.replaying() == true);
}

TEST_CASE("replay: probability 1 yields the identical target sequence every cycle") {
    ModLane l; CaptureLoop loop;
    capture_and_replay(l, loop, 8, 1.f);
    // advance to a cycle boundary, then record two full cycles of targets
    for (int i = 0; i < 48000; ++i) l.process();
    float cyc1[48000];
    for (int i = 0; i < 48000; ++i) { l.process(); cyc1[i] = l.target(); }
    for (int i = 0; i < 48000; ++i) {
        l.process();
        CHECK(l.target() == doctest::Approx(cyc1[i]));
    }
}

TEST_CASE("replay: set_replay before a valid capture does nothing") {
    ModLane l; CaptureLoop loop; loop.reset();
    l.init(48000.f, 7); l.set_capture_loop(&loop);
    l.set_replay(true);
    CHECK(l.replaying() == false);   // loop not valid yet -> stays generative
}

TEST_CASE("replay: probability < 1 suppresses triggers and holds the pitch") {
    ModLane l; CaptureLoop loop;
    capture_and_replay(l, loop, 8, 1.f);
    l.set_probability(0.f);          // every recorded trigger now fails
    int fires = 0;
    bool changed = false;
    float prev = l.target();
    for (int i = 0; i < 48000 * 2; ++i) {
        l.process();
        if (l.fired()) ++fires;
        if (l.target() != doctest::Approx(prev)) changed = true;
    }
    CHECK(fires == 0);               // no engine triggers
    CHECK(changed == false);         // pitch frozen at the held value
}

TEST_CASE("replay: EVOLVE is ignored on the replaying lane") {
    ModLane l; CaptureLoop loop;
    capture_and_replay(l, loop, 8, 1.f);
    l.set_evolve(1.f);               // would wander a live lane
    for (int i = 0; i < 48000; ++i) l.process();
    float cyc1[48000];
    for (int i = 0; i < 48000; ++i) { l.process(); cyc1[i] = l.target(); }
    for (int i = 0; i < 48000; ++i) {
        l.process();
        CHECK(l.target() == doctest::Approx(cyc1[i]));  // content + timing constant
    }
}

TEST_CASE("replay: SMOOTH still glides between loop steps") {
    ModLane l; CaptureLoop loop;
    capture_and_replay(l, loop, 4, 1.f);
    l.set_smooth(0.6f);              // audible glide
    bool gliding = false;
    for (int i = 0; i < 48000 * 2; ++i) {
        float out = l.process();
        // right after a boundary the smoothed output lags the loop target
        if (std::fabs(out - l.target()) > 0.02f) gliding = true;
    }
    CHECK(gliding == true);
}

TEST_CASE("replay: RANGE scales the loop down to off") {
    ModLane l; CaptureLoop loop;
    capture_and_replay(l, loop, 8, 1.f);
    l.set_range(0.f);                // off
    for (int i = 0; i < 48000; ++i) {
        float out = l.process();
        CHECK(out == doctest::Approx(0.f));
    }
}

TEST_CASE("replay: persists across replay-off (two-buffer promise)") {
    ModLane l; CaptureLoop loop;
    capture_and_replay(l, loop, 8, 1.f);
    for (int i = 0; i < 48000; ++i) l.process();
    float cyc[48000];
    for (int i = 0; i < 48000; ++i) { l.process(); cyc[i] = l.target(); }
    l.set_replay(false);
    for (int i = 0; i < 48000 * 2; ++i) l.process();   // generative runs on
    l.set_replay(true);
    for (int i = 0; i < 48000; ++i) l.process();        // realign to boundary
    for (int i = 0; i < 48000; ++i) {
        l.process();
        CHECK(l.target() == doctest::Approx(cyc[i]));    // same loop returns
    }
}

TEST_CASE("replay: toggling replay does not jump beyond one boundary step") {
    ModLane l; CaptureLoop loop;
    capture_and_replay(l, loop, 8, 1.f);
    float before = l.phase();
    l.set_replay(false);
    float after = l.process();  (void)after;
    // phase advances by one sample only; no phase reset on toggle
    CHECK(std::fabs(l.phase() - before) < 0.001f);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run:

```bash
cmake --build build
```

Expected: FAIL to compile — `'set_replay' is not a member of 'spky::ModLane'`.

- [ ] **Step 3: Implement replay**

In `engine/mod/lane.h`, extend the public API — change:

```cpp
    // M3 capture: wired once at init on the PITCH lane only (nullptr elsewhere).
    void set_capture_loop(CaptureLoop* loop) { _capture_loop = loop; }
```

to:

```cpp
    // M3 capture: wired once at init on the PITCH lane only (nullptr elsewhere).
    void set_capture_loop(CaptureLoop* loop) { _capture_loop = loop; }
    void set_replay(bool on) { _replay = on; if (on) _play_slot = -1; }
    bool replaying() const { return _replaying(); }
```

In `engine/mod/lane.h`, add the private helper declarations — change:

```cpp
    int   _phase_slot() const;      // floor(phase * kSlots), clamped
    void  _record_slot();           // roll _target + fired into the ring
```

to:

```cpp
    int   _phase_slot() const;      // floor(phase * kSlots), clamped
    void  _record_slot();           // roll _target + fired into the ring
    bool  _replaying() const;       // replay requested AND loop valid
    void  _replay_step();           // loop is the source this sample
```

In `engine/mod/lane.h`, extend the M3 members — change:

```cpp
    CaptureLoop* _capture_loop = nullptr;
    int          _rec_slot = -1;    // last ring slot written this pass
    bool         _rec_fired = false;// a boundary has fired since entering _rec_slot
```

to:

```cpp
    CaptureLoop* _capture_loop = nullptr;
    int          _rec_slot = -1;    // last ring slot written this pass
    bool         _rec_fired = false;// a boundary has fired since entering _rec_slot
    bool         _replay = false;   // replay requested (effective only if loop valid)
    int          _play_slot = -1;   // last slot evaluated while replaying
```

In `engine/mod/lane.cpp`, reset the replay state in `init()` — change:

```cpp
    _rec_slot = -1;
    _rec_fired = false;
    _update_slew();
```

to:

```cpp
    _rec_slot = -1;
    _rec_fired = false;
    _replay = false;
    _play_slot = -1;
    _update_slew();
```

In `engine/mod/lane.cpp`, add the two replay helpers after `_record_slot()`:

```cpp
bool ModLane::_replaying() const {
    return _replay && _capture_loop && _capture_loop->valid();
}

void ModLane::_replay_step() {
    int slot = _phase_slot();
    if (slot != _play_slot) {
        _play_slot = slot;
        if (_capture_loop->fired(slot)) {
            bool fire = _rng.next_unipolar() < _prob;  // live PROBABILITY dice
            _frozen = !fire;
            if (fire) _fired = true;                   // trigger; freeze lifts
        }
    }
    if (!_frozen) _target = _capture_loop->value(slot); // curve, or held step
}
```

In `engine/mod/lane.cpp`, replace the whole `process()` (currently lines 75-105) with:

```cpp
float ModLane::process() {
    _fired = false;
    const bool replay = _replaying();

    _phase += _phase_inc * (replay ? 1.f : (1.f + _ev_rate));  // no EVOLVE rate on replay
    bool wrapped = false;
    while (_phase >= 1.f) { _phase -= 1.f; wrapped = true; }

    if (replay) {
        _replay_step();                                 // the loop is the source
    } else {
        if (wrapped) {
            _sh_cycle = _rng.next_bipolar();            // new S&H value per cycle
            if (_evolve > 0.f) {                        // EVOLVE random walk (live only)
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
            if (!_frozen) _target = _compute_raw();     // continuous in FLOW
        }

        if (_capture_loop) _record_slot();              // roll into the ring
    }

    float smoothed = _slew.process(_target);
    return apply_range(smoothed, _range);
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run:

```bash
cmake --build build
./build/spky_tests.exe --test-case="replay*"
ctest --test-dir build --output-on-failure
```

Expected: all replay cases PASS; full suite green.

- [ ] **Step 5: Commit**

```bash
git add engine/mod/lane.h engine/mod/lane.cpp tests/test_capture.cpp
git commit -m "feat(m3): ModLane replay rule — loop source, live dice/smooth/range

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 4: `SuperModulator` ownership + wiring

`SuperModulator` owns exactly one `CaptureLoop`, wires it to `LANE_PITCH` in `init()`, and exposes the capture API. Replay must affect only the PITCH lane; the other four keep wandering.

**Files:**
- Modify: `engine/mod/super_modulator.h` (include; member; API)
- Modify: `engine/mod/super_modulator.cpp` (wire in `init()`)
- Modify: `tests/test_capture.cpp` (append cases)

**Interfaces:**
- Consumes: `CaptureLoop`, `ModLane::set_capture_loop/set_replay/replaying` (Tasks 1-3).
- Produces (all inline on `SuperModulator`):
  - `void capture_now();` — freeze the PITCH lane's ring.
  - `void set_replay(bool on);` — toggle PITCH-lane replay.
  - `bool replaying() const;` — PITCH-lane effective replay state.
  - `bool loop_valid() const;` — a loop has been captured.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_capture.cpp`:

```cpp
static void configure_sm_step(SuperModulator& sm, float prob = 1.f) {
    sm.init(48000.f, 1000);
    sm.set_range(1.f);
    sm.set_shape(0.9f);
    sm.set_smooth(0.f);
    sm.set_step(true, 8);
    sm.set_probability(prob);
    sm.set_rate(0.5f);           // some audible rate
    sm.set_sync_mode(SyncMode::Free);
}

TEST_CASE("SuperModulator: loop_valid false until capture") {
    SuperModulator sm; configure_sm_step(sm);
    CHECK(sm.loop_valid() == false);
    for (int i = 0; i < 48000 * 2; ++i) sm.process();
    sm.capture_now();
    CHECK(sm.loop_valid() == true);
}

TEST_CASE("SuperModulator: replay only swaps the PITCH lane") {
    SuperModulator sm; configure_sm_step(sm);
    for (int i = 0; i < 48000 * 2; ++i) sm.process();
    sm.capture_now();
    sm.set_replay(true);
    sm.set_evolve(1.f);           // wander the live lanes hard
    CHECK(sm.replaying() == true);

    // PITCH loop constant across cycles; a non-PITCH lane changes.
    for (int i = 0; i < 48000; ++i) sm.process();
    float pitch1[48000], motion1[48000];
    for (int i = 0; i < 48000; ++i) {
        sm.process();
        pitch1[i]  = sm.lane_output(LANE_PITCH);
        motion1[i] = sm.lane_output(LANE_MOTION);
    }
    bool pitch_const = true, motion_moved = false;
    for (int i = 0; i < 48000; ++i) {
        sm.process();
        if (sm.lane_output(LANE_PITCH)  != doctest::Approx(pitch1[i])) pitch_const = false;
        if (sm.lane_output(LANE_MOTION) != doctest::Approx(motion1[i])) motion_moved = true;
    }
    CHECK(pitch_const == true);
    CHECK(motion_moved == true);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run:

```bash
cmake --build build
```

Expected: FAIL to compile — `'capture_now' is not a member of 'spky::SuperModulator'`.

- [ ] **Step 3: Implement ownership + wiring**

In `engine/mod/super_modulator.h`, add the include after the existing `mod/` includes:

```cpp
#include "mod/lane.h"
#include "mod/lane_id.h"
#include "mod/capture.h"
```

In `engine/mod/super_modulator.h`, add the API after `float master_hz() const { return _master_hz; }`:

```cpp
    float master_hz()        const { return _master_hz; }

    // --- M3 capture sequencer (PITCH lane only) ---
    void capture_now()       { _capture.capture_now(); }
    void set_replay(bool on) { _lanes[LANE_PITCH].set_replay(on); }
    bool replaying()   const { return _lanes[LANE_PITCH].replaying(); }
    bool loop_valid()  const { return _capture.valid(); }
```

In `engine/mod/super_modulator.h`, add the member after the lanes/out arrays:

```cpp
    std::array<ModLane, LANE_COUNT> _lanes;
    std::array<float, LANE_COUNT>   _out {};
    CaptureLoop                     _capture;   // one loop, PITCH lane only
```

In `engine/mod/super_modulator.cpp`, wire the loop in `init()` — change:

```cpp
void SuperModulator::init(float sample_rate, uint32_t seed_base) {
    _sr = sample_rate;
    for (int i = 0; i < LANE_COUNT; ++i) {
        _lanes[i].init(sample_rate, seed_base + static_cast<uint32_t>(i) * 2654435761u);
        _out[i] = 0.f;
    }
    _update_rate();
}
```

to:

```cpp
void SuperModulator::init(float sample_rate, uint32_t seed_base) {
    _sr = sample_rate;
    for (int i = 0; i < LANE_COUNT; ++i) {
        _lanes[i].init(sample_rate, seed_base + static_cast<uint32_t>(i) * 2654435761u);
        _out[i] = 0.f;
    }
    _capture.reset();
    _lanes[LANE_PITCH].set_capture_loop(&_capture);   // capture is PITCH-only
    _update_rate();
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run:

```bash
cmake --build build
./build/spky_tests.exe --test-case="SuperModulator: loop*,SuperModulator: replay*"
ctest --test-dir build --output-on-failure
```

Expected: both new cases PASS; full suite green (existing `test_super_modulator` unaffected — the wired loop only records, never touches the RNG or output).

- [ ] **Step 5: Commit**

```bash
git add engine/mod/super_modulator.h engine/mod/super_modulator.cpp tests/test_capture.cpp
git commit -m "feat(m3): SuperModulator owns one CaptureLoop, wires PITCH lane

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 5: `Instrument` delegation + live-layer checks

Delegate the capture API per part in the existing one-line style, then verify the full `Part` chain re-voices the loop live: TUNE transposes, scale changes requantize, the recorded trigger pattern drives the engine via `lane_fired`.

**Files:**
- Modify: `engine/instrument.h` (four delegation methods)
- Modify: `tests/test_capture.cpp` (append cases)

**Interfaces:**
- Consumes: `SuperModulator::capture_now/set_replay/replaying/loop_valid` via `Part::mod()` (Task 4).
- Produces (inline on `Instrument`, `int p` first arg like the rest of the API):
  - `void capture_now(int p);`
  - `void set_replay(int p, bool on);`
  - `bool replaying(int p) const;`
  - `bool loop_valid(int p) const;`

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_capture.cpp`:

```cpp
// Drive part 0 as a Dorian STEP melody with a capture loop, return it replaying.
static void inst_capture_replay(Instrument& inst) {
    inst.init(48000.f);                 // engine-only init (no FX chain)
    inst.set_tempo_bpm(120.f);
    inst.set_engine(0, ENGINE_SYNTH);
    inst.set_step(0, true, 8);
    inst.set_shape(0, 0.9f);
    inst.set_smooth(0, 0.f);
    inst.set_range(0, 1.f);
    inst.set_probability(0, 1.f);
    inst.set_depth(0, 1.f);
    inst.set_rate(0, 0.5f);
    inst.set_target_active(0, LANE_PITCH, true);
    inst.set_target_base(0, LANE_PITCH, 0.5f);
    inst.set_scale(SCALE_DORIAN);
    inst.set_tune(0, 0.5f);             // neutral
    float l, r;
    for (int i = 0; i < 48000 * 2; ++i) inst.process(nullptr, nullptr, &l, &r, 1);
    inst.capture_now(0);
    CHECK(inst.loop_valid(0) == true);
    inst.set_replay(0, true);
    CHECK(inst.replaying(0) == true);
}

static void collect_pitch_cv(Instrument& inst, float* out, int n) {
    float l, r;
    for (int i = 0; i < n; ++i) { inst.process(nullptr, nullptr, &l, &r, 1); out[i] = inst.pitch_cv(0); }
}

TEST_CASE("Instrument: TUNE transposes the replayed loop") {
    Instrument inst; inst_capture_replay(inst);
    float base[9600]; collect_pitch_cv(inst, base, 9600);
    inst.set_tune(0, 1.0f);             // +max transpose
    float tuned[9600]; collect_pitch_cv(inst, tuned, 9600);
    bool differs = false;
    for (int i = 0; i < 9600; ++i) if (tuned[i] != doctest::Approx(base[i])) differs = true;
    CHECK(differs == true);
}

TEST_CASE("Instrument: scale change requantizes the replayed loop") {
    Instrument inst; inst_capture_replay(inst);
    float dorian[9600]; collect_pitch_cv(inst, dorian, 9600);
    inst.set_scale(SCALE_WHOLE);        // different scale masks -> different pitches
    float whole[9600]; collect_pitch_cv(inst, whole, 9600);
    bool differs = false;
    for (int i = 0; i < 9600; ++i) if (whole[i] != doctest::Approx(dorian[i])) differs = true;
    CHECK(differs == true);
}

TEST_CASE("Instrument: recorded fired pattern drives triggers via lane_fired") {
    Instrument inst; inst_capture_replay(inst);
    float l, r;
    int fires = 0;
    for (int i = 0; i < 48000; ++i) {
        inst.process(nullptr, nullptr, &l, &r, 1);
        if (inst.lane_fired(0, LANE_PITCH)) ++fires;
    }
    CHECK(fires >= 4);                   // ~8 triggers/cycle at prob 1
}

TEST_CASE("Instrument: probability thinning on the loop holds notes (fewer triggers)") {
    Instrument inst; inst_capture_replay(inst);
    inst.set_probability(0, 0.f);        // all recorded triggers fail -> hold
    float l, r;
    int fires = 0;
    for (int i = 0; i < 48000 * 2; ++i) {
        inst.process(nullptr, nullptr, &l, &r, 1);
        if (inst.lane_fired(0, LANE_PITCH)) ++fires;
    }
    CHECK(fires == 0);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run:

```bash
cmake --build build
```

Expected: FAIL to compile — `'capture_now' is not a member of 'spky::Instrument'`.

- [ ] **Step 3: Implement delegation**

In `engine/instrument.h`, add the four methods after `float pitch_cv(int p) const { return _parts[p].pitch_cv(); }`:

```cpp
    float pitch_cv(int p) const { return _parts[p].pitch_cv(); }

    // --- M3 capture sequencer (per part) ---
    void capture_now(int p)         { _parts[p].mod().capture_now(); }
    void set_replay(int p, bool on) { _parts[p].mod().set_replay(on); }
    bool replaying(int p) const     { return _parts[p].mod().replaying(); }
    bool loop_valid(int p) const    { return _parts[p].mod().loop_valid(); }
```

- [ ] **Step 4: Run tests to verify they pass**

Run:

```bash
cmake --build build
./build/spky_tests.exe --test-case="Instrument: TUNE*,Instrument: scale*,Instrument: recorded*,Instrument: probability*"
ctest --test-dir build --output-on-failure
```

Expected: the four new cases PASS; full suite green.

- [ ] **Step 5: Commit**

```bash
git add engine/instrument.h tests/test_capture.cpp
git commit -m "feat(m3): Instrument per-part capture delegation

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 6: Desktop host — scenario actions + `mods.csv` column

Two new scenario actions map 1:1 to the API; `mods.csv` gains one `cap` column per part (replay state 0/1) so the capture behavior is CSV-verifiable.

**Files:**
- Modify: `host/render/scenario.cpp` (two `apply_event` branches)
- Modify: `host/render/main.cpp` (CSV header + per-part write)
- Modify: `tests/test_capture.cpp` (append a scenario-dispatch case)

**Interfaces:**
- Consumes: `Instrument::capture_now/set_replay/replaying` (Task 5); the existing `Event` struct (`part`, `flag`).
- Produces: scenario actions `"capture_now"` (uses `part`) and `"set_replay"` (uses `part`, `flag`); a `a_cap`/`b_cap` CSV column emitting `replaying(p)`.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_capture.cpp`:

```cpp
TEST_CASE("scenario: capture_now + set_replay dispatch through apply_event") {
    Instrument inst; inst.init(48000.f);
    inst.set_step(0, true, 8);
    inst.set_shape(0, 0.9f);
    inst.set_target_active(0, LANE_PITCH, true);
    inst.set_rate(0, 0.5f);
    float l, r;
    for (int i = 0; i < 48000 * 2; ++i) inst.process(nullptr, nullptr, &l, &r, 1);

    Event cap;   cap.action = "capture_now"; cap.part = 0;
    Event play;  play.action = "set_replay";  play.part = 0; play.flag = true;
    apply_event(inst, cap);
    apply_event(inst, play);
    CHECK(inst.loop_valid(0) == true);
    CHECK(inst.replaying(0) == true);

    Event stop; stop.action = "set_replay"; stop.part = 0; stop.flag = false;
    apply_event(inst, stop);
    CHECK(inst.replaying(0) == false);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run:

```bash
cmake --build build
./build/spky_tests.exe --test-case="scenario: capture*"
```

Expected: FAIL — `loop_valid(0)` is false (the actions are ignored as unknown until wired).

- [ ] **Step 3: Wire the scenario actions**

In `host/render/scenario.cpp`, add two branches in `apply_event` before the trailing comment — change:

```cpp
    else if (a == "trigger_manual")      inst.trigger_manual(e.part);
    // unknown actions are ignored on purpose (forward-compatible scenarios)
```

to:

```cpp
    else if (a == "trigger_manual")      inst.trigger_manual(e.part);
    else if (a == "capture_now")         inst.capture_now(e.part);
    else if (a == "set_replay")          inst.set_replay(e.part, e.flag);
    // unknown actions are ignored on purpose (forward-compatible scenarios)
```

- [ ] **Step 4: Add the CSV column**

In `host/render/main.cpp`, extend the header — change:

```cpp
        std::fprintf(csv, "t,"
            "a_src,a_size,a_pitch,a_motion,a_level,a_pcv,a_gate,"
            "a_fx0,a_fx1,a_fx2,a_fx3,a_fx4,a_voices,a_v0,a_v1,a_v2,a_v3,"
            "b_src,b_size,b_pitch,b_motion,b_level,b_pcv,b_gate,"
            "b_fx0,b_fx1,b_fx2,b_fx3,b_fx4,b_voices,b_v0,b_v1,b_v2,b_v3\n");
```

to:

```cpp
        std::fprintf(csv, "t,"
            "a_src,a_size,a_pitch,a_motion,a_level,a_pcv,a_gate,"
            "a_fx0,a_fx1,a_fx2,a_fx3,a_fx4,a_voices,a_v0,a_v1,a_v2,a_v3,a_cap,"
            "b_src,b_size,b_pitch,b_motion,b_level,b_pcv,b_gate,"
            "b_fx0,b_fx1,b_fx2,b_fx3,b_fx4,b_voices,b_v0,b_v1,b_v2,b_v3,b_cap\n");
```

In `host/render/main.cpp`, emit the value at the end of each part's row segment — change:

```cpp
                std::fprintf(csv, ",%d", inst.active_voices(p));
                for (int v = 0; v < 4; ++v)
                    std::fprintf(csv, ",%.4f", inst.voice_env(p, v));
            }
```

to:

```cpp
                std::fprintf(csv, ",%d", inst.active_voices(p));
                for (int v = 0; v < 4; ++v)
                    std::fprintf(csv, ",%.4f", inst.voice_env(p, v));
                std::fprintf(csv, ",%d", inst.replaying(p) ? 1 : 0);
            }
```

- [ ] **Step 5: Run tests + build render to verify they pass**

Run:

```bash
cmake --build build
./build/spky_tests.exe --test-case="scenario: capture*"
ctest --test-dir build --output-on-failure
```

Expected: the scenario case PASS; full suite green; `render` links.

- [ ] **Step 6: Commit**

```bash
git add host/render/scenario.cpp host/render/main.cpp tests/test_capture.cpp
git commit -m "feat(m3): render host capture_now/set_replay actions + cap CSV column

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 7: Demo scenario + bit-determinism verification

The `capture_loop.json` demo exercises the whole path end-to-end; two identical renders must be bit-identical (the determinism invariant), and the CSV must show the loop repeating while the `cap` column toggles.

**Files:**
- Create: `host/render/scenarios/capture_loop.json`

**Interfaces:**
- Consumes: all scenario actions from Task 6 plus the existing melody/scale/tune actions.
- Produces: a renderable demo; no code interface.

- [ ] **Step 1: Write the demo scenario**

Create `host/render/scenarios/capture_loop.json`:

```json
{
  "sample_rate": 48000,
  "bpm": 120,
  "duration_s": 40,
  "init": [
    {"action":"set_engine","part":0,"value":"synth"},
    {"action":"set_sync_mode","part":0,"value":"sync"},
    {"action":"set_rate","part":0,"value":0.5},
    {"action":"set_step","part":0,"flag":true,"ivalue":16},
    {"action":"set_shape","part":0,"value":0.7},
    {"action":"set_range","part":0,"value":0.9},
    {"action":"set_smooth","part":0,"value":0.05},
    {"action":"set_probability","part":0,"value":0.6},
    {"action":"set_depth","part":0,"value":1.0},
    {"action":"set_scale","value":"dorian"},
    {"action":"set_quant_mode","part":0,"value":"scale"},
    {"action":"set_target_active","part":0,"slot":2,"flag":true},
    {"action":"set_target_base","part":0,"slot":2,"value":0.4}
  ],
  "events": [
    {"t":20.0,"action":"set_probability","part":0,"value":1.0},
    {"t":20.0,"action":"capture_now","part":0},
    {"t":20.0,"action":"set_replay","part":0,"flag":true},
    {"t":26.0,"action":"set_probability","part":0,"value":0.5},
    {"t":30.0,"action":"set_probability","part":0,"value":1.0},
    {"t":30.0,"action":"set_tune","part":0,"value":0.75},
    {"t":33.0,"action":"set_replay","part":0,"flag":false},
    {"t":36.0,"action":"set_replay","part":0,"flag":true}
  ]
}
```

- [ ] **Step 2: Render it (smoke)**

Run:

```bash
cmake --build build
./build/render.exe host/render/scenarios/capture_loop.json cap.wav cap.csv
```

Expected: prints `wrote cap.wav (1920000 frames) and cap.csv`.

- [ ] **Step 3: Verify the bit-determinism invariant**

Run the identical render twice and compare bytes:

```bash
./build/render.exe host/render/scenarios/capture_loop.json a.wav a.csv
./build/render.exe host/render/scenarios/capture_loop.json b.wav b.csv
cmp a.wav b.wav && echo "BIT-IDENTICAL"
```

Expected: `cmp` is silent (exit 0) and prints `BIT-IDENTICAL`. If it differs, the replay path consumed the RNG differently between runs — stop and debug before proceeding.

- [ ] **Step 4: Verify the acceptance behavior in the CSV**

Confirm the `a_cap` column is 0 before t=20 and 1 during replay, and the PITCH pitch-CV (`a_pcv`) repeats across cycles while replaying. Inspect with the Read tool (open `cap.csv`) or:

```bash
head -1 cap.csv
```

Expected: header ends with `...,a_cap,...,b_cap`. Read rows near t≈19 (`a_cap` = 0) and t≈22 (`a_cap` = 1); the `a_pcv` values in the replay region repeat with the loop period, and momentary `a_cap` = 0 around t≈34 (replay off) then 1 again after t≈36 with the **same** `a_pcv` pattern returning.

- [ ] **Step 5: Full regression + commit**

Run:

```bash
ctest --test-dir build --output-on-failure
```

Expected: all tests pass. Then commit (the scratch `*.wav`/`*.csv` are not added):

```bash
git add host/render/scenarios/capture_loop.json
git commit -m "feat(m3): capture_loop demo scenario + bit-determinism verified

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

## Self-Review

**Spec coverage** (design doc §Behavior model / Module changes / Desktop host / Testing / Acceptance):

| Spec item | Task |
|---|---|
| `CaptureLoop` new, 192 slots, ring + frozen buffers, init state | Task 1 |
| Rolling record of `_target` + fired into the ring | Task 2 |
| Retroactive seam (window, not cycle start) | Task 1 (buffer), Task 2 (lane determinism) |
| Replay rule: dice on fired slots, hold when frozen, EVOLVE ignored, no ev_rate on phase | Task 3 |
| Downstream SMOOTH/RANGE stay live | Task 3 |
| Phase-sync toggle, persistence across replay-off | Task 3 |
| `set_capture_loop`/`set_replay`/`replaying` on lane | Tasks 2-3 |
| `SuperModulator` owns one loop, wires PITCH only, API | Task 4 |
| `Instrument` delegation (`capture_now`/`set_replay`/`replaying`/`loop_valid`) | Task 5 |
| Live layers: TUNE transpose, scale requantize, trigger path, probability hold | Task 5 |
| Scenario actions `capture_now`/`set_replay`, `mods.csv` cap column | Task 6 |
| `capture_loop.json` demo | Task 7 |
| Bit-determinism invariant | Task 7 (verified), Tasks 2/4 (record never touches RNG) |
| `engine/` compiles with no libDaisy; existing tests stay green | every task Step 4/5 |

**Placeholder scan:** no TBD/TODO/"handle edge cases"; every code step shows the full code. Each test step contains real assertions.

**Type consistency:** `CaptureLoop` API (`reset`, `record(int,float,bool)`, `capture_now`, `value(int)`, `fired(int)`, `valid`, `kSlots`) is used identically in Tasks 1-4. `ModLane` names (`set_capture_loop`, `set_replay`, `replaying`, `_phase_slot`, `_record_slot`, `_replaying`, `_replay_step`, `_capture_loop`, `_rec_slot`, `_rec_fired`, `_replay`, `_play_slot`) are consistent across Tasks 2-3. `SuperModulator`/`Instrument` methods (`capture_now`, `set_replay`, `replaying`, `loop_valid`) match across Tasks 4-6. Scenario action strings `"capture_now"`/`"set_replay"` match between Task 6 dispatch and Task 7 JSON. CSV column names `a_cap`/`b_cap` are consistent within Task 6.

**Note on one spec detail:** the spec's module table names the class file `engine/mod/capture.h/.cpp`. This plan makes it header-only (`capture.h` only) — the class is a dumb fixed buffer with no out-of-line code, matching the existing header-only helpers (`rng.h`, `range.h`), and this avoids adding a compiled source to both the `spky_tests` and `render` CMake targets. Behavior and public API are exactly as specified.

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-07-12-spotykach-capture-sequencer.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration.

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints.

**Which approach?**

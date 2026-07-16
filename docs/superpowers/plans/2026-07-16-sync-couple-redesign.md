# SYNC / COUPLE Redesign Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the per-part Free/Sync/Triplet toggles with one global SYNC switch backed by a new engine transport phase; COUPLE becomes a texture control in sync mode and gains grid gravity in free mode.

**Architecture:** A new `Transport` (beat accumulator, owned by `Center`) is the single time reference. `SuperModulator` gets a global synced flag (rate ladder with dotted/triplet divisions) and a split pitch/mod rate-scale hook. `Center::update()` branches into a grid world (pitch servo onto the transport, COUPLE scales only mod-lane wander) and a free world (today's Kuramoto plus grid gravity above COUPLE 0.5). Hosts: scenario action rename, VCV panel layout "A" via the generator.

**Tech Stack:** C++17 header-mostly engine (no heap, host-agnostic), doctest, CMake+Ninja+clang (desktop), Rack-SDK 2.6.6 Makefile (VCV), Python panel generator.

**Spec:** `docs/superpowers/specs/2026-07-16-sync-couple-redesign-design.md`

## Global Constraints

- Repo: `/c/Users/bernd/Documents/AI/Spotykach` (all paths below relative to it; all commands from this cwd in Git Bash).
- Engine build+test: `source env.sh && cmake --build build -j && ./build/spky_tests` (filter: `./build/spky_tests -tc="*pattern*"`). If `build/` is unconfigured: `cmake -B build -S .` first.
- VCV build: `cd host/vcv && make -j4` (RACK_DIR defaults to `../../../Rack-SDK`; needs WinLibs mingw g++ and MSYS2 make on PATH; `EXTRA_CXXFLAGS=-std=c++17` is already in the Makefile).
- Engine stays host-agnostic: no Rack/JSON types below `engine/`. No heap allocation in the audio path.
- Every commit ends with trailer: `Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>`
- Hardware constraint: net control count must not grow (this plan: −2 SW3, +1 SW2).
- **Known spec deviation (intentional):** the spec's ladder listing groups divisions per family; the implemented ladder below is strictly speed-sorted so the knob is monotonic. Task 1 updates the spec line to match.

---

### Task 1: Division ladder (`engine/mod/divisions.h`)

**Files:**
- Create: `engine/mod/divisions.h`
- Create: `tests/test_divisions.cpp`
- Modify: `CMakeLists.txt` (add test file to `spky_tests` sources, after `tests/test_rng.cpp`)
- Modify: `docs/superpowers/specs/2026-07-16-sync-couple-redesign-design.md` (ladder line)

**Interfaces:**
- Consumes: `spky::clampf` from `engine/util/math.h`.
- Produces (used by Tasks 3–5, 7): `struct Division { float cpb; const char* name; }`, `kDivisions[17]`, `kDivisionCount`, `int division_index(float norm)`, `float division_hz(int idx, float bpm)`, `int nearest_division(float hz, float bpm)`, `float free_hz(float norm)`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_divisions.cpp`:

```cpp
#include <doctest/doctest.h>
#include <cmath>
#include "mod/divisions.h"
using namespace spky;

TEST_CASE("divisions: 17 entries, strictly speed-sorted") {
    CHECK(kDivisionCount == 17);
    for (int i = 1; i < kDivisionCount; ++i)
        CHECK(kDivisions[i].cpb > kDivisions[i - 1].cpb);
}

TEST_CASE("divisions: knob endpoints and center land on the right rungs") {
    CHECK(division_index(0.f) == 0);                       // 8 bars
    CHECK(division_index(1.f) == kDivisionCount - 1);      // 1/32
    CHECK(std::string(kDivisions[division_index(0.5f)].name) == "1/4");
}

TEST_CASE("divisions: hz math at 120 bpm") {
    // 1/4 note = 1 cycle per beat = 2 Hz at 120 bpm
    CHECK(division_hz(8, 120.f) == doctest::Approx(2.f));
    // 8 bars = 32 beats -> 0.0625 Hz
    CHECK(division_hz(0, 120.f) == doctest::Approx(0.0625f));
    // 1/8T = 3 cycles per beat -> 6 Hz
    CHECK(division_hz(13, 120.f) == doctest::Approx(6.f));
}

TEST_CASE("divisions: nearest_division snaps in log space") {
    CHECK(nearest_division(2.1f, 120.f) == 8);    // just above 1/4 -> 1/4
    CHECK(nearest_division(2.9f, 120.f) == 10);   // 1/4T is 3 Hz at 120
    CHECK(nearest_division(0.001f, 120.f) == 0);  // clamps to the slow end
    CHECK(nearest_division(100.f, 120.f) == 16);  // clamps to the fast end
}

TEST_CASE("divisions: free_hz spans 0.02..30 exponentially") {
    CHECK(free_hz(0.f) == doctest::Approx(0.02f));
    CHECK(free_hz(1.f) == doctest::Approx(30.f));
    CHECK(free_hz(0.5f) == doctest::Approx(std::sqrt(0.02f * 30.f)).epsilon(0.001));
}
```

Add to `CMakeLists.txt` in the `spky_tests` source list directly after `tests/test_rng.cpp`:

```cmake
    tests/test_divisions.cpp
```

- [ ] **Step 2: Run to verify it fails**

Run: `source env.sh && cmake --build build -j`
Expected: compile error — `mod/divisions.h: No such file or directory`.

- [ ] **Step 3: Write the implementation**

Create `engine/mod/divisions.h`:

```cpp
#pragma once
#include <cmath>
#include "util/math.h"

namespace spky {

// The musical rate ladder for SYNC mode: 17 detents, strictly speed-sorted,
// dotted/straight/triplet interleaved. cpb = cycles per beat; the bar-length
// entries assume 4/4. Names appear verbatim in the VCV RATE tooltip.
struct Division { float cpb; const char* name; };

inline constexpr Division kDivisions[] = {
    {1.f/32.f, "8 bars"}, {1.f/16.f, "4 bars"}, {1.f/8.f, "2 bars"},
    {1.f/4.f,  "1 bar"},  {1.f/3.f,  "1/2."},   {1.f/2.f, "1/2"},
    {2.f/3.f,  "1/4."},   {3.f/4.f,  "1/2T"},   {1.f,     "1/4"},
    {4.f/3.f,  "1/8."},   {3.f/2.f,  "1/4T"},   {2.f,     "1/8"},
    {8.f/3.f,  "1/16."},  {3.f,      "1/8T"},   {4.f,     "1/16"},
    {6.f,      "1/16T"},  {8.f,      "1/32"},
};
inline constexpr int kDivisionCount = 17;

inline int division_index(float norm) {
    return static_cast<int>(clampf(norm, 0.f, 1.f) * (kDivisionCount - 1) + 0.5f);
}

inline float division_hz(int idx, float bpm) {
    return (bpm / 60.f) * kDivisions[idx].cpb;
}

// nearest ladder rung to a free rate, compared in log space (ratio symmetry)
inline int nearest_division(float hz, float bpm) {
    int best = 0;
    float best_d = 1e30f;
    for (int i = 0; i < kDivisionCount; ++i) {
        const float d = std::fabs(std::log(hz / division_hz(i, bpm)));
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

// Free-mode rate curve. Lives here (not in super_modulator.cpp) so the VCV
// tooltip shows exactly the Hz the engine runs.
inline constexpr float kRateFreeMin = 0.02f;
inline constexpr float kRateFreeMax = 30.f;
inline float free_hz(float norm) {
    return kRateFreeMin * std::pow(kRateFreeMax / kRateFreeMin, clampf(norm, 0.f, 1.f));
}

} // namespace spky
```

In the spec (`docs/superpowers/specs/2026-07-16-sync-couple-redesign-design.md`), replace the ladder code block content with:

```
8bar 4bar 2bar 1bar 1/2. 1/2 1/4. 1/2T 1/4 1/8. 1/4T 1/8 1/16. 1/8T 1/16 1/16T 1/32
```

and append below it: `(Strictly speed-sorted so the knob is monotonic — 1/2T (0.75 cpb) is faster than 1/4. (0.667 cpb).)`

- [ ] **Step 4: Run to verify it passes**

Run: `source env.sh && cmake --build build -j && ./build/spky_tests -tc="divisions*"`
Expected: all 5 cases PASS.

- [ ] **Step 5: Commit**

```bash
git add engine/mod/divisions.h tests/test_divisions.cpp CMakeLists.txt docs/superpowers/specs/2026-07-16-sync-couple-redesign-design.md
git commit -m "engine: add the SYNC rate ladder (17 speed-sorted divisions)"
```

---

### Task 2: Transport (`engine/center/transport.h`)

**Files:**
- Create: `engine/center/transport.h`
- Create: `tests/test_transport.cpp`
- Modify: `CMakeLists.txt` (add test after `tests/test_divisions.cpp`)

**Interfaces:**
- Produces (used by Tasks 4–5 via `Center`, Task 3 via `Instrument`): `class Transport` with `void init(float ctrl_rate)`, `void set_bpm(float)`, `float bpm() const`, `void tick()`, `void clock_pulse()`, `void reset()`, `double beats() const`, `float beat_phase() const`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_transport.cpp`:

```cpp
#include <doctest/doctest.h>
#include <cmath>
#include "center/transport.h"
using namespace spky;

TEST_CASE("transport: beats advance at bpm/60 per second of control ticks") {
    Transport t;
    t.init(500.f);            // Center's control rate at 48 kHz
    t.set_bpm(120.f);         // 2 beats per second
    for (int i = 0; i < 500; ++i) t.tick();
    CHECK(t.beats() == doctest::Approx(2.0).epsilon(1e-6));
    CHECK(t.beat_phase() == doctest::Approx(0.f).epsilon(1e-4));
}

TEST_CASE("transport: clock_pulse snaps the phase to the nearest beat") {
    Transport t;
    t.init(500.f);
    t.set_bpm(120.f);
    for (int i = 0; i < 540; ++i) t.tick();   // 2.16 beats
    t.clock_pulse();
    CHECK(t.beats() == doctest::Approx(2.0));
    for (int i = 0; i < 210; ++i) t.tick();   // 2.84 beats
    t.clock_pulse();
    CHECK(t.beats() == doctest::Approx(3.0)); // rounds up too
}

TEST_CASE("transport: reset zeroes the downbeat") {
    Transport t;
    t.init(500.f);
    t.set_bpm(97.f);
    for (int i = 0; i < 1234; ++i) t.tick();
    t.reset();
    CHECK(t.beats() == doctest::Approx(0.0));
    CHECK(t.beat_phase() == doctest::Approx(0.f));
}
```

Add `tests/test_transport.cpp` to the `spky_tests` list in `CMakeLists.txt` after `tests/test_divisions.cpp`.

- [ ] **Step 2: Run to verify it fails**

Run: `source env.sh && cmake --build build -j`
Expected: compile error — `center/transport.h: No such file or directory`.

- [ ] **Step 3: Write the implementation**

Create `engine/center/transport.h`:

```cpp
#pragma once
#include <cmath>

namespace spky {

// Master transport: a running beat counter advanced at the Center's control
// rate. The host reports external clock edges (one pulse per beat) through
// clock_pulse(); RST zeroes the downbeat. The accumulator is double — float
// loses beat-phase precision within minutes at 500 ticks/s.
class Transport {
public:
    void init(float ctrl_rate) { _cr = ctrl_rate; _beats = 0.0; }
    void set_bpm(float bpm)    { _bpm = bpm; }
    float bpm() const          { return _bpm; }

    void tick()        { _beats += static_cast<double>(_bpm) / (60.0 * static_cast<double>(_cr)); }
    void clock_pulse() { _beats = std::round(_beats); }   // snap to the nearest beat
    void reset()       { _beats = 0.0; }

    double beats() const     { return _beats; }
    float beat_phase() const { return static_cast<float>(_beats - std::floor(_beats)); }

private:
    double _beats = 0.0;
    float  _bpm = 120.f;
    float  _cr  = 500.f;
};

} // namespace spky
```

- [ ] **Step 4: Run to verify it passes**

Run: `source env.sh && cmake --build build -j && ./build/spky_tests -tc="transport*"`
Expected: 3 cases PASS.

- [ ] **Step 5: Commit**

```bash
git add engine/center/transport.h tests/test_transport.cpp CMakeLists.txt
git commit -m "engine: add the master transport (beat accumulator, CLK/RST hooks)"
```

---

### Task 3: Engine API swap — global sync replaces per-part SyncMode

One atomic API change across engine + hosts + tests so every commit builds. After this task the instrument behaves like today's all-Free or all-Sync configurations; the new Center behaviors come in Tasks 4–5.

**Files:**
- Modify: `engine/mod/lane_id.h` (delete `SyncMode`)
- Modify: `engine/mod/super_modulator.h`, `engine/mod/super_modulator.cpp`
- Modify: `engine/instrument.h` (API), `engine/instrument.cpp` (tempo forward)
- Modify: `engine/center/center.h`, `engine/center/center.cpp` (compile shim only)
- Modify: `host/render/scenario.cpp` (action rename)
- Modify: `host/render/scenarios/ambient_wash.json`, `host/render/scenarios/demo_step_melody.json`, `host/render/scenarios/reverb_delay.json`, `host/render/scenarios/reverb_wash.json`
- Test: `tests/test_super_modulator.cpp`, `tests/test_center.cpp`, `tests/test_instrument.cpp`, `tests/test_part.cpp`, `tests/test_scenario.cpp`

**Interfaces:**
- Consumes: `division_index`, `division_hz`, `free_hz` from Task 1; `Transport` from Task 2.
- Produces (relied on by Tasks 4–7):
  - `SuperModulator::set_synced(bool)`, `bool synced() const`, `int division() const`
  - `SuperModulator::set_rate_scale(float pitch_s, float mod_s)`, `float pitch_scale() const`, `float mod_scale() const`
  - `Instrument::set_sync(bool)`, `Instrument::clock_pulse()`, `Instrument::reset_transport()`
  - `Center::set_sync(bool)`, `Center::set_tempo_bpm(float)`, `Center::clock_pulse()`, `Center::reset_transport()`, `const Transport& Center::transport() const`
  - Scenario action `"set_sync"` (global, `ivalue` 0/1); `set_sync_mode` is gone.

- [ ] **Step 1: Rewrite the sync tests in `tests/test_super_modulator.cpp` (failing first)**

Replace every `m.set_sync_mode(SyncMode::Free);` with nothing (Free is the default). Replace the three sync-mode test cases (lines ~23–33 and ~75–79: the straight-sync case, the `"super: triplet mode is 1.5x the straight sync rate"` case, and the `"super: sync_mode getter reflects the set mode"` case) with:

```cpp
TEST_CASE("super: synced rate snaps to the division ladder") {
    SuperModulator m;
    m.init(48000.f, 1u);
    m.set_tempo_bpm(120.f);
    m.set_synced(true);
    m.set_rate(0.5f);                                  // center detent = 1/4
    CHECK(m.base_hz() == doctest::Approx(2.f));        // 1 cpb at 120 bpm
    m.set_rate(0.52f);                                 // still inside the same detent
    CHECK(m.base_hz() == doctest::Approx(2.f));
    m.set_rate(0.f);
    CHECK(m.base_hz() == doctest::Approx(0.0625f));    // 8 bars
    m.set_rate(1.f);
    CHECK(m.base_hz() == doctest::Approx(16.f));       // 1/32 = 8 cpb
}

TEST_CASE("super: triplet rungs live on the ladder") {
    SuperModulator m;
    m.init(48000.f, 1u);
    m.set_tempo_bpm(120.f);
    m.set_synced(true);
    m.set_rate(13.f / 16.f);                           // index 13 = 1/8T
    CHECK(m.base_hz() == doctest::Approx(6.f));
    CHECK(std::string(kDivisions[m.division()].name) == "1/8T");
}

TEST_CASE("super: synced getter and free default") {
    SuperModulator m;
    m.init(48000.f, 1u);
    CHECK_FALSE(m.synced());
    m.set_synced(true);
    CHECK(m.synced());
}

TEST_CASE("super: split rate scale drives pitch and mod lanes separately") {
    SuperModulator m;
    m.init(48000.f, 1u);
    m.set_rate(0.5f);
    const float base = m.base_hz();
    m.set_rate_scale(1.f, 2.f);
    CHECK(m.pitch_scale() == doctest::Approx(1.f));
    CHECK(m.mod_scale()   == doctest::Approx(2.f));
    CHECK(m.master_hz()   == doctest::Approx(base));   // master follows the pitch lane
    m.set_rate_scale(0.5f, 1.f);
    CHECK(m.master_hz()   == doctest::Approx(base * 0.5f));
}
```

Add `#include "mod/divisions.h"` and `#include <string>` at the top of the file if missing.

- [ ] **Step 2: Run to verify the new tests fail**

Run: `source env.sh && cmake --build build -j`
Expected: compile errors — `set_synced` is not a member of `SuperModulator`.

- [ ] **Step 3: Engine implementation**

`engine/mod/lane_id.h` — delete the line `enum class SyncMode { Sync, SyncTriplet, Free };`.

`engine/mod/super_modulator.h`:
- Add `#include "mod/divisions.h"` after the existing includes.
- Replace `void set_sync_mode(SyncMode m) { _mode = m; _update_rate(); }` with:
```cpp
    void set_synced(bool on)       { _synced = on; _update_rate(); }
```
- Replace the M4 hook `void set_rate_scale(float s)  { _rate_scale = s; _apply_rate(); }` with:
```cpp
    void set_rate_scale(float pitch_s, float mod_s) {
        _pitch_scale = pitch_s; _mod_scale = mod_s; _apply_rate();
    }
    float pitch_scale() const { return _pitch_scale; }
    float mod_scale()   const { return _mod_scale; }
```
- Replace `SyncMode sync_mode() const { return _mode; }` with:
```cpp
    bool synced()   const { return _synced; }
    int  division() const { return division_index(_rate_norm); }
```
- In the private members, replace `SyncMode _mode = SyncMode::Free;` and `float _rate_scale = 1.f;` with:
```cpp
    bool     _synced = false;
    float    _pitch_scale = 1.f;   // COUPLE/DRIFT on the melody clock
    float    _mod_scale   = 1.f;   // COUPLE/DRIFT on the texture lanes
```

`engine/mod/super_modulator.cpp`:
- Delete the anonymous-namespace `kRateFreeMin`, `kRateFreeMax`, `free_hz`, `sync_hz` (all now `mod/divisions.h`).
- In `init()`, replace `_rate_scale = 1.f;` with `_pitch_scale = 1.f; _mod_scale = 1.f;`.
- Replace `_update_rate()` and `_apply_rate()`:
```cpp
void SuperModulator::_update_rate() {
    _base_hz = _synced ? division_hz(division_index(_rate_norm), _bpm)
                       : free_hz(_rate_norm);
    _apply_rate();
}

void SuperModulator::_apply_rate() {
    _master_hz = _base_hz * _pitch_scale;
    for (int i = 0; i < LANE_COUNT; ++i) {
        const float s = (i == LANE_PITCH) ? _pitch_scale : _mod_scale;
        _lanes[i].set_rate_hz(_base_hz * s * kLaneRatio[i]);
    }
}
```

`engine/instrument.h`:
- Delete `void set_sync_mode(int p, SyncMode m) { ... }`.
- After `void set_drift(float d) ...` in the M4 block, add:
```cpp
    void set_sync(bool on) {
        _center.set_sync(on);
        for (auto& p : _parts) p.mod().set_synced(on);
    }
    void clock_pulse()     { _center.clock_pulse(); }
    void reset_transport() { _center.reset_transport(); }
```

`engine/instrument.cpp` — in `set_tempo_bpm`, add the center forward:
```cpp
void Instrument::set_tempo_bpm(float bpm) {
    _bpm = bpm;
    _center.set_tempo_bpm(bpm);
    for (auto& p : _parts) p.mod().set_tempo_bpm(bpm);
}
```

`engine/center/center.h`:
- Add `#include "center/transport.h"` and `#include "mod/divisions.h"`.
- In the public section add:
```cpp
    void set_sync(bool on)        { _sync = on; }
    void set_tempo_bpm(float bpm) { _transport.set_bpm(bpm); }
    void clock_pulse()            { _transport.clock_pulse(); }
    void reset_transport()        { _transport.reset(); }
    const Transport& transport() const { return _transport; }
```
- In the private section add:
```cpp
    Transport _transport;
    bool      _sync = false;
```

`engine/center/center.cpp` — **compile shim only** (Tasks 4–5 do the real rewrite):
- In `init()`, add `_transport.init(_cr);` after `_cr = ...`.
- At the top of `update()`, add `_transport.tick();`.
- Delete the `a_free` / `b_free` / `mixed` lines and the mixed-pair comments; both banks are now always symmetric:
```cpp
    const float conv_e = _couple * 0.5f;
    const float conv_a = std::pow(fb / fa, conv_e);
    const float conv_b = std::pow(fa / fb, conv_e);
```
- Replace the `pull_a` / `pull_b` lines with `const float pull_a = 1.f - corr;` and `const float pull_b = 1.f + corr;`.
- Replace the two rate-hook calls with the split form:
```cpp
    a.set_rate_scale(mult_a * rate_drift_a, mult_a * rate_drift_a);
    b.set_rate_scale(mult_b * rate_drift_b, mult_b * rate_drift_b);
```

`host/render/scenario.cpp`:
- Delete `parse_sync()` (lines ~46–50).
- Replace `else if (a == "set_sync_mode") inst.set_sync_mode(e.part, parse_sync(e.svalue));` with:
```cpp
    else if (a == "set_sync")          inst.set_sync(e.ivalue != 0);
```

Scenario JSONs — replace every `set_sync_mode` line:
- `ambient_wash.json`: line 9 becomes `{"action":"set_sync","ivalue":1},` — delete the part-1 `set_sync_mode` line (35) entirely (sync is global; the old intent "A synced, B free" is superseded by the new COUPLE texture model).
- `demo_step_melody.json`: line 12 becomes `{ "action": "set_sync", "ivalue": 1 },`
- `reverb_delay.json` line 7 and `reverb_wash.json` line 7 become `{"action":"set_sync","ivalue":1},`

- [ ] **Step 4: Fix the remaining test references**

- `tests/test_part.cpp`: delete all five `p.mod().set_sync_mode(SyncMode::Free);` lines (Free is the default).
- `tests/test_instrument.cpp`: delete the two `inst.set_sync_mode(...)` lines (31–32).
- `tests/test_center.cpp`:
  - Lines 118, 133: delete the `set_sync_mode(SyncMode::Free)` pairs.
  - Lines 148, 172 (`SyncMode::Sync` pairs): replace each with
    ```cpp
    r.a.set_synced(true); r.b.set_synced(true); r.c.set_sync(true);
    ```
    **plus** read the surrounding test cases: if a case asserts couple-pull behavior on *synced* banks it describes the old world; mark it for Task 4 by renaming the case with a `[task4-rewrite]` suffix but make it compile now.
  - The two mixed-pair test cases (~lines 200–230, using Sync anchor + Free bank): delete both entirely — mixed pairs no longer exist.
- `tests/test_scenario.cpp`: line 16 becomes `{"action":"set_sync","ivalue":1},`.

- [ ] **Step 5: Run the full suite**

Run: `source env.sh && cmake --build build -j && ./build/spky_tests`
Expected: PASS (all suites, including untouched fx/voice suites). If a `[task4-rewrite]` case fails on behavior (not compile), disable it with `TEST_CASE("... " * doctest::skip())` and note it in the commit body — Task 4 replaces it.

- [ ] **Step 6: Commit**

```bash
git add -A engine host/render tests CMakeLists.txt
git commit -m "engine: one global SYNC replaces per-part SyncMode; split pitch/mod rate scale"
```

---

### Task 4: Center grid world — pitch servo + texture COUPLE

**Files:**
- Modify: `engine/center/center.h` (private helper), `engine/center/center.cpp` (sync branch)
- Test: `tests/test_center.cpp`

**Interfaces:**
- Consumes: `Transport` (Task 2), `kDivisions`/`division_hz` (Task 1), `SuperModulator::division()/set_rate_scale(p,m)/pitch_scale()/mod_scale()` (Task 3).
- Produces: behavior only — no new public API.

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_center.cpp` (and extend the `Rig` fixture with a lane-advancing helper):

```cpp
// Advance center AND lanes so phases actually move (ticks() only runs the center).
static void run_synced(Rig& r, int nticks) {
    for (int k = 0; k < nticks; ++k) {
        r.c.update(r.a, r.b, r.pa, r.pb);
        for (int s = 0; s < Center::kCtrlInterval; ++s) { r.a.process(); r.b.process(); }
    }
}

static float wrap_err(float e) { return e - std::floor(e + 0.5f); }

TEST_CASE("center grid: pitch sits on the division and the transport phase") {
    Rig r; r.init();
    r.a.set_tempo_bpm(120.f); r.b.set_tempo_bpm(120.f); r.c.set_tempo_bpm(120.f);
    r.a.set_synced(true); r.b.set_synced(true); r.c.set_sync(true);
    r.a.set_rate(0.5f);            // 1/4 -> 2 Hz
    r.b.set_rate(13.f / 16.f);     // 1/8T -> 6 Hz
    r.c.set_couple(1.f); r.c.set_drift(1.f);
    run_synced(r, 5000);           // 10 s: converge
    // rate: master pinned to the division despite full COUPLE + DRIFT
    CHECK(r.a.master_hz() == doctest::Approx(2.f).epsilon(0.02));
    CHECK(r.b.master_hz() == doctest::Approx(6.f).epsilon(0.02));
    // phase: each bank tracks its own grid target
    double beats = r.c.transport().beats();
    float ta = (float)(beats * 1.0 - std::floor(beats * 1.0));
    CHECK(std::fabs(wrap_err(ta - r.a.pitch_phase())) < 0.03f);
}

TEST_CASE("center grid: COUPLE 1 freezes the texture wander exactly") {
    Rig r; r.init(99u);
    r.a.set_synced(true); r.b.set_synced(true); r.c.set_sync(true);
    r.c.set_couple(1.f); r.c.set_drift(1.f);
    run_synced(r, 3000);           // let the weather walk build up
    CHECK(r.a.mod_scale() == doctest::Approx(r.a.pitch_scale()));   // wander factor == 1
    CHECK(r.b.mod_scale() == doctest::Approx(r.b.pitch_scale()));
}

TEST_CASE("center grid: COUPLE 0 lets DRIFT breathe the textures apart") {
    Rig r; r.init(99u);
    r.a.set_synced(true); r.b.set_synced(true); r.c.set_sync(true);
    r.c.set_couple(0.f); r.c.set_drift(1.f);
    bool wandered = false;
    for (int k = 0; k < 3000 && !wandered; ++k) {
        run_synced(r, 1);
        if (std::fabs(r.a.mod_scale() / r.a.pitch_scale() - 1.f) > 0.01f) wandered = true;
    }
    CHECK(wandered);
    // and the melody stays pinned regardless
    CHECK(r.a.master_hz() == doctest::Approx(r.a.base_hz()).epsilon(0.05));
}
```

Also now rewrite/delete any case marked `[task4-rewrite]` in Task 3 using these semantics.

- [ ] **Step 2: Run to verify they fail**

Run: `source env.sh && cmake --build build -j && ./build/spky_tests -tc="center grid*"`
Expected: FAIL — the shim still runs the free-world Kuramoto for synced banks (master_hz gets pulled off the division).

- [ ] **Step 3: Implement the sync branch**

`engine/center/center.h` — add the private helper declaration:

```cpp
    float _grid_servo(const SuperModulator& m) const;
```

`engine/center/center.cpp` — add the helper and branch `update()` after the DRIFT tap block (`pa.set_detune_cents(...)` stays common to both worlds) and the `dphi` / `_phase_err` computation (also common):

```cpp
// Per-tick rate correction that servos a synced bank's pitch phase onto its
// own grid target (transport beats x the bank's division). kKHard/kLockCap:
// same hard-lock servo as the free-world full-COUPLE lock — strong enough to
// outrun EVOLVE's +/-20% raw-rate wander, capped to stay click-free.
float Center::_grid_servo(const SuperModulator& m) const {
    const float cpb = kDivisions[m.division()].cpb;
    const double t = _transport.beats() * static_cast<double>(cpb);
    const float target = static_cast<float>(t - std::floor(t));
    float err = target - m.pitch_phase();
    err -= std::floor(err + 0.5f);                  // wrap to [-0.5, 0.5)
    return clampf(kKHard * err, -kLockCap, kLockCap);
}
```

In `update()`, wrap the existing (Task 3 shim) COUPLE block:

```cpp
    if (_sync) {
        // GRID WORLD: melody/steps live on the transport; COUPLE only sets how
        // tightly the four texture lanes follow. texture = 0 at full COUPLE
        // -> the DRIFT rate wander is fully suppressed and the mod lanes hold
        // their exact ratios (lockstep); smaller COUPLE lets it through.
        const float pitch_a = 1.f + _grid_servo(a);
        const float pitch_b = 1.f + _grid_servo(b);
        const float texture = 1.f - _couple;
        const float mod_a = pitch_a * std::pow(rate_drift_a, texture);
        const float mod_b = pitch_b * std::pow(rate_drift_b, texture);
        a.set_rate_scale(pitch_a, mod_a);
        b.set_rate_scale(pitch_b, mod_b);
    } else {
        // FREE WORLD: (the Task 3 shim body — geometric-mean convergence,
        // Kuramoto pull, hard lock at full COUPLE; Task 5 adds grid gravity)
        ...existing code...
    }
```

Note: `rate_drift_a/b` stay computed unconditionally before the branch (the free world uses them at full strength).

- [ ] **Step 4: Run the tests**

Run: `source env.sh && cmake --build build -j && ./build/spky_tests -tc="center*"`
Expected: all center cases PASS, including the three new grid cases.

- [ ] **Step 5: Commit**

```bash
git add engine/center tests/test_center.cpp
git commit -m "center: grid world — pitch servos onto the transport, COUPLE gates texture wander"
```

---

### Task 5: Center free world — zoned COUPLE with grid gravity

**Files:**
- Modify: `engine/center/center.cpp` (free branch)
- Test: `tests/test_center.cpp`

**Interfaces:**
- Consumes: `nearest_division`/`division_hz` (Task 1), `Transport::beats()/bpm()` (Task 2).
- Produces: behavior only.

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_center.cpp`:

```cpp
TEST_CASE("center free: below COUPLE 0.5 the tempo has zero influence") {
    // Identical rigs, wildly different BPM -> identical rate hooks.
    Rig r1; r1.init(5u); Rig r2; r2.init(5u);
    r1.c.set_tempo_bpm(120.f); r2.c.set_tempo_bpm(77.f);
    for (Rig* r : {&r1, &r2}) {
        r->a.set_rate(0.62f); r->b.set_rate(0.44f);
        r->c.set_couple(0.4f); r->c.set_drift(0.f);
    }
    for (int k = 0; k < 4000; ++k) {
        run_synced(r1, 1); run_synced(r2, 1);
        CHECK(r1.a.pitch_scale() == doctest::Approx(r2.a.pitch_scale()).epsilon(1e-6));
        CHECK(r1.b.pitch_scale() == doctest::Approx(r2.b.pitch_scale()).epsilon(1e-6));
    }
}

TEST_CASE("center free: full COUPLE lands the pair on the ladder and the downbeat") {
    Rig r; r.init(11u);
    r.c.set_tempo_bpm(120.f);
    r.a.set_rate(0.60f); r.b.set_rate(0.52f);   // free Hz, off-grid geometric mean
    r.c.set_couple(1.f); r.c.set_drift(0.f);
    run_synced(r, 15000);                        // 30 s to converge
    const float geo = std::sqrt(r.a.base_hz() * r.b.base_hz());
    const float grid = division_hz(nearest_division(geo, 120.f), 120.f);
    CHECK(r.a.master_hz() == doctest::Approx(grid).epsilon(0.03));
    CHECK(r.b.master_hz() == doctest::Approx(grid).epsilon(0.03));
    // pairwise phase lock still holds
    CHECK(std::fabs(r.c.phase_err()) < 0.03f);
    // and the pair sits on the transport grid phase
    const float cpb = kDivisions[nearest_division(geo, 120.f)].cpb;
    const double t = r.c.transport().beats() * (double)cpb;
    const float tgt = (float)(t - std::floor(t));
    CHECK(std::fabs(wrap_err(tgt - r.a.pitch_phase())) < 0.05f);
}
```

Add `#include "mod/divisions.h"` at the top of the test file if not already there.

- [ ] **Step 2: Run to verify status**

Run: `source env.sh && cmake --build build -j && ./build/spky_tests -tc="center free*"`
Expected: the "below 0.5" case PASSES already (guards against regressions); the "full COUPLE lands on the ladder" case FAILS (locks onto the off-grid geometric mean instead).

- [ ] **Step 3: Implement grid gravity in the free branch**

In `engine/center/center.cpp`, inside the `else` (free-world) branch, after `pull_a`/`pull_b` are computed and before the clamp:

```cpp
        // Grid gravity: above COUPLE 0.5 the pair is additionally pulled onto
        // the nearest ladder division (rate) and the transport grid (phase).
        // Below 0.5 g == 0 exactly — the organic pairwise character is
        // untouched and provably tempo-free. smoothstep avoids a corner at 0.5.
        float g = 0.f;
        if (_couple > 0.5f) {
            const float z = (_couple - 0.5f) * 2.f;
            g = z * z * (3.f - 2.f * z);
        }
        if (g > 0.f) {
            const float geo  = std::sqrt(fa * fb);
            const int   div  = nearest_division(geo, _transport.bpm());
            const float grid = division_hz(div, _transport.bpm());
            const float grid_mult = std::pow(grid / geo, g);   // common-mode rate pull
            conv_a *= grid_mult;
            conv_b *= grid_mult;
            // Common-mode phase gravity. Bank A is the pair's phase reference:
            // at this COUPLE level the pairwise pull already holds A and B
            // together, so steering A steers the pair.
            const double t = _transport.beats() * static_cast<double>(kDivisions[div].cpb);
            const float tgt = static_cast<float>(t - std::floor(t));
            float cme = tgt - a.pitch_phase();
            cme -= std::floor(cme + 0.5f);
            const float cm = g * (hard ? clampf(kKHard * cme, -kLockCap, kLockCap)
                                       : kK * std::sin(TWO_PI * cme));
            pull_a *= (1.f + cm);
            pull_b *= (1.f + cm);
        }
```

(`conv_a`/`conv_b` change from `const float` to `float` for this.)

- [ ] **Step 4: Run the tests**

Run: `source env.sh && cmake --build build -j && ./build/spky_tests`
Expected: full suite PASS. If the full-COUPLE case converges too slowly (flaky near tolerance), extend `run_synced` to 25000 ticks rather than loosening the epsilon.

- [ ] **Step 5: Commit**

```bash
git add engine/center/center.cpp tests/test_center.cpp
git commit -m "center: zoned COUPLE — grid gravity above 0.5, hard rate+downbeat lock at full"
```

---

### Task 6: Panel generator — layout A (TIME group, toggles removed)

**Files:**
- Modify: `host/vcv/res/gen_panel.py`
- Regenerate (committed artifacts): `host/vcv/res/Spotymod.svg`, `host/vcv/src/generated_panel.hpp`

**Interfaces:**
- Produces (consumed by Task 7): `ParamId` without `SYNC_A`/`SYNC_B`, with global `SYNC`; `WK_SW2` replaces `WK_SW3` in `WidgetKind`; `PART_STRIDE` drops 23→22; a `TIME` eyebrow in `kPanelTexts`.

Note: this task intentionally breaks the VCV build until Task 7 lands (generated header changes ahead of the C++). Commit them separately anyway — the generator diff reviews cleanly on its own, and the engine tests (`spky_tests`) stay green throughout.

- [ ] **Step 1: Edit `gen_panel.py`**

1. Replace the `SW3` kind with `SW2` (2-position). In the control-kinds block:
```python
SW2     = "SW2"       # 2-pos switch (global SYNC)
```
(delete the `SW3` line), and update the two dicts:
```python
GLYPH_R = {BIGKNOB:4.2, KNOBC:4.2, SMKNOB:3.0, KNOBI:3.0, SW2:3.0,
           LATCH:2.7, SMBTN:2.7, IN:4.2, OUT:4.2, LIGHT:1.7}
WKMAP = {BIGKNOB:"WK_BIGKNOB", KNOBC:"WK_KNOBC", SMKNOB:"WK_SMKNOB",
         KNOBI:"WK_KNOBI", SW2:"WK_SW2", LATCH:"WK_LATCH", SMBTN:"WK_SMBTN",
         IN:"WK_IN", OUT:"WK_OUT", LIGHT:"WK_LIGHT"}
```
2. In `part_controls()`, remove the SYNC pad and respace the remaining six evenly (same 10.5 pitch, re-centred on the backplate):
```python
    pads = [("ENGINE",LATCH,"ENG"),("GRITMODE",LATCH,"GRIT"),
            ("STEP",LATCH,"STEP"),("PRINCIPLE",SMBTN,"PRIN"),
            ("NEWPHRASE",SMBTN,"NEW"),("TRIGGER",SMBTN,"TRIG")]
    for i,(enum,kind,lbl) in enumerate(pads):
        out.append(Ctl(enum, kind, 15.75 + i*10.5, 102.8, lbl))
```
3. Replace the `SHARED` list head (MORPH through SETTLE) with layout A — a TIME row under MORPH, SCALE/DRIFT and the gesture row shuffling down; ROOM rows unchanged:
```python
SHARED = [
    Ctl("MORPH",  BIGKNOB, CX,  22.0, "MORPH"),
    # TIME: the one clock story — the mode switch, its tempo, and how tightly
    # the two parts hang together (spec 2026-07-16 sync/couple redesign)
    Ctl("SYNC",   SW2,     L,   38.0, "SYNC"),
    Ctl("TEMPO",  SMKNOB,  CX,  38.0, "TEMPO"),
    Ctl("COUPLE", SMKNOB,  R,   38.0, "COUPL"),
    Ctl("SCALE",  KNOBI,   L,   51.0, "SCALE"),
    Ctl("DRIFT",  SMKNOB,  R,   51.0, "DRIFT"),
    Ctl("SPOT",   SMBTN,   L,   62.0, "SPOT"),
    Ctl("MASTER_DRIVE", SMKNOB, CX, 62.0, "DRIVE"),
    Ctl("SETTLE", SMBTN,   R,   62.0, "SETL"),
    # ROOM: unchanged rows; TEMPO has moved out, SMEAR/MOD keep flanking.
    Ctl("REV_SIZE",  SMKNOB, L,  ROW_VOICE, "SIZE"),
    Ctl("REV_DECAY", SMKNOB, R,  ROW_VOICE, "DECAY"),
    Ctl("REV_MIX",   SMKNOB, CX, (ROW_VOICE + ROW_FX) / 2.0, "MIX"),
    Ctl("REV_TONE",  SMKNOB, L,  ROW_FX,    "TONE"),
    Ctl("REV_DIFF",  SMKNOB, R,  ROW_FX,    "DIFF"),
    Ctl("REV_SMEAR", SMKNOB, L,  ROW_PAD, "SMEAR"),
    Ctl("REV_MOD",   SMKNOB, R,  ROW_PAD, "MOD"),
]
```
4. Add the TIME eyebrow to `TEXTS` (after the ROOM entry):
```python
    (CX,            32.2,          2.2, 0.5, MUTED,      "TIME"),
```
and in `svg()`, extend the eyebrow-rule loop to draw TIME rules too — replace the ROOM-only loop with:
```python
    # ROOM + TIME eyebrow rules (text itself comes from TEXTS)
    for ey in (69.2, 31.4):
        for (x0, x1) in ((CX-19.0, CX-8.0), (CX+8.0, CX+19.0)):
            P.append(f'<line x1="{mm(x0)}" y1="{mm(ey)}" x2="{mm(x1)}" y2="{mm(ey)}" '
                     f'stroke="{LINE}" stroke-width="0.25"/>')
```
5. In the SVG glyph loop, replace the `elif c.kind == SW3:` branch with a 2-pos switch drawing:
```python
        elif c.kind == SW2:
            P.append(f'<rect x="{mm(c.x-1.7)}" y="{mm(c.y-3.0)}" width="3.4" '
                     f'height="6.0" rx="0.8" fill="{WHITE}" stroke="{INK}" stroke-width="0.35"/>')
            P.append(f'<rect x="{mm(c.x-1.1)}" y="{mm(c.y-2.4)}" width="2.2" '
                     f'height="2.4" rx="0.5" fill="{GRAPHITE}"/>')
```
6. In `header()`, update the `WidgetKind` enum emission: `WK_SW3` becomes `WK_SW2` (same position in the list).

- [ ] **Step 2: Regenerate and sanity-check**

Run: `cd host/vcv && python res/gen_panel.py`
Expected output: `wrote res/Spotymod.svg and src/generated_panel.hpp` and `params=60 (stride=22) inputs=4 outputs=6 lights=2  panel=42HP` (was stride 23 / params 62: −2 SYNC toggles per part, −0 shared, +1 SYNC).
Then: `grep -n "SYNC\|WK_SW2\|PART_STRIDE" src/generated_panel.hpp` — expect one global `SYNC` enum entry, `WK_SW2` in WidgetKind, `PART_STRIDE = 22`, and NO `SYNC_A`/`SYNC_B`.
Open `res/Spotymod.svg` in a browser and eyeball: TIME eyebrow + switch row under MORPH, six pads per side, ROOM without TEMPO.

- [ ] **Step 3: Confirm the engine tests still pass (VCV header is not part of them)**

Run: `source env.sh && cmake --build build -j && ./build/spky_tests`
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add host/vcv/res/gen_panel.py host/vcv/res/Spotymod.svg host/vcv/src/generated_panel.hpp
git commit -m "vcv(panel): layout A — TIME group (SYNC/TEMPO/COUPL) under MORPH, per-part sync toggles removed"
```

---### Task 7: VCV host wiring — SYNC switch, CLK/RST, RATE tooltip

**Files:**
- Modify: `host/vcv/src/Spotymod.cpp`
- Modify: `host/vcv/plugin.json` (version 2.0.4 → 2.1.0; the ParamId relayout breaks saved patches, so this is a minor bump, not a patch)

**Interfaces:**
- Consumes: `SYNC`, `WK_SW2`, `RESET`, `CLOCK` from `generated_panel.hpp`; `Instrument::set_sync/clock_pulse/reset_transport` (Task 3); `spky::kDivisions/division_index/free_hz` (Task 1).

- [ ] **Step 1: Edit `Spotymod.cpp`**

1. Add near the top (after the includes, before `struct Spotymod`):
```cpp
#include "mod/divisions.h"

// RATE tooltip: the division name while SYNC is on, free Hz otherwise.
struct RateQuantity : ParamQuantity {
    std::string getDisplayValueString() override {
        if (module && module->params[SYNC].getValue() > 0.5f)
            return spky::kDivisions[spky::division_index(getValue())].name;
        return string::f("%.3f Hz", spky::free_hz(getValue()));
    }
};
```
2. In `configControls()`:
   - In the `WK_BIGKNOB`/`WK_SMKNOB` case, special-case RATE:
   ```cpp
                case WK_BIGKNOB:
                case WK_SMKNOB:
                    if (c.id == RATE_A || c.id == RATE_B)
                        configParam<RateQuantity>(c.id, 0.f, 1.f, defaultFor(c.id), lbl);
                    else
                        configParam(c.id, 0.f, 1.f, defaultFor(c.id), lbl);
                    break;
   ```
   - Replace the `case WK_SW3:` block with:
   ```cpp
                case WK_SW2:  // init patch runs the instrument on the grid
                    configSwitch(c.id, 0.f, 1.f, 1.f, "Sync", {"Free", "Synced"});
                    break;
   ```
3. In the `Spotymod` struct, add a reset trigger next to `clockTrig`:
```cpp
    dsp::SchmittTrigger clockTrig, resetTrig;
```
(remove the old standalone `clockTrig` declaration).
4. In `pushParams()`:
   - Delete the per-part sync push (the `int sm = ...; inst.set_sync_mode(...)` block).
   - After `inst.set_drift(...)`, add:
   ```cpp
        inst.set_sync(params[SYNC].getValue() > 0.5f);
   ```
5. In `process()`:
   - Extend the clock-edge branch to also align the engine transport:
   ```cpp
        if (inputs[CLOCK].isConnected()) {
            clkSamples += 1.f;
            if (clockTrig.process(inputs[CLOCK].getVoltage(), 0.1f, 1.f)) {
                clkSamples = 0.f;
                inst.clock_pulse();
            }
        }
   ```
   - Below it, give RST its job:
   ```cpp
        if (inputs[RESET].isConnected() &&
            resetTrig.process(inputs[RESET].getVoltage(), 0.1f, 1.f))
            inst.reset_transport();
   ```
6. In `SpotymodWidget`, replace the `case WK_SW3:` / `CKSSThree` branch with:
```cpp
                case WK_SW2:
                    addParam(createParamCentered<CKSS>(pos, module, c.id)); break;
```
7. In `PanelText::glyphR`, replace `case WK_SW3: return 2.2f;` with `case WK_SW2: return 3.0f;`.

`host/vcv/plugin.json`: `"version": "2.1.0",`

- [ ] **Step 2: Build the plugin**

Run: `cd host/vcv && make -j4`
Expected: links `plugin.dll` with no warnings about unused SYNC enums. If `RateQuantity` fails to see `SYNC` (declared later in the generated header include order), the include of `generated_panel.hpp` is already above — it works; a failure here means Task 6 wasn't regenerated.

- [ ] **Step 3: Manual smoke in Rack**

Run: `cd host/vcv && make install`, start VCV Rack (Spotymod is installed to the user plugin dir).
Check, in order:
1. Fresh Spotymod: SYNC switch up (Synced), RATE tooltips show division names ("1/4" etc.), audio groovt like the init patch.
2. Flip SYNC to Free: RATE tooltips switch to Hz; COUPLE full → the two rings pull together (yesterday's hard lock) and settle onto a steady shared rate.
3. SYNC on, COUPLE full → rings tick in lockstep; COUPLE at 0 with DRIFT full → ring textures visibly breathe apart while the gates stay on the grid.
4. Patch an LFO square into CLK → tempo follows; pulse into RST → phrase downbeat realigns.

- [ ] **Step 4: Commit**

```bash
git add host/vcv/src/Spotymod.cpp host/vcv/plugin.json
git commit -m "vcv: global SYNC switch, CLK phase-align + live RST, division-aware RATE tooltip"
```

---

### Task 8: Full verification + listening pass

**Files:**
- No source changes expected (fixes only if verification finds regressions).

- [ ] **Step 1: Full engine suite**

Run: `source env.sh && cmake --build build -j && ./build/spky_tests`
Expected: every suite PASS, zero skipped (any `doctest::skip()` left from Task 3 must be gone by now — grep to confirm: `grep -rn "doctest::skip" tests/` returns nothing).

- [ ] **Step 2: Render both worlds and listen**

```bash
cmake --build build -j --target render
./build/render host/render/scenarios/ambient_wash.json build/sync_redesign_wash.wav
./build/render host/render/scenarios/demo_step_melody.json build/sync_redesign_steps.wav
```
Expected: renders complete without NaN warnings. Listen to both WAVs: the wash should hold its character (global sync on now); the step melody must sit rigidly on the grid.

- [ ] **Step 3: VCV play test**

Repeat Task 7 Step 3's four checks after a Rack restart (catches param-id persistence issues with the new layout). Saved 2.0.x patches will load with shuffled params — expected and accepted (version bump 2.1.0); note it for the release notes.

- [ ] **Step 4: Update the roadmap**

Add a line to `docs/roadmap.md` under the current milestone marking the sync/couple redesign as landed, referencing the spec path.

- [ ] **Step 5: Commit**

```bash
git add docs/roadmap.md
git commit -m "docs: mark the SYNC/COUPLE redesign landed"
```

---

## Self-Review Notes

- **Spec coverage:** interaction model → Tasks 4+5+7; transport/CLK/RST → Tasks 2+3+7; rate-scale split → Task 3; ladder → Task 1; panel layout A + init defaults → Tasks 6+7; verification → per-task tests + Task 8. Ladder ordering deviation is flagged in Global Constraints and fixed into the spec in Task 1.
- **Type consistency:** `set_rate_scale(float, float)` (Task 3) is what Tasks 4–5 call; `division()`/`kDivisions[].cpb` names match between divisions.h, center.cpp, and the tooltip; `WK_SW2`/`SYNC` names match Task 6's generator output and Task 7's C++.
- **Known cost:** VCV param relayout breaks saved 2.0.x patches (version bump to 2.1.0, noted in Task 8). The VCV tree does not build between Tasks 6 and 7 (generated header ahead of C++); engine tests stay green throughout.

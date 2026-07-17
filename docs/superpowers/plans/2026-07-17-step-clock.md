# Step-Clock Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** RATE wird im STEP-Modus zur Step-Clock mit 8-Step-Referenz (`zyklus_hz = rate_hz × 8 / steps`), damit die Step-Dauer unabhängig von der Step-Zahl ist und die Decks polymetrisch laufen können.

**Architecture:** Die gesamte Änderung lebt in `ModLane` (`engine/mod/lane.{h,cpp}`): die Lane speichert die rohe Rate und rechnet `_phase_inc` bei Rate-, Modus- und Step-Änderungen neu; `set_step()` reskaliert beim Live-Drehen die Phase, damit Step-Index und Timing nahtlos weiterlaufen. `SuperModulator`, `Part`, `Instrument` und der VCV-Host bleiben unangetastet. Spec: `docs/superpowers/specs/2026-07-17-step-clock-design.md`.

**Tech Stack:** C++17, doctest, CMake+Ninja (clang), Build-Env via `source env.sh` (siehe unten).

## Global Constraints

- **Arbeitsverzeichnis:** `C:\Users\bernd\Documents\AI\Spotykach` (Git-Bash-Pfad `/c/Users/bernd/Documents/AI/Spotykach`). Direkt auf `main` arbeiten, kein Worktree.
- **Build (Bash, nicht PowerShell):** `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake -B build && cmake --build build`
- **Tests:** `./build/spky_tests` (alle) bzw. `./build/spky_tests -tc="<pattern>"` (Filter, doctest).
- **Bit-Identität am Default:** Bei 8 Steps muss der Faktor exakt `1.0f` sein (`8.f/8 == 1.f`) — keine zusätzliche Rundung einführen (Multiplikation mit dem Faktor, keine Division durch `steps/8.f`).
- **Kein Panel-/Host-Impact:** Nichts unter `host/` oder an `SuperModulator`/`Part`/`Instrument` ändern.
- **Commit-Trailer:** `Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>` (nicht der Standard-Claude-Trailer).
- **Grün auf main:** Der einzige Commit fällt am Ende von Task 2, wenn die volle Suite grün ist. Task 1 committet bewusst NICHT (die Alt-Tests sind zwischen Task 1 und 2 rot).

---

### Task 1: Step-Clock in ModLane (TDD, ohne Commit)

**Files:**
- Create: `tests/test_step_clock.cpp`
- Modify: `CMakeLists.txt:48` (Testdatei registrieren)
- Modify: `engine/mod/lane.h:50,66-68` (Member `_rate_hz`, Methode `_update_inc`)
- Modify: `engine/mod/lane.cpp:48,53-63` (`set_rate_hz`, `set_step`, `_update_inc`)

**Interfaces:**
- Consumes: bestehende `ModLane`-API (`set_rate_hz(float)`, `set_step(bool, int)`, `process()`, `fired()`, `phase()`).
- Produces: identische Signaturen, neue Semantik — im STEP-Modus gilt `phase_inc = (rate_hz/sr) × 8/steps`; `set_step()` reskaliert bei aktivem STEP-Modus und geänderter Step-Zahl die Phase positionserhaltend (`pos = fmod(phase×alt, neu)`, `_cur_step = int(pos)`), aber nur wenn `_cur_step >= 0` (schon gelaufen). Task 2 verlässt sich exakt darauf.

- [ ] **Step 1: Failing Tests schreiben**

`tests/test_step_clock.cpp` neu anlegen (kompletter Inhalt):

```cpp
#include <doctest/doctest.h>
#include <cmath>
#include <vector>
#include "mod/lane.h"
using namespace spky;

// Spec 2026-07-17 step-clock: in STEP mode RATE is a step clock with an
// 8-step reference — cycle_hz = rate_hz * 8 / steps. Step duration depends
// only on RATE, never on the step count; 8 steps is bit-identical to the
// old pattern-clock behavior.

static ModLane step_lane(int steps, float hz) {
    ModLane l;
    l.init(48000.f, 7);
    l.set_range(1.f); l.set_shape(0.5f); l.set_smooth(0.f);
    l.set_step(true, steps);
    l.set_rate_hz(hz);
    return l;
}

static std::vector<int> fire_samples(ModLane& l, int samples) {
    std::vector<int> out;
    for (int i = 0; i < samples; ++i) { l.process(); if (l.fired()) out.push_back(i); }
    return out;
}

TEST_CASE("step-clock: 8 steps is the reference — step = 6000 samples at 1 Hz") {
    ModLane l = step_lane(8, 1.f);
    // boundaries every 6000 samples: 8 fires in [0, 47000)
    CHECK(fire_samples(l, 47000).size() == 8);
}

TEST_CASE("step-clock: step duration is independent of the step count") {
    // same RATE => same fires-per-second, whatever STEPS says
    for (int steps : {2, 8, 14, 16}) {
        ModLane l = step_lane(steps, 1.f);
        CHECK(fire_samples(l, 47000).size() == 8);   // one fire per 6000 samples
    }
}

TEST_CASE("step-clock: 8 vs 14 steps stay boundary-aligned (polymeter)") {
    ModLane a = step_lane(8, 1.f);
    ModLane b = step_lane(14, 1.f);
    auto fa = fire_samples(a, 47000);
    auto fb = fire_samples(b, 47000);
    REQUIRE(fa.size() == fb.size());
    for (size_t i = 0; i < fa.size(); ++i)
        CHECK(std::abs(fa[i] - fb[i]) <= 256);   // float-phasor drift only
}

TEST_CASE("step-clock: live STEPS grow 8->16 keeps position and timing") {
    ModLane l = step_lane(8, 1.f);
    for (int i = 0; i < 3000; ++i) l.process();   // mid step 0
    float pos = l.phase() * 8.f;                  // step index + fraction
    l.set_step(true, 16);
    CHECK(l.phase() == doctest::Approx(pos / 16.f).epsilon(0.001));  // rescaled, not jumped
    l.process();
    CHECK_FALSE(l.fired());                       // no ghost boundary on the switch
    int to_fire = 1;
    while (to_fire < 20000) { l.process(); ++to_fire; if (l.fired()) break; }
    CHECK(to_fire > 2900); CHECK(to_fire < 3100); // next boundary still ~sample 6000
    CHECK(static_cast<int>(l.phase() * 16.f) == 1);   // ...and it is step 1
}

TEST_CASE("step-clock: live STEPS shrink 16->8 wraps the index, keeps the grid") {
    ModLane l = step_lane(16, 1.f);
    for (int i = 0; i < 61000; ++i) l.process();  // step 10 of 16 (step = 6000 samples)
    float pos = std::fmod(l.phase() * 16.f, 8.f); // 10.x -> 2.x
    l.set_step(true, 8);
    CHECK(l.phase() == doctest::Approx(pos / 8.f).epsilon(0.001));
    l.process();
    CHECK_FALSE(l.fired());
    int to_fire = 1;
    while (to_fire < 20000) { l.process(); ++to_fire; if (l.fired()) break; }
    // grid unbroken: next fire lands where step 11 of 16 would have (~5000 on)
    CHECK(to_fire > 4700); CHECK(to_fire < 5300);
    CHECK(static_cast<int>(l.phase() * 8.f) == 3);
}

TEST_CASE("step-clock: FLOW rate is untouched by the step count") {
    ModLane a; a.init(48000.f, 7); a.set_rate_hz(2.f); a.set_step(false, 3);
    ModLane b; b.init(48000.f, 7); b.set_rate_hz(2.f); b.set_step(false, 16);
    int fa = 0, fb = 0;
    for (int i = 0; i < 240000; ++i) {            // 5 s
        a.process(); if (a.fired()) ++fa;
        b.process(); if (b.fired()) ++fb;
    }
    CHECK(fa == fb);                              // same seed, same inc: exact
    CHECK(fa >= 9); CHECK(fa <= 11);              // ~2 wraps/s
}
```

In `CMakeLists.txt` direkt nach `tests/test_step.cpp` (Zeile 48) registrieren:

```cmake
    tests/test_step.cpp
    tests/test_step_clock.cpp
```

- [ ] **Step 2: Tests laufen lassen — müssen fehlschlagen**

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake -B build && cmake --build build && ./build/spky_tests -tc="step-clock*"`
Expected: FAIL — u.a. „step duration is independent" (14/16 Steps feuern öfter als 8×) und beide Live-STEPS-Tests (Phase springt statt reskaliert).

- [ ] **Step 3: ModLane umbauen**

`engine/mod/lane.h` — Member ergänzen (bei `_phase_inc`, Zeile 68):

```cpp
    float _sr = 48000.f;
    float _phase = 0.f;
    float _phase_inc = 0.f;
    float _rate_hz = 0.f;
```

und die private Methode deklarieren (neben `_update_slew()`, Zeile 50):

```cpp
    void  _update_slew();
    void  _update_inc();            // step-clock: inc = rate/sr * (STEP ? 8/steps : 1)
```

`engine/mod/lane.cpp` — `set_rate_hz` (Zeile 48) ersetzen:

```cpp
void ModLane::set_rate_hz(float hz)   { _rate_hz = hz > 0.f ? hz : 0.f; _update_inc(); }
```

`set_step` (Zeile 53-63) ersetzen:

```cpp
void ModLane::set_step(bool on, int steps) {
    if (on && !_step_mode) { _note_age = 0; _note_hold = 0; }  // STEP entry: no stale sustain
    int new_steps = steps < 1 ? 1 : steps;
    if (_melodic) {
        int old_n = _steps > kSeqSlots ? kSeqSlots : _steps;
        int new_n = new_steps > kSeqSlots ? kSeqSlots : new_steps;
        if (new_n != old_n) _regen_pending = true; // only when effective length changes
    }
    if (on && _step_mode && new_steps != _steps && _cur_step >= 0) {
        // Seamless live STEPS turn (spec: step-clock): keep the step index and
        // the fraction inside it so the boundary grid never jumps; _cur_step
        // follows along so the next sample sees no ghost boundary. The
        // _cur_step >= 0 guard keeps pre-run configuration (init -> set_step
        // before the first process()) on the old path, where the first sample
        // must still fire step 0.
        float pos = std::fmod(_phase * static_cast<float>(_steps),
                              static_cast<float>(new_steps));
        _phase = pos / static_cast<float>(new_steps);
        _cur_step = static_cast<int>(pos);
    }
    _step_mode = on;
    _steps = new_steps;
    _update_inc();
}
```

`_update_inc` neu, direkt unter `set_step` einfügen:

```cpp
// Spec 2026-07-17 step-clock: STEP runs RATE as a step clock with an 8-step
// reference; FLOW keeps RATE as the cycle rate. At 8 steps the factor is
// exactly 1.0f, so the panel default stays bit-identical to the old
// pattern-clock behavior.
void ModLane::_update_inc() {
    const float f = _step_mode ? 8.f / static_cast<float>(_steps) : 1.f;
    _phase_inc = (_rate_hz / _sr) * f;
}
```

- [ ] **Step 4: Neue Tests laufen lassen — müssen bestehen**

Run: `cmake --build build && ./build/spky_tests -tc="step-clock*"`
Expected: PASS (7 Test Cases).

- [ ] **Step 5: Vollsuite laufen lassen, erwartete Alt-Fehler dokumentieren**

Run: `./build/spky_tests`
Expected: FAIL — genau in diesen Dateien (alte Timing-Annahmen, Task 2 passt sie an): `test_step.cpp` („fires once per step", „target held constant"), `test_gate_density.cpp` (DENSE-/Gate-/Tail-Fälle), `test_part.cpp` („gate sustains the composed STEP note"). Andere Fehlschläge wären ein echtes Problem → dann NICHT weiter, sondern die Implementierung prüfen. **Kein Commit in diesem Task** (main bliebe rot).

### Task 2: Alt-Tests auf Step-Clock-Semantik heben + Commit

**Files:**
- Modify: `tests/test_step.cpp:5-47`
- Modify: `tests/test_lane.cpp:34-53` (Timing + Kommentare)
- Modify: `tests/test_gate_density.cpp:15,19-20,30-57,71-84,95-111,113-126`
- Modify: `tests/test_part.cpp:278-303` (Kommentar + `step_samples`)

**Interfaces:**
- Consumes: Task-1-Semantik — im STEP-Modus gilt `step_dauer = sr/(8×rate_hz)` Samples, `zyklus = steps × step_dauer`; bei 8 Steps alles wie vorher.
- Produces: grüne Vollsuite; ein Commit mit allen Änderungen aus Task 1+2.

Umrechnung überall gleich: Lane bei `rate_hz` → Step-Dauer = `48000/(8×rate_hz)` Samples, unabhängig von der Step-Zahl. Beispiele: rate 1 → 6000, rate 2 → 3000, rate 8 → 750.

- [ ] **Step 1: `tests/test_step.cpp` anpassen**

Die drei Cases so ersetzen (Datei-Kopf mit Includes bleibt):

```cpp
TEST_CASE("lane STEP: fires once per step") {
    ModLane l;
    l.init(48000.f, 7);
    l.set_range(1.f); l.set_shape(0.5f); l.set_smooth(0.f);
    l.set_step(true, 4);
    l.set_rate_hz(1.f);       // step-clock: step = 6000 samples, cycle = 24000
    int fires = 0;
    // Count over LESS than one cycle (23000 < 24000) so the free-running float
    // phasor does not wrap and re-enter step 0 with a spurious 5th fire.
    for (int i = 0; i < 23000; ++i) { l.process(); if (l.fired()) ++fires; }
    CHECK(fires == 4);
}

TEST_CASE("lane STEP: target held constant within a step") {
    ModLane l;
    l.init(48000.f, 7);
    l.set_range(1.f); l.set_shape(0.5f); l.set_smooth(0.f);
    l.set_step(true, 4);      // step-clock: step 0 spans samples [0, 6000)
    l.set_rate_hz(1.f);
    for (int i = 0; i < 1500; ++i) l.process();
    float a = l.target();
    for (int i = 0; i < 3000; ++i) l.process();   // still step 0 (~sample 4500)
    float b = l.target();
    CHECK(a == doctest::Approx(b));
}

TEST_CASE("lane STEP: fixed slew ignores the SMOOTH knob") {
    // Panel switch 3 middle = STEP + fixed slew: the glide time must be constant
    // regardless of SMOOTH. Sample the output a fixed offset past a step boundary
    // for two very different SMOOTH settings; with fixed slew they must match.
    auto glide_after_boundary = [](float smooth) {
        ModLane l;
        l.init(48000.f, 7);
        l.set_range(1.f); l.set_shape(0.5f);
        l.set_step(true, 2);
        l.set_fixed_slew(true);        // engage fixed slew BEFORE SMOOTH
        l.set_smooth(smooth);          // must be ignored while fixed slew is on
        l.set_rate_hz(1.f);            // step-clock: step = 6000 samples; boundary at ~6000
        for (int i = 0; i < 6100; ++i) l.process();
        return l.process();            // ~100 samples past the step-1 boundary
    };
    CHECK(glide_after_boundary(0.0f) == doctest::Approx(glide_after_boundary(1.0f)).epsilon(0.001));
}
```

- [ ] **Step 2: `tests/test_lane.cpp` — SMOOTH-Glide-Case anpassen**

Case „lane: SMOOTH turns a step into a glide" (Zeile 34-53) ersetzen:

```cpp
TEST_CASE("lane: SMOOTH turns a step into a glide") {
    ModLane l;
    l.init(48000.f, 55);
    l.set_range(1.f);
    l.set_shape(0.5f);        // ramp: consecutive step values differ
    l.set_step(true, 2);      // step-clock: step = 6000 samples; boundary at ~6000
    l.set_smooth(0.5f);       // glide ~3 ms: settles well within a step, still gliding ~1 ms past a boundary
    l.set_rate_hz(1.f);       // cycle_hz = 4 -> 12000 samples/cycle

    for (int i = 0; i < 5000; ++i) l.process();    // settle in step 0
    float settled0 = l.process();
    float target0  = l.target();
    for (int i = 5002; i < 6050; ++i) l.process(); // cross into step 1
    float out_after = l.process();                 // ~1 ms past boundary
    float target1   = l.target();

    CHECK(target1 != doctest::Approx(target0));        // new value latched
    CHECK(std::fabs(out_after - target1) > 0.01f);     // output still gliding
    CHECK(std::fabs(settled0  - target0) < 0.01f);     // was settled before
}
```

- [ ] **Step 3: `tests/test_gate_density.cpp` anpassen**

Rate 2 Hz ⇒ Step = 3000 Samples; Zyklus bei 16 Steps = 48000, bei 20 Steps = 60000. Gezielte Ersetzungen:

Helper-Kommentare (Zeile 15 und 19-20):

```cpp
    l.set_rate_hz(2.0f);             // step-clock: step = 3000 samples
```

```cpp
// Which step indices fire over one full cycle. At rate 2 Hz a step is 3000
// samples (step-clock); l.phase() just after a fire identifies the entered step.
```

„DENSE is monotonic" (Zeile 36): Fenster `24000` → `47000` (16 Steps × 3000 = 48000; knapp darunter bleiben, damit der Phasor nicht wrappt):

```cpp
        auto s = fired_step_set(l, 16, 47000);
```

„DENSE 0 leaves exactly the cell anchors" (Zeile 47):

```cpp
    auto s = fired_step_set(l, 16, 47000);
```

„DENSE is reversible" (Zeile 56):

```cpp
    CHECK(fired_step_set(a, 16, 47000) == fired_step_set(b, 16, 47000));
```

„gate releases before the next note when the gap is long" (Zeile 74):

```cpp
    const int step_samples = 3000;         // step-clock: 48000/(8*2 Hz)
```

„gate can sustain across a frozen (rest) step" (Zeile 104): Fenster auf einen vollen 16-Step-Zyklus heben, damit beide Zellen (Rest-Slot kann in der zweiten liegen) beobachtet werden:

```cpp
        for (int n = 0; n < 48000; ++n) {
```

Tail-Case (Zeile 113-126): 20 Steps ⇒ Zyklus 60000; erst über den ersten Wrap (Regen) laufen, dann knapp unter einem Zyklus sampeln:

```cpp
// steps=20 forces a tail: pg_derive_sizing(TwoMotif, 20) -> k=3, L=6, r=2
// (3*6+2==20). At density 0 only the cell anchor (rank 0, always cell-relative
// slot 0) fires: once per full instance (0, 6, 12) plus the tail's own slot 0,
// which lands at absolute slot 18 (18 % L(=6) == 0). set_step() only flags a
// regen (it lands at the next STEP-mode wrap), so the lane still runs its
// stale init()-time layout (steps=8) for the first cycle; with the step-clock
// a 20-step cycle is 60000 samples, so run past the first wrap before sampling.
TEST_CASE("DENSE 0 truncates cleanly over a tail: anchors plus the tail's own anchor") {
    ModLane l = melodic_step(0x11, 20);
    l.set_density(0.f);
    for (int n = 0; n < 61000; ++n) l.process();       // let the steps=20 regen land
    auto s = fired_step_set(l, 20, 59000);
    CHECK(s == std::set<int>{0, 6, 12, 18});
}
```

Alle übrigen Cases der Datei („FLOW never freezes", „adjacent notes tie", „STEP re-entry") bleiben unverändert.

- [ ] **Step 4: `tests/test_part.cpp` — Gate-Sustain-Case anpassen**

Kommentarblock und `step_samples` (Zeile 278-292) ersetzen:

```cpp
// Composed-note sustain routed to the GATE output (rhythm-groove-design.md
// section 3). SYNCED + tempo 60 + rate norm 0.5 lands on division index 8
// ("1/4", cpb 1) -> base_hz 1 Hz -> PITCH lane rate = base_hz * kLaneRatio
// (2.0) = 2 Hz; with the step-clock a step is 48000/(8*2) = 3000 samples
// regardless of the 16-step count. Mirrors the ModLane-level timing in "gate
// releases before the next note when the gap is long"
// (tests/test_gate_density.cpp), but observed through Part.
TEST_CASE("part: gate sustains the composed STEP note, releasing before the next downbeat") {
    Part p;
    p.init(48000.f, 5);
    p.set_step(true, 16);
    p.mod().set_density(0.f);      // anchor-only: notes at steps 0 and 8 (L=8)
    p.mod().set_synced(true);
    p.mod().set_tempo_bpm(60.f);
    p.mod().set_rate(0.5f);        // -> PITCH lane 2 Hz: 3000 samples/step
    const int step_samples = 3000;
```

Der Rest des Cases (ab `std::vector<char> gate;`) bleibt unverändert.

- [ ] **Step 5: Vollsuite laufen lassen — muss grün sein**

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build && ./build/spky_tests`
Expected: alle Tests PASS, keine Failures. Falls ein hier nicht genannter Test rot ist: stoppen und analysieren, nicht blind fixen.

- [ ] **Step 6: Commit (Task 1 + Task 2 zusammen)**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
git add engine/mod/lane.h engine/mod/lane.cpp CMakeLists.txt \
        tests/test_step_clock.cpp tests/test_step.cpp tests/test_lane.cpp \
        tests/test_gate_density.cpp tests/test_part.cpp
git commit -m "feat(engine): step-clock — STEPS von der Geschwindigkeit entkoppelt

RATE laeuft im STEP-Modus als Step-Clock mit 8-Step-Referenz
(zyklus = rate x 8/steps): Step-Dauer unabhaengig von der Step-Zahl,
Decks koennen polymetrisch laufen (8 vs 14 Steps, gleiche Rate).
Live-Drehen am STEPS-Knopf reskaliert die Phase nahtlos. Bei 8 Steps
bit-identisch zum alten Verhalten; FLOW unberuehrt.

Spec: docs/superpowers/specs/2026-07-17-step-clock-design.md

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

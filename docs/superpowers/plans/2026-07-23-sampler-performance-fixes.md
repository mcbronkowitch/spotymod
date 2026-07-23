# Sampler Performance Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Drei Befunde aus dem Spielen abstellen — der STEP-Einstieg schnappt auf die aktuelle Taktposition, die SCAN-Kurve wird unterhalb des Knies linear (Maximum 4x statt 8x), und MOD wirkt auf die Leseposition quadratisch statt linear (Spec `docs/superpowers/specs/2026-07-23-sampler-performance-fixes-design.md`).

**Architecture:** Der Snap ist die Umkehrung von `Center::_rebase_grid`: statt das Grid-Ziel auf die Phase zu schieben, wird die Phase auf das Ziel gesetzt, wodurch der Servofehler ab dem ersten Sample 0 ist. `Part::set_step` erkennt die Flanke und setzt ein Flag; `Center::update` — das ohnehin beide Modulatoren und beide Parts bekommt — konsumiert es nach den `_rebase_grid`-Aufrufen. SCAN und MOD sind reine Kennlinienänderungen ohne neue Struktur.

**Tech Stack:** C++17, kein Heap in `engine/`, doctest, CMake + Ninja + clang (Desktop), deterministischer Rng mit dokumentierter Zugreihenfolge.

## Global Constraints

- **Build/Test-Schleife (immer so, nie MSVC):**
  `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build --target spky_tests && ./build/spky_tests -tc="<filter>"`
  Volle Suite: `./build/spky_tests` (Exit 0 = grün). Ausgangsstand: **586 Testfälle grün**.
- **Der VCV-Host wird NUR über `cd host/vcv && ./build-local.sh` gebaut** — der System-`g++` ist der ARM-Cross-Compiler. Dieser Plan braucht ihn nicht.
- **Kein Heap, kein libDaisy in `engine/`.** Nur Arrays fester Größe.
- **Beide Golden Vectors müssen bit-identisch bleiben.** Sie treiben `SamplerEngine` direkt ohne `Part` und ohne `Center` (`tests/test_sampler_engine.cpp:860` FLOW, `:2860` STEP); weder `target_raw` noch der Snap noch `set_scan` laufen dort. Bewegt sich einer: **STOP und melden**, nicht neu aufnehmen.
- **Der Draw-Contract ändert sich nicht.** Keine Task fügt einen Rng-Zug hinzu, entfernt einen oder verschiebt einen.
- **Kein `std::pow`/`exp2f` pro Sample.** Trigger- und Control-Rate sind in Ordnung, der Audio-Pfad nicht.
- **Einen Test zu löschen ist eine gemeldete Entscheidung, keine stille Aufräumaktion.** Dieser Plan löscht keinen.
- **Bestehende Zusicherungen werden nicht gelockert.** Wird ein Test rot, ist das ein Befund und wird gemeldet, nicht umgebogen.
- Neue Konstanten nach `engine/sampler/sampler_config.h`, mit Kommentar der sagt, ob ear-tunable oder Vertrag — im Stil der Datei.
- **Commit-Trailer (jeder Commit):** `Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>` — nie der Default-Claude/Anthropic-Trailer.

---

### Task 1: Der Schritt-Helfer und der pitch-only Snap-Hook

**Files:**
- Modify: `engine/mod/lane.h` (Deklaration neben `cur_step()`, `:57`)
- Modify: `engine/mod/lane.cpp` (Nutzung in `process()`, `:288-290`)
- Modify: `engine/mod/super_modulator.h` (neue Methode neben `reset_phases()`, `:71`)
- Test: `tests/test_lane.cpp`

**Interfaces:**
- Consumes: `ModLane::reset(float)` (`lane.h:76`), die Member `_since_onset`, `_onsets`, `_gap[2]`, `_rhythm` (`super_modulator.h:122-123`).
- Produces (Task 4 nutzt genau das): `static int ModLane::step_index(float phase, int steps)`; `void SuperModulator::snap_pitch_phase(float ph)`.

**Warum ein Helfer und nicht zwei Kopien:** Task 4 muss den Slot aus einer Phase berechnen, die die Lane noch nicht gesehen hat. Die Rundungsregel dafür steht heute in `process()`. Zwei Kopien derselben Regel divergieren später still — deshalb eine Funktion, die beide rufen.

- [ ] **Step 1: Den fehlschlagenden Test schreiben**

Ans Ende von `tests/test_lane.cpp` anhängen:

```cpp
// --- step_index: die Rundungsregel, die process() und der Snap teilen -------
//
// Der Snap (spec 2026-07-23 sampler-performance-fixes) muss den Slot aus einer
// Phase berechnen, BEVOR die Lane sie verarbeitet hat -- zurueckgelesen waere
// cur_step() noch -1, weil ModLane::reset() es genau darauf setzt. Diese
// Funktion ist die eine Stelle, an der die Regel steht.
TEST_CASE("lane: step_index folds a phase onto its slot") {
    CHECK(ModLane::step_index(0.f,     8) == 0);
    CHECK(ModLane::step_index(0.124f,  8) == 0);
    CHECK(ModLane::step_index(0.125f,  8) == 1);
    CHECK(ModLane::step_index(0.5f,    8) == 4);
    CHECK(ModLane::step_index(0.999f,  8) == 7);

    // Die obere Klemme ist der Grund, warum das eine Funktion ist und kein
    // int-Cast: ohne sie liefert eine Phase von exakt 1.0 den Slot 8 und
    // greift einen Schritt hinter das Ende der Phrase.
    CHECK(ModLane::step_index(1.f,     8) == 7);
    CHECK(ModLane::step_index(1.5f,    8) == 7);

    // Ein einzelner Schritt hat nur den Slot 0, bei jeder Phase.
    CHECK(ModLane::step_index(0.f,     1) == 0);
    CHECK(ModLane::step_index(0.99f,   1) == 0);
}

// snap_pitch_phase setzt die PITCH-Lane und NUR sie -- die vier Texturlanes
// laufen weiter, sonst waere es die RST-Geste (reset_phases) unter anderem
// Namen. Der Onset-Gap-Ring wird mitgenullt: nach einem Phasensprung waere
// der naechste gemessene Abstand einer, den es nie gab, und dieser
// Rhythmus-Blick steuert die FX-Abgriffe des ANDEREN Decks.
TEST_CASE("mod: snap_pitch_phase moves the pitch lane alone") {
    SuperModulator m;
    m.init(48000.f, 7u);
    for (int i = 0; i < 5000; ++i) m.process();

    const float tex_before = m.lane_phase(LANE_MOTION);
    REQUIRE(m.pitch_phase() != doctest::Approx(0.25f).epsilon(1e-3));

    m.snap_pitch_phase(0.25f);

    CHECK(m.pitch_phase() == doctest::Approx(0.25f).epsilon(1e-6));
    CHECK(m.lane_phase(LANE_MOTION) == doctest::Approx(tex_before).epsilon(1e-6));
    CHECK(m.rhythm().gap_a == 0);
    CHECK(m.rhythm().gap_b == 0);
}
```

Lies vor dem Schreiben die Rig-Idiome am Anfang von `tests/test_lane.cpp` und folge ihnen. Prüfe die Feldnamen von `RhythmView` in `engine/mod/rhythm_view.h` und nimm die dort vorhandenen — heißen sie anders als `gap_a`/`gap_b`, gilt die Datei, nicht dieser Plan.

- [ ] **Step 2: Test laufen lassen, Fehlschlag bestätigen**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build --target spky_tests && ./build/spky_tests -tc="lane: step_index*,mod: snap_pitch_phase*"
```

Erwartet: Compile-Fehler — `step_index` und `snap_pitch_phase` existieren nicht.

- [ ] **Step 3: `step_index` anlegen und in `process()` benutzen**

In `engine/mod/lane.h`, direkt hinter `int cur_step() const { return _cur_step; }` (`:57`):

```cpp
    // Die Phase-auf-Slot-Regel, an EINER Stelle. process() rechnet damit, und
    // der STEP-Einstiegs-Snap (spec 2026-07-23 sampler-performance-fixes)
    // braucht denselben Slot fuer eine Phase, die diese Lane noch nicht
    // gesehen hat -- zurueckgelesen waere _cur_step dort noch -1, weil
    // reset() es genau darauf setzt. Zwei Kopien dieser Rundung wuerden
    // spaeter still auseinanderlaufen.
    static int step_index(float phase, int steps) {
        int s = static_cast<int>(phase * static_cast<float>(steps));
        if (s >= steps) s = steps - 1;
        if (s < 0)      s = 0;
        return s;
    }
```

In `engine/mod/lane.cpp`, `process()`, ersetze (`:288-290`)

```cpp
        int step = static_cast<int>(_phase * _steps);
        if (step >= _steps) step = _steps - 1;
```

durch

```cpp
        const int step = step_index(_phase, _steps);
```

Prüfe, ob `tick()` (`lane.cpp:344-348`) dieselbe Rundung noch einmal enthält. Wenn ja, geht sie ebenfalls auf `step_index` — das ist der Zweck der Übung. Wenn sie dort anders aussieht, **nicht angleichen**, sondern im Bericht melden: dann ist der Unterschied entweder Absicht oder ein eigener Befund.

- [ ] **Step 4: `snap_pitch_phase` anlegen**

In `engine/mod/super_modulator.h`, direkt hinter `reset_phases()` (`:71-77`):

```cpp
    // STEP-Einstiegs-Snap (spec 2026-07-23 sampler-performance-fixes): setzt
    // NUR die PITCH-Lane auf eine gegebene Phase. Bewusst nicht reset_phases:
    // das ist die RST-Geste und setzt alle fuenf Lanes -- ein Sprung in den
    // Texturlanes waere ein hoerbarer Ruck in Filter und Pan, der dem
    // Rhythmus nicht hilft.
    //
    // Der Onset-Gap-Ring wird mitgenullt, dieselbe Kopplung, auf der
    // reset_phases oben besteht: nach einem Phasensprung misst der erste
    // Onset sonst einen Abstand, den es nie gab, und dieser Rhythmus-Blick
    // steuert ueber Instrument die FX-Abgriffe des ANDEREN Decks -- eines
    // Decks, das von diesem Snap nichts merken soll.
    void snap_pitch_phase(float ph) {
        _lanes[LANE_PITCH].reset(ph);
        _since_onset = 0;
        _onsets = 0;
        _gap[0] = _gap[1] = 0;
        _rhythm = RhythmView{};
    }
```

- [ ] **Step 5: Tests laufen lassen, dann die volle Suite**

```bash
./build/spky_tests -tc="lane: step_index*,mod: snap_pitch_phase*"
./build/spky_tests
```

Erwartet: beide neu grün, Suite grün bei 588. Beide Golden Vectors unberührt.

- [ ] **Step 6: Beweisen, dass die Tests tragen (Mutationsprobe)**

Nimm in `step_index` die obere Klemme heraus (`if (s >= steps) s = steps - 1;` löschen), baue neu, lass `"lane: step_index*"` laufen. Erwartet: der Fall mit Phase 1.0 schlägt fehl. Zurücknehmen. Dann ersetze in `snap_pitch_phase` den Aufruf `_lanes[LANE_PITCH].reset(ph)` durch `reset_phases()`, baue neu, lass `"mod: snap_pitch_phase*"` laufen. Erwartet: die MOTION-Zusicherung schlägt fehl. Zurücknehmen. Beide beobachteten Fehlermeldungen in den Bericht.

- [ ] **Step 7: Commit**

```bash
git add engine/mod/lane.h engine/mod/lane.cpp engine/mod/super_modulator.h tests/test_lane.cpp
git commit -m "feat(mod): step_index und ein pitch-only Snap-Hook

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 2: Der Sampler nimmt den Cursor entgegen

**Files:**
- Modify: `engine/sampler/sampler_engine.h` (Deklaration neben `set_phrase_pos`, `:194`)
- Modify: `engine/sampler/sampler_engine.cpp` (Definition neben `set_phrase_pos`, `:1048`)
- Test: `tests/test_sampler_engine.cpp`

**Interfaces:**
- Consumes: die Member `_cursor`, `_walk`, `_last_slot` (`sampler_engine.h:385-387`), die Wrap-Erkennung in `_fire_slice` (`sampler_engine.cpp:786`).
- Produces (Task 3 nutzt genau das): `void SamplerEngine::snap_phrase_cursor(int slot)`.

- [ ] **Step 1: Den fehlschlagenden Test schreiben**

Ans Ende von `tests/test_sampler_engine.cpp` anhängen:

```cpp
// --- STEP-Einstiegs-Snap: der Slice-Cursor (spec 2026-07-23) ---------------
//
// Beim Schnappen auf Zaehlzeit n soll Slice n klingen, nicht Slice 1 -- sonst
// laege das Material dauerhaft gegen die Phrase versetzt, bis der naechste
// Wrap es geraderueckt.
//
// _last_slot MITZUSETZEN ist der Kern dieses Tests und keine Kosmetik: ohne
// das sieht _fire_slice beim naechsten Feuern einen rueckwaerts gesprungenen
// Slot, haelt das fuer einen Phrasen-Wrap und nullt den Cursor sofort wieder
// (sampler_engine.cpp:786) -- die Ausrichtung waere nach einem Schritt weg.
TEST_CASE("sampler: snap_phrase_cursor aligns the slice cursor and survives the next fire") {
    Rig g;
    g.e.snap_phrase_cursor(3);
    CHECK(g.e.test_cursor()    == 3);
    CHECK(g.e.test_last_slot() == 3);

    // Der Wrap-Waechter darf beim naechsten regulaeren Push NICHT zuschlagen:
    // Slot 4 folgt auf 3, das ist vorwaerts.
    g.e.set_phrase_pos(4, 8, 0.f);
    CHECK(g.e.test_last_slot() == 3);   // erst _fire_slice zieht nach
}
```

`test_cursor()` und `test_last_slot()` sind Beobachter, die es noch nicht gibt. **Bevor du sie anlegst:** sieh nach, wie diese Datei sonst an interne Sampler-Zustände kommt (`test_scan_rate`/`test_detune_factor` in `sampler_engine.h:39` sind das etablierte Muster für Test-Nähte). Gibt es bereits einen Weg an `_cursor` heranzukommen, nimm ihn und schreibe den Test darauf um, statt neue Beobachter zu erfinden. Nur wenn es keinen gibt, lege genau diese zwei als `int test_cursor() const` / `int test_last_slot() const` an, mit einem Kommentar, dass sie Testnähte sind.

- [ ] **Step 2: Test laufen lassen, Fehlschlag bestätigen**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build --target spky_tests && ./build/spky_tests -tc="sampler: snap_phrase_cursor*"
```

Erwartet: Compile-Fehler — `snap_phrase_cursor` existiert nicht.

- [ ] **Step 3: Deklaration und Definition**

In `engine/sampler/sampler_engine.h`, direkt hinter `set_phrase_pos` (`:194`):

```cpp
    // STEP-Einstiegs-Snap (spec 2026-07-23 sampler-performance-fixes): der
    // Moduswechsel FLOW->STEP setzt das Deck auf die Taktposition, an der der
    // Transport steht -- der Cursor muss auf denselben Slot, damit das
    // Material zur Zaehlzeit passt und nicht bei Slice 1 anfaengt.
    //
    // Setzt _last_slot MIT: ohne das haelt _fire_slice den Sprung fuer einen
    // Phrasen-Wrap und nullt den Cursor beim ersten Feuern wieder.
    void snap_phrase_cursor(int slot);
```

In `engine/sampler/sampler_engine.cpp`, direkt hinter der `set_phrase_pos`-Definition (`:1048-1050`):

```cpp
void SamplerEngine::snap_phrase_cursor(int slot) {
    if (slot < 0) slot = 0;
    _cursor    = slot;
    _last_slot = slot;
    // _walk gehoert zum Cursor: im Wrap-Pfad (:786) werden die beiden immer
    // zusammen gesetzt, und ein stehengebliebener Walk-Offset verschoebe die
    // gerade hergestellte Ausrichtung sofort wieder.
    _walk      = 0.f;
}
```

- [ ] **Step 4: Tests laufen lassen, dann die volle Suite**

```bash
./build/spky_tests -tc="sampler: snap_phrase_cursor*"
./build/spky_tests
```

Erwartet: grün, Suite grün bei 589. **Beide Golden Vectors unberührt** — `snap_phrase_cursor` hat noch keinen Aufrufer. Bewegt sich einer: **STOP und melden.**

- [ ] **Step 5: Beweisen, dass der Test trägt**

Lösche testweise die Zeile `_last_slot = slot;`, baue neu, lass den Fall laufen. Erwartet: die `test_last_slot()`-Zusicherung schlägt fehl. Zurücknehmen, Fehlermeldung in den Bericht.

- [ ] **Step 6: Commit**

```bash
git add engine/sampler/sampler_engine.h engine/sampler/sampler_engine.cpp tests/test_sampler_engine.cpp
git commit -m "feat(sampler): snap_phrase_cursor richtet den Slice-Cursor aus

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 3: Part erkennt die Flanke

**Files:**
- Modify: `engine/parts/part.h` (zwei `bool`-Member, ein Beobachter, eine Methode)
- Modify: `engine/parts/part.cpp` (`set_step`, `:86-90`)
- Test: `tests/test_part.cpp`

**Interfaces:**
- Consumes: `SamplerEngine::snap_phrase_cursor(int)` aus Task 2; `_step_on`, `_engine_id`, `_switching`, `_pending_engine` (`part.h`), `_sampler`.
- Produces (Task 4 nutzt genau das): `bool Part::take_step_snap()` — liefert `true` **genau einmal** nach einer FLOW→STEP-Flanke und löscht das Flag dabei; `void Part::snap_sampler_cursor(int slot)` — leitet an die Sampler-Engine weiter, wenn dieses Deck auf Sampler steht (bzw. gerade dorthin wechselt), sonst ein No-op.

- [ ] **Step 1: Den fehlschlagenden Test schreiben**

An `tests/test_part.cpp` anhängen:

```cpp
// --- STEP-Einstiegs-Snap: die Flanke (spec 2026-07-23) ---------------------
//
// Hosts pushen set_step in JEDEM Control-Tick mit dem Schalterzustand
// (Spotymod.cpp:503), nicht nur beim Wechsel. Der Snap darf deshalb an der
// FLANKE haengen und nicht am Zustand, sonst schnappt das Deck jeden Tick neu
// und stuende dauerhaft still auf dem Raster.
TEST_CASE("part: the FLOW->STEP edge raises the snap request exactly once") {
    Part p;
    p.init(48000.f, 0, nullptr, nullptr, nullptr, 0);

    // Erste Beobachtung des Schalters nach init(): KEIN Snap. Ein Patch, der
    // mit STEP an geladen wird, erzeugt hier eine steigende Flanke, aber es
    // gab keine Geste und keine Wolke, aus der man kaeme.
    p.set_step(true, 8);
    CHECK(p.take_step_snap() == false);

    // Zurueck in die Wolke und wieder heraus: DAS ist die Geste.
    p.set_step(false, 8);
    CHECK(p.take_step_snap() == false);
    p.set_step(true, 8);
    CHECK(p.take_step_snap() == true);

    // Genau einmal -- take_ loescht das Flag.
    CHECK(p.take_step_snap() == false);

    // Derselbe Zustand noch einmal gepusht ist keine Flanke.
    p.set_step(true, 8);
    CHECK(p.take_step_snap() == false);

    // Und der Rueckweg in die Wolke schnappt nicht.
    p.set_step(false, 8);
    CHECK(p.take_step_snap() == false);
}
```

Lies die Rig-Idiome am Anfang von `tests/test_part.cpp` und folge ihnen; braucht `Part::init` dort andere Argumente als oben, gilt die Datei.

- [ ] **Step 2: Test laufen lassen, Fehlschlag bestätigen**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build --target spky_tests && ./build/spky_tests -tc="part: the FLOW->STEP edge*"
```

Erwartet: Compile-Fehler — `take_step_snap` existiert nicht.

- [ ] **Step 3: Die Flankenerkennung einbauen**

In `engine/parts/part.h`, zu den Membern (neben `_step_on`):

```cpp
    // STEP-Einstiegs-Snap (spec 2026-07-23 sampler-performance-fixes).
    // _step_seen unterscheidet die erste Beobachtung des Schalters von einer
    // echten Geste: Hosts pushen set_step jeden Tick, und _step_on bootet auf
    // false -- ein mit STEP an geladenes Patch erzeugte sonst beim ersten
    // Push eine Flanke und schnappte, ohne dass jemand etwas geschaltet haette.
    bool _step_seen = false;
    bool _step_snap = false;
```

und zu den Methoden:

```cpp
    // Liefert true GENAU EINMAL nach einer FLOW->STEP-Flanke und loescht das
    // Flag dabei. Center konsumiert es im Control-Tick; Part selbst weiss
    // nichts vom Transport (dieselbe Regel wie beim COLOR-Push: Part schiebt
    // Werte, keine Politik).
    bool take_step_snap() { const bool w = _step_snap; _step_snap = false; return w; }
    // Reicht den Slot an die Sampler-Engine weiter, wenn dieses Deck auf
    // Sampler steht. Waehrend eines Engine-Wechsels zaehlt die ZIEL-Engine --
    // sonst verloere ein Wechsel "auf Sampler und auf STEP zugleich" die
    // Ausrichtung.
    void snap_sampler_cursor(int slot) {
        const EngineId e = _switching ? _pending_engine : _engine_id;
        if (e == ENGINE_SAMPLER) _sampler.snap_phrase_cursor(slot);
    }
```

Prüfe die tatsächlichen Namen und Typen von `_switching`, `_pending_engine` und `_engine_id` in `part.h` und folge ihnen.

In `engine/parts/part.cpp`, `set_step` (`:86-90`) ersetzen durch:

```cpp
void Part::set_step(bool on, int steps) {
    // Die steigende Flanke ist die Geste, nicht der Zustand: der Host pusht
    // hier jeden Control-Tick denselben Wert.
    if (_step_seen && on && !_step_on) _step_snap = true;
    _step_seen = true;
    _step_on = on;
    _mod.set_step(on, steps);
    _engine->set_flow(!on);
}
```

- [ ] **Step 4: Tests laufen lassen, dann die volle Suite**

```bash
./build/spky_tests -tc="part: the FLOW->STEP edge*"
./build/spky_tests
```

Erwartet: grün, Suite grün bei 590. Das Flag hat noch keinen Konsumenten, es ändert also kein Verhalten. Beide Golden Vectors unberührt.

- [ ] **Step 5: Beweisen, dass der Test trägt**

Entferne testweise die `_step_seen &&`-Bedingung, baue neu, lass den Fall laufen. Erwartet: die erste Zusicherung (kein Snap beim Laden) schlägt fehl. Zurücknehmen. Dann lass `take_step_snap` das Flag nicht löschen (`return _step_snap;`), baue neu: die „genau einmal"-Zusicherung schlägt fehl. Zurücknehmen. Beide Fehlermeldungen in den Bericht.

- [ ] **Step 6: Commit**

```bash
git add engine/parts/part.h engine/parts/part.cpp tests/test_part.cpp
git commit -m "feat(part): die FLOW->STEP-Flanke meldet einen Snap-Wunsch

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 4: Center führt den Snap aus

**Files:**
- Modify: `engine/center/center.h` (Deklaration einer privaten Hilfsmethode)
- Modify: `engine/center/center.cpp` (`update`, `:72-76`)
- Test: `tests/test_center.cpp`

**Interfaces:**
- Consumes: `Part::take_step_snap()` und `Part::snap_sampler_cursor(int)` aus Task 3; `SuperModulator::snap_pitch_phase(float)` und `ModLane::step_index(float,int)` aus Task 1; `SuperModulator::pitch_phase()`, `pitch_steps()`, `division()`, `clock_scale()`; `_transport.beats()`, `_grid_off[2]`, `_sync`.
- Produces: nichts, was eine spätere Task konsumiert.

**Die zentrale Zusicherung:** die Phase wird **auf** das Servo-Ziel gesetzt, nicht das Ziel auf die Phase. Damit ist `err` in `_grid_servo` ab dem ersten Sample 0 — kein Zerren am Tempo. Das ist die Umkehrung von `_rebase_grid` und der ganze Punkt der Übung.

- [ ] **Step 1: Den fehlschlagenden Test schreiben**

An `tests/test_center.cpp` anhängen. Das Rig dieser Datei (`:12-19`) reicht `SuperModulator`s herein, die **nicht** `pa.mod()`/`pb.mod()` sind — der Snap muss deshalb auf den übergebenen Referenzen arbeiten. Genau das prüfen diese Fälle mit.

```cpp
// --- STEP-Einstiegs-Snap (spec 2026-07-23 sampler-performance-fixes) -------
//
// Die Phase wird AUF das Servo-Ziel gesetzt statt umgekehrt. Der Servofehler
// ist damit ab dem ersten Sample 0 -- das ist die Zusage gegenueber RST, das
// den Downbeat nullt und beide Decks mitreisst.
TEST_CASE("center: the FLOW->STEP snap lands the deck on the running grid") {
    Rig r; r.init();
    r.c.set_sync(true);
    run_synced(r, 40);                       // Transport laeuft, Phasen wandern

    // Ein Offset, wie ihn der Free-Run hinterlaesst.
    r.pa.set_step(true, 8);                  // erste Beobachtung: kein Snap
    r.pa.set_step(false, 8);
    run_synced(r, 10);

    r.pa.set_step(true, 8);                  // DAS ist die Flanke
    r.c.update(r.a, r.b, r.pa, r.pb);

    // Ziel exakt so gerechnet, wie _grid_servo es tut -- mit genulltem Offset.
    const float cpb = kDivisions[r.a.division()].cpb * r.a.clock_scale();
    const double t  = r.c.transport().beats() * static_cast<double>(cpb);
    const float tgt = static_cast<float>(t - std::floor(t));
    CHECK(r.a.pitch_phase() == doctest::Approx(tgt).epsilon(1e-4));
}

// Kein Zerren: nach dem Snap darf der Servo das Deck nicht mehr ziehen. Ohne
// diese Zusicherung ginge auch ein naives "nur _grid_off nullen" durch -- und
// genau das zerrt (Rack-Bericht 2026-07-17).
TEST_CASE("center: the snap leaves no servo error behind") {
    Rig r; r.init();
    r.c.set_sync(true);
    run_synced(r, 40);
    r.pa.set_step(true, 8);
    r.pa.set_step(false, 8);
    run_synced(r, 10);

    r.pa.set_step(true, 8);
    r.c.update(r.a, r.b, r.pa, r.pb);

    const float cpb = kDivisions[r.a.division()].cpb * r.a.clock_scale();
    const double t  = r.c.transport().beats() * static_cast<double>(cpb);
    const float err = wrap_err(static_cast<float>(t - std::floor(t)) - r.a.pitch_phase());
    CHECK(std::fabs(err) < 1e-4f);
}

// Das andere Deck und der Transport bleiben unangetastet -- der ganze Grund,
// warum das nicht reset_transport ruft.
TEST_CASE("center: the snap touches neither the transport nor the other deck") {
    Rig r; r.init();
    r.c.set_sync(true);
    run_synced(r, 40);

    const float b_before = r.b.pitch_phase();
    const double beats_before = r.c.transport().beats();

    r.pa.set_step(true, 8);
    r.pa.set_step(false, 8);
    r.pa.set_step(true, 8);
    r.c.update(r.a, r.b, r.pa, r.pb);

    CHECK(r.b.pitch_phase() == doctest::Approx(b_before).epsilon(1e-6));
    CHECK(r.c.transport().beats() == doctest::Approx(beats_before).epsilon(1e-9));
}

// Freie Welt: ohne Transport ist das andere Deck die Referenz.
TEST_CASE("center: without SYNC the snap lands on the other deck's phase") {
    Rig r; r.init();
    r.c.set_sync(false);
    run_synced(r, 40);
    REQUIRE(r.a.pitch_phase() != doctest::Approx(r.b.pitch_phase()).epsilon(1e-3));

    const float b_phase = r.b.pitch_phase();
    r.pa.set_step(true, 8);
    r.pa.set_step(false, 8);
    r.pa.set_step(true, 8);
    r.c.update(r.a, r.b, r.pa, r.pb);

    CHECK(r.a.pitch_phase() == doctest::Approx(b_phase).epsilon(1e-4));
}

// Schalten beide im selben Tick, bleibt A stehen und B schnappt auf A. Ohne
// diese Regel schnappten beide auf die jeweils andere Vorher-Phase und
// tauschten sie nur.
TEST_CASE("center: when both decks switch in one tick, A is the reference") {
    Rig r; r.init();
    r.c.set_sync(false);
    run_synced(r, 40);
    const float a_before = r.a.pitch_phase();
    REQUIRE(r.b.pitch_phase() != doctest::Approx(a_before).epsilon(1e-3));

    r.pa.set_step(true, 8); r.pb.set_step(true, 8);
    r.pa.set_step(false, 8); r.pb.set_step(false, 8);
    r.pa.set_step(true, 8); r.pb.set_step(true, 8);
    r.c.update(r.a, r.b, r.pa, r.pb);

    CHECK(r.a.pitch_phase() == doctest::Approx(a_before).epsilon(1e-4));
    CHECK(r.b.pitch_phase() == doctest::Approx(a_before).epsilon(1e-4));
}
```

- [ ] **Step 2: Test laufen lassen, Fehlschlag bestätigen**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build --target spky_tests && ./build/spky_tests -tc="center: the FLOW->STEP snap*,center: the snap*,center: without SYNC*,center: when both decks*"
```

Erwartet: die Phasen-Fälle schlagen fehl (niemand schnappt), der Transport-Fall besteht bereits — er pinnt die Zusage, die diese Task nicht brechen darf.

- [ ] **Step 3: Den Snap einbauen**

In `engine/center/center.h`, zu den privaten Methoden (neben `_rebase_grid`):

```cpp
    // STEP-Einstiegs-Snap (spec 2026-07-23 sampler-performance-fixes): setzt
    // die Pitch-Phase des Decks i AUF das Grid-Ziel, statt wie _rebase_grid
    // das Ziel auf die Phase. Damit startet der Servofehler bei 0 -- kein
    // Zerren, kein Tempo-Wobble.
    void _snap_to_grid(SuperModulator& m, Part& p, int i, const SuperModulator& other);
```

In `engine/center/center.cpp`, in `update` direkt hinter den beiden `_rebase_grid`-Aufrufen (`:74-75`):

```cpp
    // NACH _rebase_grid, und das ist eine Zusicherung: der Moduswechsel
    // aendert clock_scale (1 in FLOW, 8/S in STEP), loest also im selben Tick
    // einen Rebase aus. Liefe der Snap davor, schriebe der Rebase den gerade
    // genullten Offset sofort wieder voll.
    //
    // Bank A ist die Phasenreferenz des Paars (siehe die Grid-Gravity unten),
    // deshalb wird A zuerst konsumiert und B sieht As bereits gesnappte Phase.
    // Schalten beide im selben Tick, bleibt A damit stehen und B landet auf A,
    // statt dass die beiden nur ihre Vorher-Phasen tauschen.
    if (pa.take_step_snap()) _snap_to_grid(a, pa, 0, b);
    if (pb.take_step_snap()) _snap_to_grid(b, pb, 1, a);
```

Und die Definition, neben `_rebase_grid`:

```cpp
// Siehe die Deklaration in center.h. Der Offset wird ZUERST genullt: das Ziel
// unten muss mit dem genullten Offset gerechnet werden, sonst landet der Snap
// um genau den alten Offset daneben. In der freien Welt liest den Offset
// niemand -- genullt wird er trotzdem, sonst zoege ein spaeter eingeschaltetes
// SYNC einmal am Tempo.
void Center::_snap_to_grid(SuperModulator& m, Part& p, int i,
                           const SuperModulator& other) {
    _grid_off[i] = 0.f;
    _grid_cs[i]  = m.clock_scale();   // sonst rebased der naechste Tick sofort

    float tgt;
    if (_sync) {
        const float cpb = kDivisions[m.division()].cpb * m.clock_scale();
        const double t  = _transport.beats() * static_cast<double>(cpb);
        tgt = static_cast<float>(t - std::floor(t));
    } else {
        // Ohne Transport gibt es kein Raster: das andere Deck ist die einzige
        // sinnvolle Referenz. Roh auf roh -- die Kopplung und beide Servos
        // rechnen mit pitch_phase(), und die Schrittgrenzen der Lane haengen
        // an derselben Groesse.
        tgt = other.pitch_phase();
    }

    m.snap_pitch_phase(tgt);
    p.snap_sampler_cursor(ModLane::step_index(tgt, m.pitch_steps()));
}
```

Der `_grid_cs[i]`-Eintrag ist kein Detail: `_rebase_grid` feuert bei jeder Änderung von `clock_scale` (`:196-198`), und der Moduswechsel ist genau so eine. Ohne die Zeile schriebe der Rebase im **nächsten** Tick den Offset neu. Prüfe den Namen des Members in `center.h` und folge ihm.

- [ ] **Step 4: Tests laufen lassen, dann die volle Suite**

```bash
./build/spky_tests -tc="center: the FLOW->STEP snap*,center: the snap*,center: without SYNC*,center: when both decks*"
./build/spky_tests
```

Erwartet: alle grün, Suite grün bei 595. **Beide Golden Vectors dürfen sich nicht bewegen** — sie treiben die Engine direkt, ohne Center.

**Erwartete Kollateraltreffer:** mehrere bestehende Tests schalten mitten im Lauf von FLOW auf STEP, etwa `tests/test_sampler_part.cpp:181` (SYNC aus, also der Freie-Welt-Zweig) und die Annahme „STEP-Einstieg: Schritt −1 → 0" bei `tests/test_instrument.cpp:283-286`. Wird dort etwas rot, ist das ein **Befund und wird gemeldet**, nicht umgebogen: entweder die Zusicherung war zu eng gefasst, oder der Snap tut etwas Unerwünschtes. Beide Möglichkeiten gehören in den Bericht, mit der Fehlermeldung.

- [ ] **Step 5: Beweisen, dass die Tests tragen**

Verschiebe die beiden Snap-Zeilen testweise **vor** die `_rebase_grid`-Aufrufe, baue neu, lass die Center-Fälle laufen. Erwartet: der Grid-Fall schlägt fehl, weil der Rebase den Offset zurückschreibt. Zurücknehmen. Dann tausche in `_snap_to_grid` `m.snap_pitch_phase(tgt)` gegen `_grid_off[i] = 0.f;` allein (also nur Offset nullen, Phase stehen lassen), baue neu: der Servofehler-Fall muss fehlschlagen. Zurücknehmen. Beide Fehlermeldungen in den Bericht.

- [ ] **Step 6: Commit**

```bash
git add engine/center/center.h engine/center/center.cpp tests/test_center.cpp
git commit -m "feat(center): der STEP-Einstieg schnappt aufs laufende Raster

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 5: Die SCAN-Kurve

**Files:**
- Modify: `engine/sampler/sampler_config.h` (Konstanten und Kommentarblock, `:232-247`)
- Modify: `engine/sampler/sampler_engine.cpp` (`scan_rate`, `:47-58`)
- Modify: `host/vcv/README.md` (`:66`, `:78-81`)
- Modify: `host/vcv/src/Spotymod.cpp` (Begründung bei `:461-464`)
- Modify: `bench/workloads_sampler.cpp` (`:178-226`) und ggf. `bench/run.py` (`:276`)
- Test: `tests/test_sampler_engine.cpp`

**Interfaces:**
- Consumes: nichts aus Task 1–4. Diese Task ist unabhängig und könnte auch zuerst laufen.
- Produces: nichts.

- [ ] **Step 1: Den fehlschlagenden Test schreiben**

An `tests/test_sampler_engine.cpp` anhängen. Der bestehende Kurventest (`:1699-1733`) prüft Totzone, 1.0x am Knie, Maximum bei ±1, Stetigkeit und Monotonie — alles gegen die Konstanten beim Namen, alles überlebt. **Er wird nicht angefasst.** Was ihm fehlt, ist die Linearität selbst:

```cpp
// Die untere Zone ist seit spec 2026-07-23 (sampler-performance-fixes)
// LINEAR, nicht mehr exponentiell: exponentiell verbrachte sie den halben
// Reglerweg unter 0.1x (bei halb aufgedreht 0.09x -- der Kopf kroch), und der
// Sprung ins oberste Viertel wirkte dadurch doppelt hart.
//
// Geprueft wird die Mittelwert-Eigenschaft, denn genau die unterscheidet eine
// Gerade von jeder gebogenen Kurve: der Wert in der Mitte zweier Positionen
// ist der Mittelwert ihrer Raten. Ohne diesen Fall haelt nichts die Linearitaet
// fest, und die naechste Kurvenidee kippt sie unbemerkt zurueck.
TEST_CASE("sampler: the SCAN curve is linear below the knee") {
    using namespace sampler_cfg;
    for (int i = 1; i < 8; ++i) {
        const float lo = kScanDead + (kScanKnee - kScanDead) * (float(i) - 1.f) / 8.f;
        const float hi = kScanDead + (kScanKnee - kScanDead) * (float(i) + 1.f) / 8.f;
        const float mid = 0.5f * (lo + hi);
        INFO("i=" << i << " lo=" << lo << " hi=" << hi);
        CHECK(test_scan_rate(mid)
              == doctest::Approx(0.5f * (test_scan_rate(lo) + test_scan_rate(hi)))
                     .epsilon(1e-3));
    }

    // Bei halb aufgedreht laeuft der Kopf jetzt hoerbar -- die Zahl, um die es
    // in der Spec geht. Frueher: 0.09x.
    CHECK(test_scan_rate(0.5f) > 0.5f);
}
```

- [ ] **Step 2: Test laufen lassen, Fehlschlag bestätigen**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build --target spky_tests && ./build/spky_tests -tc="sampler: the SCAN curve is linear*"
```

Erwartet: rot — die Kurve ist noch exponentiell.

- [ ] **Step 3: Die Kurve umstellen**

In `engine/sampler/sampler_engine.cpp`, `scan_rate` (`:47-58`), den unteren Zweig ersetzen. Aus

```cpp
    if (a <= kScanKnee) {
        const float t = (a - kScanDead) / (kScanKnee - kScanDead);
        return sign * kScanMinRate * std::pow(1.f / kScanMinRate, t);
    }
```

wird

```cpp
    if (a <= kScanKnee) {
        const float t = (a - kScanDead) / (kScanKnee - kScanDead);
        return sign * lerpf(kScanMinRate, 1.f, t);
    }
```

Der obere Zweig bleibt unverändert. Damit entfällt das letzte `std::pow` aus dieser Funktion — prüfe, ob `<cmath>` bzw. der `std::pow`-Include in dieser Datei noch von etwas anderem gebraucht wird, und lass ihn stehen, wenn ja.

In `engine/sampler/sampler_config.h`, `kScanMaxRate` (`:247`):

```cpp
constexpr float  kScanMaxRate = 4.f;
```

- [ ] **Step 4: Den Kommentarblock wahr machen**

Der Block bei `sampler_config.h:232-243` beschreibt die alte Kurve und schreibt ausdrücklich fest: *„The top quarter carries the factor 8 and is the steepest stretch of the curve — if it plays too nervously, the fix is an exponential top segment, not a smaller range"*. Genau diese Entscheidung wird hier umgestoßen.

Schreibe den Block so um, dass er die neue Form trägt: untere Zone linear von `kScanMinRate` auf 1.0x, obere Zone linear auf `kScanMaxRate`, Totzone und Knie unverändert und mit ihrer bisherigen Begründung (findbare Echtzeit bei 0.75, eingefrorener Kopf bleibt eingefroren). Nimm ausdrücklich auf, **dass** das kleinere Maximum eine bewusste Rücknahme der früheren Entscheidung ist und **warum** (das steilste Viertel war im Spiel der störendste Teil des Reglers), damit die Zeile nicht später als Versehen „repariert" wird. Nenne die Spec `2026-07-23-sampler-performance-fixes`.

Halte fest, dass die Gerade bei `kScanMinRate` beginnt und nicht bei 0: bei 0 zu starten würde die Kante der Totzone verwischen, weil direkt daneben der Kopf ebenfalls praktisch stillstünde.

- [ ] **Step 5: Die vier Stellen außerhalb der Engine nachziehen**

Alle vier beschreiben die alte Kurve. Lies jede und stelle den Stand her, der zur neuen passt:

1. `host/vcv/README.md:66` und `:78-81` — die exponentielle Form, die 8x und die Init-Patch-Notiz „−8x realtime". Mit dem neuen Maximum wird daraus −4x, und der dort genannte Reglerwert −0.728 ergibt jetzt ~−0.97x statt −0.81x: nicht nur eine andere Zahl, sondern eine andere beschriebene Wirkung. Rechne den Wert nach, bevor du ihn hinschreibst.
2. `host/vcv/src/Spotymod.cpp:461-464` — begründet ein Sampler-Gate (K-03) mit dem `std::pow` im exponentiellen Zweig. Das `pow` ist weg. Das Gate bleibt; seine Begründung muss neu geschrieben werden.
3. `bench/workloads_sampler.cpp:178-226` samt Eintrag in `bench/run.py:276` — misst eigens dieses `std::pow` und verliert damit seine Prämisse. **Nicht eigenmächtig löschen:** beschreibe im Bericht, was der Workload jetzt noch misst, und schlage vor, ihn umzuwidmen oder zu entfernen. Die Entscheidung trifft der Autor.
4. Der Testfallname `tests/test_sampler_engine.cpp:1699` sagt „tops at 8x", der Kommentar bei `:1915-1917` spricht vom „sub-knee exponential segment". Beide Zusicherungen überleben; die Wörter nicht. Titel und Kommentar anpassen, **keine** Zusicherung ändern.

- [ ] **Step 6: Tests laufen lassen, dann die volle Suite**

```bash
./build/spky_tests -tc="sampler: the SCAN curve*"
./build/spky_tests
```

Erwartet: grün, Suite grün bei 596. **Beide Golden Vectors unberührt** — keiner ruft `set_scan`. Bewegt sich einer: **STOP und melden.**

- [ ] **Step 7: Beweisen, dass der Test trägt**

Setze den unteren Zweig testweise auf die alte exponentielle Form zurück, baue neu, lass `"sampler: the SCAN curve is linear*"` laufen. Erwartet: die Mittelwert-Zusicherung schlägt fehl. Zurücknehmen, Fehlermeldung in den Bericht.

- [ ] **Step 8: Commit**

```bash
git add engine/sampler/sampler_config.h engine/sampler/sampler_engine.cpp \
        tests/test_sampler_engine.cpp host/vcv/README.md host/vcv/src/Spotymod.cpp \
        bench/workloads_sampler.cpp bench/run.py
git commit -m "feat(sampler): SCAN laeuft unterhalb des Knies linear, Maximum 4x

Exponentiell verbrachte der Regler den halben Weg unter 0.1x und sprang
dann ins oberste Viertel. Die Ruecknahme des 8x-Maximums ist bewusst.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 6: MOD wirkt quadratisch auf die Leseposition

**Files:**
- Modify: `engine/sampler/sampler_config.h` (Konstante anhängen)
- Modify: `engine/parts/part.cpp` (`target_raw`, `:44-49`)
- Test: `tests/test_sampler_part.cpp`

**Interfaces:**
- Consumes: `Part::target_raw` (`part.cpp:44`), `_engine_id`, `_depth`, `_tdepth[LANE_SOURCE]`.
- Produces: nichts.

**Die Zahlen, um die es geht:** `_tdepth[LANE_SOURCE]` ist **1.0** (`part.h:273` in Verbindung mit `LANE_SOURCE = 0`, `lane_id.h:8`) — die SOURCE-Lane läuft ungedämpft über die gesamte Aufnahme. Bei MOD 0.3 wandert die Leseposition dadurch um ±30% des Materials. Quadratisch werden daraus ±9%.

- [ ] **Step 1: Den fehlschlagenden Test schreiben**

An `tests/test_sampler_part.cpp` anhängen:

```cpp
// --- MOD auf die Leseposition (spec 2026-07-23 sampler-performance-fixes) ---
//
// Die SOURCE-Lane greift ueber die GESAMTE Aufnahme (sampler_engine.cpp:548)
// und ist ungedaempft (_tdepth[LANE_SOURCE] == 1.0). Linear wanderte die
// Position schon bei MOD 0.3 um +-30% des Materials -- eine Prise MOD warf
// die Position durchs Material. Quadratisch bleibt das Ausbrechen erhalten,
// rueckt aber ans obere Reglerende.
TEST_CASE("sampler part: MOD moves the read position quadratically") {
    auto swing = [](float depth) {
        std::vector<SampleBuffer::Frame> sbuf(kSFrames, SampleBuffer::Frame{ 0.f, 0.f });
        Part p;
        p.init(48000.f, 0, nullptr, nullptr, sbuf.data(), sbuf.size());
        p.set_engine(ENGINE_SAMPLER);
        p.set_target_base(LANE_SOURCE, 0.5f);   // Mitte: Platz nach beiden Seiten
        p.set_depth(depth);
        p.set_target_active(LANE_SOURCE, true);

        float lo = 2.f, hi = -2.f;
        for (int i = 0; i < 48000; ++i) {
            float a = 0.f, b = 0.f;
            p.process(a, b);
            const float v = p.target_value(LANE_SOURCE);
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        return hi - lo;
    };

    const float half = swing(0.5f);
    const float full = swing(1.f);

    // Bei halber MOD ein Viertel der vollen Auslenkung, nicht die Haelfte --
    // das ist die Quadratur, und der Faktor, den ein linearer Nachbau nicht
    // trifft.
    CHECK(half == doctest::Approx(0.25f * full).epsilon(0.1));

    // Bei voll aufgerissener MOD aendert sich nichts (1^2 == 1): die Zusage,
    // dass oben nichts verloren geht. Verglichen wird gegen die ungedaempfte
    // Reichweite der Lane, nicht gegen sich selbst.
    CHECK(full > 0.8f);
}

// Auf einem Synth-Deck bedeutet SOURCE etwas anderes und bleibt linear --
// die Kurve ist eine Sampler-Entscheidung, keine Aenderung am Mod-Plane.
TEST_CASE("sampler part: the SOURCE curve leaves a synth deck alone") {
    auto swing = [](float depth) {
        Part p;
        p.init(48000.f, 0, nullptr, nullptr, nullptr, 0);
        p.set_engine(ENGINE_SYNTH);
        p.set_target_base(LANE_SOURCE, 0.5f);
        p.set_depth(depth);
        p.set_target_active(LANE_SOURCE, true);

        float lo = 2.f, hi = -2.f;
        for (int i = 0; i < 48000; ++i) {
            float a = 0.f, b = 0.f;
            p.process(a, b);
            const float v = p.target_value(LANE_SOURCE);
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        return hi - lo;
    };

    CHECK(swing(0.5f) == doctest::Approx(0.5f * swing(1.f)).epsilon(0.1));
}
```

Prüfe die genauen Namen von `set_target_base`, `set_target_active`, `set_depth` und `target_value` in `part.h` und folge ihnen; `set_depth` existiert in diesem Codebase mehrfach in verschiedenen Scopes, nimm das `Part`-eigene.

- [ ] **Step 2: Test laufen lassen, Fehlschlag bestätigen**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build --target spky_tests && ./build/spky_tests -tc="sampler part: MOD moves the read position*,sampler part: the SOURCE curve leaves*"
```

Erwartet: der erste Fall schlägt fehl (heute ist `half` die Hälfte von `full`, nicht ein Viertel), der zweite besteht bereits — er pinnt die Zusage, die diese Task nicht brechen darf.

- [ ] **Step 3: Die Konstante anlegen**

Ans Ende von `engine/sampler/sampler_config.h` innerhalb `namespace sampler_cfg`:

```cpp
// --- MOD auf die Leseposition (spec 2026-07-23 sampler-performance-fixes) ---
// Die SOURCE-Lane greift auf einem Sampler-Deck ueber die GESAMTE Aufnahme
// und ist ungedaempft (_tdepth[LANE_SOURCE] == 1.0), anders als FILTER (0.55)
// und MOTION (0.7). Linear wanderte die Leseposition damit schon bei MOD 0.3
// um +-30% des Materials -- auf zehn Sekunden +-3 s. Der Exponent zieht den
// unteren Reglerweg zusammen, ohne oben etwas wegzunehmen: bei MOD 1 ist
// 1^n == 1, das Ausbrechen bleibt vollstaendig erhalten und liegt nur dort,
// wo man es bestellt.
//
// Ear-tunable. Kubisch war die Alternative (bei MOD 0.3 nur noch +-1.5% statt
// +-9%); die Entscheidung fiel bewusst auf die sanftere Kurve, weil sie das
// untere Drittel nicht stillstellt. Fuehlt es sich weiter zu nervoes an, ist
// DIESER Wert die Stellschraube -- nicht _tdepth, das die vom Nutzer gesetzte
// Ziel-Tiefe ist und keine Kennlinie.
constexpr float  kSourceModExp = 2.f;
```

- [ ] **Step 4: Die Kurve einbauen**

In `engine/parts/part.cpp`, `target_raw` (`:44-49`). Aus

```cpp
    const float d = (slot == LANE_PITCH) ? 1.f : _depth;
```

wird

```cpp
    float d = (slot == LANE_PITCH) ? 1.f : _depth;
    // Die SOURCE-Lane ist auf einem Sampler-Deck die LESEPOSITION und greift
    // ueber die gesamte Aufnahme -- linear warf schon eine Prise MOD die
    // Position durchs Material (spec 2026-07-23 sampler-performance-fixes).
    // Bei MOD 1 unveraendert (1^n == 1); gebogen wird nur der untere Weg.
    //
    // Gilt in BEIDEN Modi: _targets[LANE_SOURCE] speist ueber _base_pos()
    // auch die Slice-Basisposition in STEP. Denselben Regler je nach Modus
    // verschieden tief wirken zu lassen waere genau die versteckte Kopplung,
    // die die FEEL-Spec abgeschafft hat.
    if (slot == LANE_SOURCE && _engine_id == ENGINE_SAMPLER)
        d = std::pow(d, sampler_cfg::kSourceModExp);
```

`sampler_cfg` ist hier sichtbar: `part.h:10` inkludiert `sampler/sampler_engine.h`, das `sampler_config.h` zieht.

**Zum `std::pow`:** `target_raw` läuft im Control-Tick, nicht pro Sample — dieselbe Klasse wie `scan_rate` und `cutoff_hz`. Falls du beim Lesen feststellst, dass `target_raw` doch pro Sample gerufen wird, ist das ein **STOP-und-melden**: dann gehört die Potenz vorberechnet, wo `_depth` gesetzt wird, und das ist eine andere Bauform als hier beschrieben.

- [ ] **Step 5: Tests laufen lassen, dann die volle Suite**

```bash
./build/spky_tests -tc="sampler part: MOD moves the read position*,sampler part: the SOURCE curve leaves*"
./build/spky_tests
```

Erwartet: beide grün, Suite grün bei 598. **Beide Golden Vectors unberührt** — sie treiben die Engine direkt ohne `Part`, `target_raw` läuft dort nie.

- [ ] **Step 6: Beweisen, dass der Test trägt**

Setze `kSourceModExp` testweise auf `1.f`, baue neu, lass die beiden Fälle laufen. Erwartet: der Viertel-Fall schlägt fehl. Zurücknehmen. Dann entferne die `_engine_id == ENGINE_SAMPLER`-Bedingung, baue neu: der Synth-Fall muss fehlschlagen. Zurücknehmen. Beide Fehlermeldungen in den Bericht.

- [ ] **Step 7: Commit**

```bash
git add engine/sampler/sampler_config.h engine/parts/part.cpp tests/test_sampler_part.cpp
git commit -m "feat(sampler): MOD wirkt quadratisch auf die Leseposition

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

## Nach dem Plan

Für Bastian, zu melden statt zu entscheiden:

1. **Der Bench-Workload** (`bench/workloads_sampler.cpp:178-226`) misst ein `std::pow`, das es nicht mehr gibt. Umwidmen oder entfernen?
2. **Die Hörprobe im Rack:** `cd host/vcv && ./build-local.sh install`. Drei Fragen — trifft der STEP-Einstieg den Takt so, wie er soll? Ist SCAN mit 4x oben noch schnell genug? Reicht quadratisch, oder muss `kSourceModExp` auf 3?
3. **`kDispersionKnee` und `kSubSpreadMax`** aus der Cloud-Dispersion-Spec sind weiter ungehört — sie stehen seit v2.10.0 im Plugin.

## Plan Self-Review

**Spec-Abdeckung** — jeder Abschnitt der Spec hat eine Task:

| Spec-Abschnitt | Task |
|---|---|
| Pitch-only Hook + Onset-Ring-Politik | 1 |
| Slot aus `tgt` rechnen, ein gemeinsamer Helfer | 1 (Helfer) + 4 (Aufruf) |
| `snap_phrase_cursor`, `_last_slot` mitsetzen | 2 |
| Flankenerkennung, kein Snap beim Laden | 3 |
| Engine-Wechsel: Ziel-Engine zählt | 3 (`snap_sampler_cursor`) |
| GRID-Zweig, Offset zuerst nullen | 4 |
| Reihenfolge nach `_rebase_grid` | 4 (Step 3 + Mutationsprobe) |
| Freie Welt, beide Decks, COUPLE 0 | 4 |
| Transport und anderes Deck unberührt | 4 |
| SCAN linear unter dem Knie, Maximum 4x | 5 |
| Kommentarblock + vier Stellen außerhalb der Engine | 5 (Step 4–5) |
| MOD quadratisch, nur Sampler, beide Modi | 6 |
| Beide Golden Vectors bit-identisch | 1–6, jede Task prüft es |
| Kein Test gelöscht | Global Constraints |

**Nicht abgedeckt, bewusst:** die Nebenwirkungen von `ModLane::reset` auf einem melodischen Synth-Deck (abgeschnittene Note im Umschaltmoment) bekommen keinen eigenen Test. Die Spec nimmt sie ausdrücklich hin; ein Test würde eine Zusicherung festschreiben, die niemand versprochen hat.

**Typkonsistenz:** `step_index(float,int)->int` heißt in Task 1 und 4 gleich; `snap_pitch_phase(float)` in 1 und 4; `snap_phrase_cursor(int)` in 2, 3 und 4; `take_step_snap()->bool` und `snap_sampler_cursor(int)` in 3 und 4; `kSourceModExp` nur in 6 definiert und gelesen; `kScanMaxRate`/`kScanMinRate`/`kScanKnee`/`kScanDead` behalten ihre Namen und Typen.

**Testzählung:** 586 → 588 (T1) → 589 (T2) → 590 (T3) → 595 (T4) → 596 (T5) → 598 (T6).

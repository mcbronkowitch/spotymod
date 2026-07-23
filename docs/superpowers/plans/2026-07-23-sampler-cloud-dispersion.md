# Sampler Cloud Dispersion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** COLOR steuert auf einem Sampler-Deck in FLOW die Tonhöhenstreuung der Wolke — untere Reglerhälfte Verstimmungsbreite, obere Hälfte Spaltung in zwei Oktavlagen (Spec `docs/superpowers/specs/2026-07-23-sampler-cloud-dispersion-design.md`).

**Architecture:** Die Streumechanik existiert vollständig in `_spawn_one` (`_sub_n`, `_detune_n`, beide Rng-Züge im Vertrag) und ist an nichts angeschlossen. Eine neue Methode `SamplerEngine::set_dispersion(float)` bildet den Reglerweg über eine Zweizonenkurve auf die beiden Felder ab; `Part::_control_tick` pusht `_color_eff` hinein, neben dem bestehenden `set_feel(_color)`. Kein neuer Rng-Zug, kein neues Bedienelement, keine Änderung am STEP-Pfad.

**Tech Stack:** C++17, kein Heap in `engine/`, doctest, CMake + Ninja + clang (Desktop), deterministischer Rng mit dokumentierter Zugreihenfolge.

## Global Constraints

- **Build/Test-Schleife (immer so, nie MSVC):**
  `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build --target spky_tests && ./build/spky_tests -tc="<filter>"`
  Volle Suite: `./build/spky_tests` (Exit 0 = grün). Ausgangsstand: **581 Testfälle grün**.
- **Der VCV-Host wird NUR über `cd host/vcv && ./build-local.sh` gebaut** — der System-`g++` ist der ARM-Cross-Compiler. Dieser Plan braucht ihn nicht; nur der Panel-Generator wird ausgeführt.
- **Kein Heap, kein libDaisy in `engine/`.** Nur Arrays fester Größe.
- **Beide Golden Vectors müssen bit-identisch bleiben.** Der FLOW-Vector (`tests/test_sampler_engine.cpp:860`) läuft **nicht** mit genullten Streufeldern — er setzt selbst `set_sub(0.4f)` / `set_detune(0.5f)` (`:872-873`). Er bleibt gültig, weil er `set_dispersion` nie ruft. Bewegt sich eine der beiden Tabellen: **STOP und melden**, nicht neu aufnehmen.
- **`set_sub` und `set_detune` dürfen NICHT entfernt werden.** Der FLOW-Golden-Vector ruft beide selbst auf — sie sind Vertragsbestandteil, nicht Altlast.
- **Der Draw-Contract ändert sich nicht.** Beide Züge (SUB 4., DTUN 5.) laufen heute schon unbedingt bei jedem FLOW-Spawn. Kein Zug kommt hinzu, entfällt oder wandert.
- **STEP bleibt unberührt.** `_spawn_slice` liest weder `_sub_n` noch `_detune_n`; `_next_ratio` kehrt in STEP vor dem FLOW-Zweig zurück. Keine Task darf daran etwas ändern.
- **Kein `std::pow`/`exp2f` pro Sample.** Trigger- und Control-Rate sind in Ordnung, der Audio-Pfad nicht.
- **Commit-Trailer (jeder Commit):** `Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>` — nie der Default-Claude/Anthropic-Trailer.
- **Einen Test zu löschen ist eine gemeldete Entscheidung, keine stille Aufräumaktion.** Dieser Plan löscht keinen.
- Neue Konstanten nach `engine/sampler/sampler_config.h`, mit Kommentar der sagt, ob ear-tunable oder Vertrag — im Stil der Datei.

---

### Task 1: `set_dispersion` — die Zweizonenkurve

**Files:**
- Modify: `engine/sampler/sampler_config.h` (Konstanten anhängen)
- Modify: `engine/sampler/sampler_engine.h` (Deklaration neben `set_feel`, `:205`)
- Modify: `engine/sampler/sampler_engine.cpp` (Definition neben `set_feel`)
- Test: `tests/test_sampler_engine.cpp`

**Interfaces:**
- Consumes: `SamplerEngine::set_sub(float)` / `set_detune(float)` (`sampler_engine.cpp:1010-1011`), beide klemmen intern auf `[0,1]`; die Beobachter `sub()` / `detune()`.
- Produces (Task 2 nutzt genau das): `void SamplerEngine::set_dispersion(float n)`; `sampler_cfg::kDispersionKnee` (`float`); `sampler_cfg::kSubSpreadMax` (`float`).

**Warum die Kurve in der Engine liegt und nicht in Part:** Part pusht Reglerwerte, keine Politik — genau wie bei `set_overlap`, `set_step_clock` und `set_feel`. Die Kurve gehört dorthin, wo ihre Konstanten liegen.

- [ ] **Step 1: Den fehlschlagenden Test schreiben**

Ans Ende von `tests/test_sampler_engine.cpp` anhängen:

```cpp
// --- cloud dispersion (spec 2026-07-23) -----------------------------------
//
// Die Zonenkurve ist deterministische Arithmetik und wird auch so geprueft.
// Statistische Anteilstests waeren hier das falsche Werkzeug: langsam,
// rauschbehaftet, und sie pinnen die Knie nur indirekt. Die Mechanik DAHINTER
// (dass _sub_n wirklich Koerner halbiert, dass _detune_n wirklich verstimmt)
// ist bereits abgedeckt -- "sampler: SUB drops a share of grains an octave"
// und "sampler: DTUN spreads the grain pitches" weiter oben in dieser Datei.
TEST_CASE("sampler: the dispersion curve puts detune below the knee and sub above") {
    Rig g;
    using namespace sampler_cfg;

    // COLOR 0: beides exakt 0. Strukturell, nicht ungefaehr -- das ist die
    // Zusage, an der FLOWs Bit-Identitaet haengt.
    g.e.set_dispersion(0.f);
    CHECK(g.e.detune() == 0.f);
    CHECK(g.e.sub()    == 0.f);

    // Knapp unter dem Knie: Verstimmung fast voll, Oktavanteil noch bei null.
    g.e.set_dispersion(kDispersionKnee * 0.98f);
    CHECK(g.e.detune() == doctest::Approx(0.98f).epsilon(1e-4));
    CHECK(g.e.sub()    == 0.f);

    // Auf dem Knie: Verstimmung voll, Oktavanteil beginnt gerade.
    g.e.set_dispersion(kDispersionKnee);
    CHECK(g.e.detune() == doctest::Approx(1.f).epsilon(1e-4));
    CHECK(g.e.sub()    == 0.f);

    // Ganz oben: Verstimmung bleibt voll, Oktavanteil deckelt bei
    // kSubSpreadMax -- die Haelfte der Koerner unten, nicht alle. Bei 1.0
    // waere es eine Transposition und keine Streuung mehr.
    g.e.set_dispersion(1.f);
    CHECK(g.e.detune() == doctest::Approx(1.f).epsilon(1e-4));
    CHECK(g.e.sub()    == doctest::Approx(kSubSpreadMax).epsilon(1e-4));
}

// Die Kurve rechnet VOR den internen Klemmen von set_sub/set_detune. Ein
// Argument ausserhalb [0,1] wuerde also die Knie verschieben, bevor
// irgendeine Klemme greift -- deshalb klemmt set_dispersion selbst zuerst.
TEST_CASE("sampler: set_dispersion clamps its argument before the curve") {
    Rig g;
    using namespace sampler_cfg;

    g.e.set_dispersion(-1.f);
    CHECK(g.e.detune() == 0.f);
    CHECK(g.e.sub()    == 0.f);

    g.e.set_dispersion(4.f);
    CHECK(g.e.detune() == doctest::Approx(1.f).epsilon(1e-4));
    CHECK(g.e.sub()    == doctest::Approx(kSubSpreadMax).epsilon(1e-4));
}
```

Der `Rig`-Helfer steht bereits am Anfang der Datei; lies seine Definition und folge ihr, statt ein eigenes Setup zu bauen. Diese Tests brauchen keinen Puffer und kein `process()` — sie prüfen reine Arithmetik.

- [ ] **Step 2: Test laufen lassen, Fehlschlag bestätigen**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build --target spky_tests && ./build/spky_tests -tc="sampler: the dispersion curve*,sampler: set_dispersion clamps*"
```

Erwartet: Compile-Fehler — `set_dispersion` existiert nicht.

- [ ] **Step 3: Konstanten anlegen**

Ans Ende von `engine/sampler/sampler_config.h` innerhalb `namespace sampler_cfg`, hinter dem FEEL-Block:

```cpp
// --- cloud dispersion (spec 2026-07-23 sampler-cloud-dispersion-design.md) ---
// COLOR streut in FLOW die Tonhoehen der Wolke, in zwei Zonen ueber den
// Reglerweg. Unterhalb des Knies laeuft die Verstimmungsbreite (DTUN) auf,
// oberhalb kommt der Oktavanteil (SUB) dazu.
//
// Das Knie sitzt auf der Reglermitte, also auf einer findbaren Position --
// dasselbe Prinzip wie SCANs 1.0x und SIZEs Slice-Unity. Ear-tunable: wenn
// sich die Breite zu frueh erschoepft anfuehlt, wandert es nach oben.
constexpr float  kDispersionKnee = 0.5f;
// Groesster Oktavanteil am oberen Reglerende. NICHT 1.0: bei jedem Korn eine
// Oktave tiefer waere es eine Transposition und keine Streuung -- die Wolke
// klaenge geschlossen tiefer und spreizte gar nichts. Bei 0.5 ist die
// Streuung maximal (die Varianz des Oktav-Indikators hat dort ihr Maximum).
//
// Musikalisch heisst das ausdruecklich SPALTUNG, nicht Grundierung: am
// oberen Reglerende liegen zwei gleichwertige Lagen im Oktavabstand, die das
// Ohr trennt -- naeher an Organum als an einem Subbass. Wer stattdessen eine
// Grundierung will, braucht ~0.3, und dann traegt das Varianz-Argument
// nicht mehr. Der Wert ist ear-tunable, die Begruendung darf aber nicht
// ohne den Wert wechseln.
constexpr float  kSubSpreadMax   = 0.5f;
```

- [ ] **Step 4: Deklaration und Definition**

In `engine/sampler/sampler_engine.h`, direkt hinter `void set_feel(float n);` (`:205`):

```cpp
    // COLOR auf einem Sampler-Deck in FLOW (spec 2026-07-23): die
    // Tonhoehenstreuung der Wolke, 0..1. Setzt _detune_n und _sub_n ueber die
    // Zweizonenkurve. Bei 0 sind beide exakt 0 und FLOW ist bit-identisch mit
    // einer Version ohne diesen Regler.
    //
    // Gepusht wird der GESCHWUNGENE Reglerwert (_color_eff), anders als bei
    // set_feel. Die Regel dahinter: diskrete Ereignisse bekommen keinen
    // versteckten Swing, kontinuierliche Texturen schon. In STEP wirkt das
    // hier nichts -- _spawn_slice liest weder _sub_n noch _detune_n.
    //
    // set_sub/set_detune bleiben daneben bestehen und sind KEIN Altbestand:
    // der FLOW-Golden-Vector ruft beide selbst auf. Sie zugunsten dieser
    // Methode zu entfernen bricht einen Vertrag.
    void set_dispersion(float n);
```

In `engine/sampler/sampler_engine.cpp`, direkt hinter der `set_feel`-Definition:

```cpp
// Zweizonenkurve, siehe sampler_config.h. Klemmt zuerst: die Kurve rechnet vor
// den internen Klemmen von set_sub/set_detune, ein Argument ausserhalb [0,1]
// wuerde also die Knie verschieben, bevor irgendeine Klemme greift.
void SamplerEngine::set_dispersion(float n) {
    static_assert(kDispersionKnee > 0.f && kDispersionKnee < 1.f,
                  "das Knie muss echt zwischen den Reglerenden liegen -- "
                  "bei 0 oder 1 faellt eine der beiden Zonen weg und die "
                  "Division unten wird singulaer");
    const float a = clampf(n, 0.f, 1.f);
    set_detune(clampf(a / kDispersionKnee, 0.f, 1.f));
    set_sub(kSubSpreadMax
            * clampf((a - kDispersionKnee) / (1.f - kDispersionKnee), 0.f, 1.f));
}
```

- [ ] **Step 5: Tests laufen lassen, dann die volle Suite**

```bash
./build/spky_tests -tc="sampler: the dispersion curve*,sampler: set_dispersion clamps*"
./build/spky_tests
```

Erwartet: beide neu grün, Suite grün bei 583. Beide Golden Vectors unberührt — `set_dispersion` hat noch keinen Aufrufer. Bewegt sich einer: **STOP und melden.**

- [ ] **Step 6: Beweisen, dass die Tests tragen (Mutationsprobe)**

Vertausche die beiden Zonen (`set_detune` bekommt den oberen Ausdruck, `set_sub` den unteren), baue neu, lass die beiden Fälle laufen. Erwartet: der Kurventest schlägt fehl. Zurücknehmen. Dann setze `kSubSpreadMax` testweise auf `1.f`, baue neu: der Kurventest muss am obersten Punkt fehlschlagen. Zurücknehmen. Beide beobachteten Fehlermeldungen in den Bericht.

- [ ] **Step 7: Commit**

```bash
git add engine/sampler/sampler_config.h engine/sampler/sampler_engine.h \
        engine/sampler/sampler_engine.cpp tests/test_sampler_engine.cpp
git commit -m "feat(sampler): add the cloud dispersion curve

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 2: Part pusht COLOR, und drei Kommentare werden wieder wahr

**Files:**
- Modify: `engine/parts/part.cpp` (der `ENGINE_SAMPLER`-Block, `:244-247`)
- Modify: `engine/sampler/sampler_engine.h` (der Kommentar an `sub()` / `detune()`)
- Modify: `tests/test_sampler_part.cpp` (Testtitel und Kommentar, `:442`)
- Test: `tests/test_sampler_part.cpp`

**Interfaces:**
- Consumes: `SamplerEngine::set_dispersion(float)` aus Task 1; `Part::color_eff()` (`part.h:43`); `Part::sampler()` (`part.h:98`); die Beobachter `SamplerEngine::sub()` / `detune()`.
- Produces: nichts, was eine spätere Task konsumiert.

- [ ] **Step 1: Den fehlschlagenden Test schreiben**

An `tests/test_sampler_part.cpp` anhängen. Das Nachbar-Testcase `"sampler part: the MOTION lane breathes the grain overlap"` ist die Vorlage für das Rig — lies es und folge ihm.

```cpp
// COLOR erreicht den Sampler in FLOW als GESCHWUNGENER Wert (_color_eff),
// waehrend FEEL den rohen Knopf bekommt. Die Regel dahinter: diskrete
// Ereignisse bekommen keinen versteckten Swing, kontinuierliche Texturen
// schon. Der Beweis ist ein Deck mit weit offener MOTION-Lane: der Streuwert
// muss sich bewegen, der FEEL-Wert nicht.
TEST_CASE("sampler part: COLOR reaches the cloud swung, FEEL raw") {
    std::vector<SampleBuffer::Frame> sbuf(kSFrames, SampleBuffer::Frame{ 0.f, 0.f });
    Part p;
    p.init(48000.f, 0, nullptr, nullptr, sbuf.data(), sbuf.size());
    p.set_engine(ENGINE_SAMPLER);
    p.set_color(0.5f);                  // Reglermitte: Swing hat nach beiden Seiten Platz
    p.set_depth(1.f);
    p.set_target_active(LANE_MOTION, true);

    float dlo = 2.f, dhi = -2.f, flo = 2.f, fhi = -2.f;
    for (int i = 0; i < 48000; ++i) {
        float a = 0.f, b = 0.f;
        p.process(a, b);
        const float d = p.sampler().detune();
        if (d < dlo) dlo = d;
        if (d > dhi) dhi = d;
        const float f = p.sampler().feel();
        if (f < flo) flo = f;
        if (f > fhi) fhi = f;
    }
    CHECK(dhi > dlo + 0.02f);            // die Streuung atmet...
    CHECK(flo == doctest::Approx(0.5f)); // ...und FEEL steht still
    CHECK(fhi == doctest::Approx(0.5f));
}

// COLOR 0 muss FLOW strukturell unberuehrt lassen, AUCH bei weit offener
// MOTION-Lane. Das haengt an kColorGate (part.cpp, part.h): unterhalb von
// 0.01 Reglerweg wird der Swing ausgeblendet, sonst schoebe MOTION
// _color_eff von der Null weg und die Bit-Identitaets-Zusage der Spec fiele.
// Das Gate war fuer den Akkord da; ab jetzt traegt es auch fuer die Wolke.
TEST_CASE("sampler part: COLOR 0 leaves the cloud unswung even at full MOTION") {
    std::vector<SampleBuffer::Frame> sbuf(kSFrames, SampleBuffer::Frame{ 0.f, 0.f });
    Part p;
    p.init(48000.f, 0, nullptr, nullptr, sbuf.data(), sbuf.size());
    p.set_engine(ENGINE_SAMPLER);
    p.set_color(0.f);
    p.set_depth(1.f);
    p.set_target_active(LANE_MOTION, true);

    for (int i = 0; i < 48000; ++i) {
        float a = 0.f, b = 0.f;
        p.process(a, b);
        REQUIRE(p.sampler().detune() == 0.f);
        REQUIRE(p.sampler().sub()    == 0.f);
    }
}
```

- [ ] **Step 2: Test laufen lassen, Fehlschlag bestätigen**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build --target spky_tests && ./build/spky_tests -tc="sampler part: COLOR reaches the cloud*,sampler part: COLOR 0 leaves the cloud*"
```

Erwartet: der erste Fall schlägt fehl — `detune()` bleibt bei 0, weil niemand pusht (`CHECK(dhi > dlo + 0.02f)` mit 0 > 0). Der zweite Fall besteht bereits; er pinnt die Zusage, die diese Task nicht brechen darf.

- [ ] **Step 3: Den Push einbauen**

In `engine/parts/part.cpp`, im bestehenden `ENGINE_SAMPLER`-Block. Der Block sieht heute so aus:

```cpp
    if (_engine_id == ENGINE_SAMPLER) {
        _sampler.set_step_clock(_mod.pitch_step_samples());
        _sampler.set_feel(_color);
    }
```

Ersetze ihn durch:

```cpp
    if (_engine_id == ENGINE_SAMPLER) {
        _sampler.set_step_clock(_mod.pitch_step_samples());
        // Die beiden COLOR-Pushes lesen ABSICHTLICH verschiedene Werte, und
        // die Regel dahinter ist allgemeiner als dieser Regler:
        // DISKRETE EREIGNISSE BEKOMMEN KEINEN VERSTECKTEN SWING,
        // KONTINUIERLICHE TEXTUREN SCHON.
        //
        // FEEL ist Akzenttiefe auf einzelnen komponierten Noten -- eine
        // atmende Akzenttiefe waere genau die versteckte Kopplung, die die
        // FEEL-Spec abgeschafft hat. Also der rohe Knopf.
        //
        // Die Streuung ist eine Eigenschaft der WOLKE, und MOTION besitzt
        // dort bereits jede andere Streuachse: Position, Pan, Spawn-Timing.
        // Die Tonhoehe davon auszunehmen hiesse, eine Achse still stehen zu
        // lassen, waehrend die anderen drei atmen. Also _color_eff.
        //
        // Gepusht wird in BEIDEN Modi. Die Felder sind in STEP schlicht
        // wirkungslos (_spawn_slice liest sie nicht); ein Modus-Gate waere
        // Zustand ohne Sicherheitsgewinn.
        _sampler.set_feel(_color);
        _sampler.set_dispersion(_color_eff);
    }
```

Der vorhandene Kommentarblock oberhalb des `if` (der erklärt, warum FEEL den rohen Wert liest und dass COLOR in FLOW nichts tut) beschreibt jetzt einen überholten Zustand — der letzte Absatz sagt „COLOR does nothing at all in FLOW here". Das stimmt ab dieser Zeile nicht mehr. Streiche diesen Absatz; der Rest des Blocks bleibt.

- [ ] **Step 4: Den Header wieder wahr machen**

`engine/sampler/sampler_engine.h`, der Kommentar an `sub()` / `detune()` sagt heute sinngemäß: *"SUB and DTUN no longer reach the sampler (spec 2026-07-21 morphagene-controls); these stay at their silent 0.f defaults. Exposed so tests can pin the disconnection down."*

Der erste Halbsatz bleibt wahr, der zweite wird falsch. Ersetze den Kommentar durch:

```cpp
    // Die REGLER SUB und DTUN erreichen den Sampler nach wie vor nicht (spec
    // 2026-07-21 morphagene-controls) -- sie heissen auf einem Sampler-Deck
    // LEN und ORG, und Part::set_voice_sub/set_voice_detune leiten nur noch
    // an den Synth weiter.
    //
    // Die FELDER dahinter sind seit spec 2026-07-23 (cloud-dispersion) aber
    // sehr wohl erreichbar: COLOR schreibt sie in FLOW ueber set_dispersion.
    // Das ist kein Widerspruch, sondern der Unterschied zwischen Regler und
    // Feld -- wer den ersten Satz als Zusage liest, dass hier immer 0 steht,
    // liegt falsch.
    float sub() const    { return _sub_n; }
    float detune() const { return _detune_n; }
```

- [ ] **Step 5: Den Nachbartest umbenennen**

`tests/test_sampler_part.cpp:442` heißt heute `"sampler part: SUB and DTUN no longer reach the sampler"`. Er bleibt grün (er ruft nie `process()`, also feuert kein Control-Tick), aber Titel und Kommentar werden irreführend. Benenne ihn auf das um, was er tatsächlich pinnt:

```cpp
TEST_CASE("sampler part: the SUB and DTUN knobs reach the synth, not the sampler") {
```

und ergänze am Ende seines Kommentarblocks:

```cpp
    // Cloud-dispersion (spec 2026-07-23): die sampler-seitigen FELDER sind
    // inzwischen ueber COLOR schreibbar. Dieser Fall pinnt weiterhin die
    // KNOEPFE -- er ruft nie process(), also feuert kein Control-Tick und
    // set_dispersion laeuft nie. Genau deshalb misst er noch, was er soll.
```

- [ ] **Step 6: Tests laufen lassen, dann die volle Suite**

```bash
./build/spky_tests -tc="sampler part: COLOR reaches the cloud*,sampler part: COLOR 0 leaves the cloud*,sampler part: the SUB and DTUN knobs*"
./build/spky_tests
```

Erwartet: alle grün, Suite grün bei 585. **Beide Golden Vectors dürfen sich nicht bewegen** — sie treiben die Engine direkt ohne Part, also läuft `set_dispersion` dort nie. Bewegt sich einer: **STOP und melden.**

- [ ] **Step 7: Beweisen, dass der Test trägt**

Ändere den Push testweise auf `_sampler.set_dispersion(_color)` (roh statt geschwungen), baue neu, lass `"sampler part: COLOR reaches the cloud*"` laufen. Erwartet: `CHECK(dhi > dlo + 0.02f)` schlägt fehl, weil sich ohne Swing nichts bewegt. Zurücknehmen, Fehlermeldung in den Bericht.

- [ ] **Step 8: Commit**

```bash
git add engine/parts/part.cpp engine/sampler/sampler_engine.h tests/test_sampler_part.cpp
git commit -m "feat(sampler): COLOR streut die Wolke in FLOW

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 3: Die Zweitbeschriftung wieder entfernen

**Files:**
- Modify: `host/vcv/res/gen_panel.py:397` (`SAMPLER_LBL`) und `:417` (`SAMPLER_RADIAL`)
- Regenerate: `host/vcv/res/Spotymod.svg`, `host/vcv/src/generated_panel.hpp`

**Interfaces:**
- Consumes: nichts aus Task 1 oder 2. Diese Task ist unabhängig und könnte auch zuerst laufen.
- Produces: nichts.

**Was hier passiert und warum:** v2.9.0 hat `("COLOR", "FEEL")` in `SAMPLER_LBL` eingetragen und dafür zusätzlich `"COLOR"` in `SAMPLER_RADIAL` aufgenommen — Letzteres, weil COLORs Beschriftung über `orbit_label` gesetzt wird und der `else`-Zweig in `sampler_texts()` sie sonst mit der zentrierten Default-Mathematik überschrieben hätte. Beides wird jetzt zurückgenommen. Das ist **exakt der v2.9.0-Panel-Diff rückwärts**.

Der Grund ist die Hardware-Vorgabe, nicht Wortklauberei: das Panel soll auf das reale Gerät reduzierbar bleiben, und eine Zweitbeschriftung, die nur in einem von zwei Modi stimmt, ist dort schlechter als gar keine. Die naheliegende Alternative — die Beschriftung im VCV-Panel modus-abhängig umschalten — geht auf Hardware nicht, und ein Modul, das mehr verrät als das Instrument, das es prototypisiert, prüft die falsche Bedienfläche.

- [ ] **Step 1: Beide Einträge entfernen**

`host/vcv/res/gen_panel.py:397`:

```python
SAMPLER_LBL = [("MELODY", "SCAN"), ("SUB", "LEN"), ("DETUNE", "ORG")]
```

`host/vcv/res/gen_panel.py:417` — `"COLOR"` aus dem Set nehmen:

```python
SAMPLER_RADIAL = {"MELODY"}
```

Prüfe beim Editieren die umliegenden Kommentare: v2.9.0 hat drei erklärende Kommentare angepasst (bei `:400`, `:411-413` und `:443`), damit sie zur geänderten Form von `SAMPLER_RADIAL` passen. Die müssen jetzt mitwandern — lies sie und stelle den Stand her, der zur Zweierform passt.

- [ ] **Step 2: Panel neu erzeugen**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach/host/vcv && python res/gen_panel.py && git diff --stat res/ src/generated_panel.hpp
```

Erwartet: `res/Spotymod.svg` und `src/generated_panel.hpp` ändern sich, sonst nichts.

- [ ] **Step 3: Prüfen, dass genau die zwei FEEL-Vorkommen verschwinden**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach && grep -c ">FEEL<" host/vcv/res/Spotymod.svg
git diff host/vcv/res/Spotymod.svg | grep "^[+-]" | grep -v "^[+-][+-]" | head -20
```

Erwartet: `grep -c` liefert 0 (vorher 2, je einmal für Deck A und B). Der Diff darf **nur** Löschungen der beiden FEEL-Textknoten enthalten — COLORs eigene Textknoten müssen unverändert bleiben. Bewegen sie sich, ist der `SAMPLER_RADIAL`-Eintrag falsch zurückgenommen: **STOP und melden.**

- [ ] **Step 4: Volle Suite**

```bash
source env.sh && cmake --build build --target spky_tests && ./build/spky_tests
```

Erwartet: grün. (`tests/test_review_register.cpp` oder ein Panel-Test könnte auf die Beschriftungen schauen — falls etwas rot wird, ist das die Ursache und gehört gemeldet, nicht umgebogen.)

- [ ] **Step 5: Commit**

```bash
git add host/vcv/res/gen_panel.py host/vcv/res/Spotymod.svg host/vcv/src/generated_panel.hpp
git commit -m "feat(panel): COLOR steht wieder ohne Zweitbeschriftung

Eine Zweitbeschriftung, die nur in einem von zwei Modi stimmt, ist auf
Hardware schlechter als gar keine.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 4: Die Dichtemessung und die Hörprobe

**Files:**
- Test: `tests/test_sampler_engine.cpp`
- Create: `host/render/scenarios/sampler_dispersion.json`
- Regenerate: `renders/sampler_dispersion.wav`, `renders/sampler_dispersion.csv` (nicht committen, `/renders/` ist gitignored)

**Interfaces:**
- Consumes: `SamplerEngine::set_dispersion(float)` aus Task 1; die Beobachter `dropped_spawns()`, `active_grains()`, `spawn_count()`; die Szenario-Aktion `set_color` (`host/render/scenario.cpp:113`).
- Produces: nichts.

**Worum es geht:** Die Spec verlangt eine Messung **vor** dem Merge, keine Vertagung. Der reale Preis der Streuung ist nämlich kein CPU-Preis — der Deckel begrenzt die Grain-*Anzahl*, nicht die Länge, und `len_ceil` klemmt ein oktavtiefes Tape-Korn auf 1.25× — sondern ein musikalischer: längere Körner füllen den Pool früher, `dropped_spawns` steigt, und die Wolke wird **dünner**, je weiter COLOR aufgeht. Ein Regler, der „mehr Variation" verspricht und Dichte wegnimmt, ist die Regression, auf die zu achten ist.

**Abweichung von der Spec, bewusst:** die Spec beschreibt das als Render mit Vorher-/Nachher-Vergleich. Ein Test ist dafür strikt besser — er ist deterministisch, läuft in CI und *schlägt fehl*, wenn die Regression später auftritt, statt einmalig beobachtet zu werden. Der Render bleibt zusätzlich, aber als Hörprobe, nicht als Messung.

- [ ] **Step 1: Den Messtest schreiben**

An `tests/test_sampler_engine.cpp` anhängen, hinter die Kurventests aus Task 1:

```cpp
// Der reale Preis der Streuung ist keine Rechenlast, sondern Dichte. Im
// Tape-Modus haengt die Grain-Laenge an der Ratio (lenf = _grain_len / ratio),
// ein oktavtiefes Korn ist also laenger; laengere Koerner fuellen den Pool
// frueher; verworfene Spawns duennen die Wolke aus. Ein Regler, der "mehr
// Variation" verspricht und Dichte wegnimmt, waere die Regression.
//
// Gemessen wird im schlimmsten Eck: Tape an, DENS max (kuerzestes
// Spawn-Intervall, hoechste Dichte). Die Schranke ist bewusst grosszuegig --
// dieser Test ist ein Waechter gegen einen Einbruch, kein Feintuning des
// Klangs. Die tatsaechlichen Zahlen gehoeren in den Bericht und ins Ohr.
TEST_CASE("sampler FLOW: dispersion does not starve the cloud at DENS max in tape") {
    auto measure = [](float dispersion) {
        Rig g;
        g.e.set_flow(true);
        g.e.set_tape_mode(true);
        g.e.set_overlap(1.f);               // DENS max
        g.feed(/*pitch*/0.5f, /*source*/0.f, /*size*/0.5f, /*motion*/0.5f);
        g.e.set_dispersion(dispersion);
        g.render(96);                        // Control-Tick sieht die Werte
        const int dropped0 = g.e.dropped_spawns();
        const int spawned0 = g.e.spawn_count();
        g.render(48000 * 4);                 // vier Sekunden Wolke
        const int dropped = g.e.dropped_spawns() - dropped0;
        const int spawned = g.e.spawn_count()  - spawned0;
        return std::make_pair(dropped, spawned);
    };

    const auto off = measure(0.f);
    const auto on  = measure(1.f);

    REQUIRE(off.second > 100);               // es lief ueberhaupt eine Wolke
    REQUIRE(on.second  > 100);

    MESSAGE("dispersion 0: ", off.second, " spawned, ", off.first, " dropped");
    MESSAGE("dispersion 1: ", on.second,  " spawned, ", on.first,  " dropped");

    // Die Wolke darf am oberen Reglerende nicht einbrechen. Halb so viele
    // gelandete Spawns waere ein hoerbarer Dichteverlust und damit genau die
    // Regression, gegen die dieser Test steht.
    const int landed_off = off.second - off.first;
    const int landed_on  = on.second  - on.first;
    CHECK(landed_on * 2 >= landed_off);
}
```

Lies `Rig`s Definition und die Signatur von `feed(...)` am Anfang der Datei und folge ihnen; falls `set_tape_mode` dort anders angesprochen wird, nimm die vorhandene Schreibweise.

- [ ] **Step 2: Test laufen lassen und die Zahlen notieren**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build --target spky_tests && ./build/spky_tests -tc="sampler FLOW: dispersion does not starve*" -s
```

Erwartet: grün. **Beide `MESSAGE`-Zeilen wörtlich in den Bericht übernehmen** — sie sind das Messergebnis, das die Spec vor dem Merge verlangt, und die Grundlage für die Entscheidung, ob `kSubSpreadMax` nach unten muss.

Fällt der Test durch, ist das kein Testproblem: dann nimmt die Streuung am oberen Reglerende zu viel Dichte weg. **STOP und melden**, mit den Zahlen — die Antwort wäre dann ein kleineres `kSubSpreadMax` oder eine gestauchte obere Zone, und das ist eine Entscheidung des Autors.

- [ ] **Step 3: Das Hörszenario anlegen**

Neu: `host/render/scenarios/sampler_dispersion.json`. `sampler_solo.json` ist als Vorlage gedacht, aber **nicht** als Ort für die Rampe: es fährt zwischen 10 und 28 s bereits MOTION-Rampen und schaltet bei 32 s auf STEP. Eine COLOR-Rampe darüber würde beide Demos verderben — zumal FLOW `_color_eff` liest und MOTION die Streuung dann mitbewegt, was genau die Achse verwischt, die hörbar werden soll.

```json
{
  "_comment": "Cloud-dispersion listening aid (spec 2026-07-23): COLOR streut in FLOW die Tonhoehen. Bleibt die ganzen 40s in FLOW -- kein set_step -- damit nur diese eine Achse laeuft. MOTION steht fest auf 0.35, damit die Wolke lebt, aber den Streuwert nicht selbst mitbewegt (FLOW liest _color_eff, also Knopf PLUS MOTION-Swing). COLOR faehrt in vier Stufen: 0 (unisono, klingt wie vor dieser Aenderung), 0.25 und 0.5 (Verstimmungsbreite laeuft auf, eine Farbe), 0.75 und 1.0 (Oktavanteil kommt dazu -- am oberen Ende SPALTUNG in zwei Lagen, nicht Grundierung).",
  "sample_rate": 48000,
  "bpm": 96,
  "duration_s": 40.0,
  "input_wav": "host/render/scenarios/assets/in_drone.wav",
  "init": [
    { "action": "set_engine", "part": 1, "value": "sampler" },
    { "_comment": "Part A stumm -- nur die Wolke soll zu hoeren sein",
      "action": "set_target_base", "part": 0, "slot": 4, "value": 0.0 },
    { "action": "set_target_active", "part": 0, "slot": 4, "flag": false },
    { "action": "set_target_base", "part": 1, "slot": 4, "value": 1.0 },
    { "action": "set_target_base", "part": 1, "slot": 3, "value": 0.35 },
    { "action": "sampler_feedback", "part": 1, "value": 0.95 },
    { "action": "set_color", "part": 1, "value": 0.0 }
  ],
  "events": [
    { "t": 1.0,  "action": "sampler_record", "part": 1, "flag": true },
    { "t": 9.0,  "action": "sampler_record", "part": 1, "flag": false },
    { "t": 16.0, "action": "set_color", "part": 1, "value": 0.25 },
    { "t": 22.0, "action": "set_color", "part": 1, "value": 0.5 },
    { "t": 28.0, "action": "set_color", "part": 1, "value": 0.75 },
    { "t": 34.0, "action": "set_color", "part": 1, "value": 1.0 }
  ]
}
```

Prüfe vor dem Rendern, dass `set_color` und `sampler_feedback` in `host/render/scenario.cpp`s Aktionstabelle stehen und dass `assets/in_drone.wav` existiert — `sampler_solo.json` nutzt beides, aber verlass dich nicht darauf.

- [ ] **Step 4: Rendern**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build --target render && \
  ./build/render.exe host/render/scenarios/sampler_dispersion.json \
      renders/sampler_dispersion.wav renders/sampler_dispersion.csv
```

Erwartet: schreibt WAV und CSV ohne Fehler. **Die Renders NICHT committen** — `/renders/` ist gitignored, und kein `-f`. Sie bleiben im Working Tree als Hörprobe.

Berichte aus der CSV: die Spalte für aktive Grains des Decks B, gemittelt über die Sekunden vor 16 s (COLOR 0) gegen die nach 34 s (COLOR 1). Das ist dieselbe Frage wie der Test in Step 1, nur am echten Szenario — wenn die zweite Zahl deutlich kleiner ist, dünnt die Wolke hörbar aus.

- [ ] **Step 5: Volle Suite**

```bash
./build/spky_tests
```

Erwartet: grün bei 586. Beide Golden Vectors unberührt.

- [ ] **Step 6: Commit**

```bash
git add tests/test_sampler_engine.cpp host/render/scenarios/sampler_dispersion.json
git commit -m "test(sampler): guard the cloud against thinning out at high dispersion

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

## Nach dem Plan

Für Bastian, zu melden statt zu entscheiden:

1. **Die Hörprobe.** `renders/sampler_dispersion.wav`. Die Frage ist nicht „klingt es gut", sondern: wird die Wolke am oberen Reglerende **reicher oder dünner**? Und ist die Spaltung in zwei Oktavlagen ab 0.75 gewollt — oder wäre eine Grundierung (`kSubSpreadMax` ~0.3) das bessere obere Ende?
2. **`kDispersionKnee = 0.5`** ist gesetzt, nicht gemessen. Erschöpft sich die Verstimmungsbreite zu früh, wandert das Knie nach oben.
3. **Das VCV-Plugin** muss neu gebaut und installiert werden, damit sich das im Rack hören lässt: `cd host/vcv && ./build-local.sh install` — nie von Hand, der System-`g++` ist der ARM-Cross-Compiler.

## Plan Self-Review

**Spec-Abdeckung** — jeder Abschnitt der Spec hat eine Task:

| Spec-Abschnitt | Task |
|---|---|
| Zweizonenkurve, `kDispersionKnee`, `kSubSpreadMax` | 1 |
| COLOR 0 strukturell still | 1 (direkt) + 2 (über Part, mit MOTION) |
| Klemmung des Arguments | 1 |
| `_color_eff` in FLOW, roh in STEP | 2 |
| Die Regel als Kommentar im Code | 2 (Step 3) |
| Header sagt die Unwahrheit | 2 (Step 4) |
| `test_sampler_part` umbenennen | 2 (Step 5) |
| Zweitbeschriftung entfernen | 3 |
| Dichtemessung vor dem Merge | 4 (Step 1–2) |
| Hörszenario | 4 (Step 3–4) |
| Beide Golden Vectors bit-identisch | 1, 2, 3, 4 — jede Task prüft es |
| `set_sub`/`set_detune` bleiben erhalten | Global Constraints + 1 (Header-Kommentar) |
| STEP unberührt | Global Constraints; keine Task fasst `_spawn_slice` an |

**Beim Selbstreview gefunden und geschlossen:** die Spec nennt `sampler_solo.json` als Hörszenario. Das geht nicht — die Datei fährt zwischen 10 und 28 s MOTION-Rampen und schaltet bei 32 s auf STEP, und da FLOW `_color_eff` liest, würde MOTION die Streuung mitbewegen und genau die Achse verwischen, die hörbar werden soll. Task 4 legt stattdessen ein eigenes Szenario an, mit fester MOTION und ohne `set_step`. Die Abweichung ist hier benannt, damit ein Reviewer sie nicht für einen Fehler hält.

**Zweite Abweichung, ebenfalls bewusst:** die Spec beschreibt die Dichtemessung als Render mit Vorher-/Nachher-Vergleich; Task 4 macht daraus zusätzlich einen Test. Ein Test ist deterministisch, läuft in CI und schlägt fehl, wenn die Regression später entsteht — ein einmaliger Render tut das nicht. Der Render bleibt als Hörprobe bestehen.

**Typkonsistenz:** `set_dispersion(float)` heißt in Task 1 und 2 gleich; `kDispersionKnee` und `kSubSpreadMax` sind beide `float` und werden nur in Task 1 definiert, in Task 1 und 4 gelesen; `sub()` / `detune()` / `feel()` sind die bestehenden Beobachter und werden nirgends umbenannt.

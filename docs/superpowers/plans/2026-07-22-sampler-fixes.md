# Sampler Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Die zehn im Review vom 2026-07-22 verifizierten Sampler-Fehler beheben, jeden einzelnen durch einen Test abgesichert, der vor dem Fix rot und danach grün ist.

**Architecture:** Jeder Befund bekommt eine ID (F-01…F-10, K-01…K-05). Jede ID wird zum Präfix eines Testnamens in der doctest-Suite. Eine Abschluss-Task greppt das Register gegen die Testdateien und schlägt fehl, wenn eine ID keinen Test hat — das ist der Mechanismus, der verhindert, dass ein Befund still verschwindet. Die Fixes selbst sind klein und lokal; der Aufwand steckt in den Tests, weil die bestehende Suite alle zehn Fehler übersieht.

**Tech Stack:** C++17, doctest, CMake + Ninja + clang (Desktop-Host), VCV Rack SDK (Plugin-Host), Python 3 (Panel-Generator).

## Global Constraints

- **Build:** immer `source env.sh` vor cmake/ctest. Nie MSVC, nie `g++` direkt (System-`g++` ist der ARM-Cross-Compiler).
  - Konfigurieren: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`
  - Bauen: `cmake --build build --target spky_tests`
  - Laufen: `./build/spky_tests.exe`
- **VCV-Host** ausschließlich über `./build-local.sh` bauen, nie von Hand.
- **Keine Bit-Exaktheits-Gates.** Renders sind Plausibilitätsprüfungen, keine Checksummen.
- **Branch:** `sampler-controls`. Nicht auf `main` committen.
- **Commit-Trailer:** jeder Commit endet mit
  `Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>`
- **By-ear-Werte sind tabu**, solange dieser Plan sie nicht ausdrücklich als Entscheidungspunkt ausweist: `kDefaultFeedback`, `kFiltNeutral`, `kNormSmoothS`, die Resonanz-Klemme 0.9, die `defaultFor()`-Werte des Init-Patches.
- **Alle Tests müssen nach jeder Task grün sein** (`./build/spky_tests.exe`, aktuell 510 Cases). Ein Fix, der einen bestehenden Test rot macht, ist ein Befund und wird im Plan-Register nachgetragen, nicht wegkommentiert.
- **Testnamen beginnen mit der Befund-ID**, exakt in der Form `F-01: …`. Task 12 verlässt sich darauf.

---

## Befund-Register

Das ist die Liste, gegen die am Ende geprüft wird. Sie darf nur wachsen, nie schrumpfen.

| ID | Befund | Ort | Task |
|---|---|---|---|
| F-01 | Clamp in `_update_control` kappt nur positiven Spawn-Jitter → Rate bis +21 % zu schnell | `sampler_engine.cpp:309` | 1 |
| F-02 | Gate-Flanke setzt `_spawn_ctr = 0` auch im FLOW → bis 50× zu viele Spawns bei kleinem DENS | `sampler_engine.cpp:154` | 2 |
| F-03 | Jitter multipliziert nach dem CPU-Boden → Spitzen bis 24 kHz statt zugesicherter 6 kHz | `sampler_engine.cpp:542` | 1 |
| F-04 | `LANE_MOTION` steht im Sampler fest auf 0.5 → ORGANIZE und SCAN ohne jede Wirkung | `part.cpp:_control_tick` | 3 |
| F-05 | `read_linear` bildet `frac` aus dem ungeclampten `frame` → Ausschlag bis 1.2e6 an der Faltkante | `sample_buffer.cpp:228` | 4 |
| F-06 | Feedback knapp unter Unity ohne Sättigung → Pufferpeak ~234 statt ~2.3 | `sample_buffer.cpp:138` | 5 |
| F-07 | MELO-Default an den Extremen → SCAN rast beim ENG-Flip mit −8× rückwärts | `Spotymod.cpp:182,460` | 6 |
| F-08 | `set_recording(true)` im Fade-out verworfen → Loop dauerhaft gekürzt | `sample_buffer.cpp:67` | 7 |
| F-09 | `_off`-Stall bei DENS min + TAPE + SIZE max → Grain gibt minutenlang DC aus | `grain.h:137`, `sampler_engine.cpp:503` | 8 |
| F-10 | Pool-Cap bindet im Tape-Modus schon ab einer Oktave, Kommentar behauptet Extremfall | `sampler_engine.cpp:456-504` | 9 |
| K-01 | `trigger_manual` umgeht `_flatten_for_sampler` → Chord-Töne beim TRIG-Druck | `part.cpp:92` | 10 |
| K-02 | NaN im Aufnahme-Eingang vergiftet den Puffer dauerhaft | `sample_buffer.cpp:142` | 10 |
| K-03 | `sampler_scan()` läuft unbedingt für beide Parts → bis 6000 `std::pow`/s ungenutzt | `Spotymod.cpp:460` | 6 |
| K-04 | 96-kHz-Kommentar faktisch falsch (Hosts allozieren sekundenbasiert) | `sampler_config.h:50-56` | 11 |
| K-05 | `sampler_scan.json` lässt SOURCE-Lane aktiv; Kommentar nennt falschen Overlap | `host/render/scenarios/sampler_scan.json` | 11 |

---

## File Structure

**Engine (Verhalten):**
- `engine/sampler/sampler_engine.h` — neuer privater Helper `_next_interval()`. Verantwortlich für: Spawn-Scheduling-Zustand.
- `engine/sampler/sampler_engine.cpp` — F-01, F-02, F-03, F-09, F-10. Verantwortlich für: Grain-Scheduling und Spawn-Parameter.
- `engine/sampler/sample_buffer.cpp` — F-05, F-06, F-08, K-02. Verantwortlich für: Aufnahme, Overdub, Lesen.
- `engine/sampler/sampler_config.h` — neue Konstanten `kFbSatKnee`, `kGrainLenCeil`; Kommentarkorrektur K-04.
- `engine/parts/part.cpp` — F-04, K-01. Verantwortlich für: die Instrument-Schicht, die entscheidet, was ein Sampler-Deck nicht tut.

**Host:**
- `host/vcv/src/Spotymod.cpp` — F-07, K-03. Verantwortlich für: Knopf→Engine-Verdrahtung.
- `host/render/scenarios/sampler_scan.json` — K-05.

**Tests (jeder Fix bekommt seinen Test in der Datei, die das Subsystem abdeckt):**
- `tests/test_sampler_engine.cpp` — F-01, F-02, F-03, F-09, F-10.
- `tests/test_sample_buffer.cpp` — F-05, F-06, F-08, K-02.
- `tests/test_sampler_part.cpp` — F-04, K-01.
- `tests/test_review_register.cpp` — **neu**, Task 12. Verantwortlich für: nachweisen, dass jede Register-ID einen Test hat.

**Doku:**
- `docs/superpowers/plans/2026-07-22-sampler-fixes.md` — dieser Plan.

---

## Reihenfolge und Abhängigkeiten

Task 1 → 2 → 3 sind die Antwort auf die Ausgangsbeobachtung („feuert zu schnell", „unberechenbar") und bauen aufeinander auf: Task 3 (F-04) verändert die MOTION-Werte, mit denen Task 1 seine Raten misst, deshalb kommt Task 1 zuerst und pinnt die Rate bei explizit gesetztem MOTION statt beim Default.

Tasks 4–11 sind untereinander unabhängig und können in beliebiger Reihenfolge oder parallel laufen. Task 12 muss zuletzt laufen.

**Task 3 und Task 6 enthalten je einen Entscheidungspunkt**, der eine Höraussage verlangt. Beide sind im Task als solche markiert und blockieren die übrigen Tasks nicht.

---

### Task 1: Spawn-Intervall — Jitter-Clamp und CPU-Boden (F-01, F-03)

**Files:**
- Modify: `engine/sampler/sampler_engine.h` (privater Helper, bei den anderen `_`-Methoden ~Zeile 212-216)
- Modify: `engine/sampler/sampler_engine.cpp:304-309` und `:538-544`
- Test: `tests/test_sampler_engine.cpp` (ans Ende anhängen)

**Interfaces:**
- Consumes: nichts aus früheren Tasks.
- Produces: `float SamplerEngine::_next_interval() const` — das effektive, gejitterte und gebodete Spawn-Intervall in Samples. Task 2 verändert denselben Scheduling-Block und muss diesen Namen kennen.

**Hintergrund für den Umsetzer:** `_update_control` kappt `_spawn_ctr` auf `_spawn_every`, um zu verhindern, dass ein Herunterfahren von SIZE eine lange Lücke stehen lässt. Der Countdown enthält aber den Timing-Jitter (`_spawn_every * (1 + _spawn_jitter)`). Ist der Jitter positiv, liegt der Countdown über `_spawn_every` und wird binnen 96 Samples zurückgeschnitten; ist er negativ, bleibt er. Der Jitter kann dadurch nur beschleunigen. Getrennt davon multipliziert der Jitter **nach** dem `kSpawnMinSamples`-Boden, der damit nicht hält, was `sampler_config.h:73` verspricht. Beides verschwindet, wenn Clamp und Countdown dieselbe Zahl benutzen.

- [ ] **Step 1: Die beiden fehlschlagenden Tests schreiben**

Ans Ende von `tests/test_sampler_engine.cpp` anhängen:

```cpp
// --- Review 2026-07-22: Spawn-Rate und CPU-Boden ---

TEST_CASE("F-01: the spawn rate matches nominal even with timing jitter") {
    // Der Timing-Jitter ist symmetrisch gezogen (Rng::next_bipolar), also
    // muss er die MITTLERE Rate unangetastet lassen. Vor dem Fix kappte der
    // Clamp in _update_control nur die zu LANGEN Intervalle, womit die Rate
    // bei MOTION = 1 um rund 20 % zu hoch lag.
    //
    // MOTION wird hier ausdruecklich gesetzt statt auf dem Lane-Default zu
    // ruhen: F-04 aendert diesen Default, und dieser Test misst den
    // Scheduler, nicht die Lane.
    for (float size : {0.2f, 0.5f, 0.8f}) {
        Rig g;
        g.feed(0.5f, 0.f, size, 1.f, 1.f);      // MOTION = 1: maximaler Jitter
        g.e.set_overlap(1.f);
        g.e.set_flow(true);

        const int kSamples = 48000 * 10;
        g.render(kSamples);

        const float every   = g.e.spawn_interval_samples();
        const float nominal = float(kSamples) / every;
        // Verlorene Spawns zaehlen mit: gemessen wird die RATE des
        // Schedulers, nicht wie viele Slots gerade frei waren.
        const float actual  = float(g.e.spawn_count() + g.e.dropped_spawns());

        INFO("SIZE=" << size << " spawn_every=" << every
             << " nominal=" << nominal << " actual=" << actual);
        CHECK(actual == doctest::Approx(nominal).epsilon(0.03));
    }
}

TEST_CASE("F-03: the CPU floor bounds the spawn rate WITH jitter applied") {
    // sampler_config.h:73 sagt zu, kSpawnMinSamples deckle die Spawn-Rate
    // bei 6 kHz pro Part. Vor dem Fix multiplizierte der Jitter erst NACH
    // dem Boden, sodass real 2-Sample-Intervalle auftraten (24 kHz).
    using namespace spky::sampler_cfg;
    for (float motion : {0.5f, 1.f}) {
        Rig g;
        g.feed(0.5f, 0.f, 0.f, motion, 1.f);    // SIZE 0 -> kuerzestes Grain
        g.e.set_overlap(1.f);                   // overlap 8 -> 48/8 = 6 < Boden
        g.e.set_flow(true);

        int prev = g.e.spawn_count() + g.e.dropped_spawns();
        int last_i = 0, shortest = 1 << 30;
        for (int i = 0; i < 48000 * 5; ++i) {
            float a = 0.f, b = 0.f;
            g.e.process(a, b);
            const int now = g.e.spawn_count() + g.e.dropped_spawns();
            if (now != prev) {
                if (last_i != 0 && i - last_i < shortest) shortest = i - last_i;
                last_i = i;
                prev = now;
            }
        }
        INFO("MOTION=" << motion << " shortest interval=" << shortest);
        CHECK(float(shortest) >= kSpawnMinSamples);
    }
}

TEST_CASE("F-01: a shrinking SIZE still cancels a long pending countdown") {
    // Das ist der Zweck, den der Clamp urspruenglich hatte, und er muss den
    // Fix ueberleben: faehrt SIZE herunter, darf kein Countdown des ALTEN,
    // langen Intervalls stehenbleiben und die Wolke verstummen lassen.
    Rig g;
    g.feed(0.5f, 0.f, 0.9f, 0.f, 1.f);          // langes Grain, kein Jitter
    g.e.set_overlap(0.f);                       // overlap 1 -> Intervall = Grain
    g.e.set_flow(true);
    g.render(96);                               // ein Control-Tick: Spawn laeuft
    const float long_every = g.e.spawn_interval_samples();
    REQUIRE(long_every > 100000.f);

    g.feed(0.5f, 0.f, 0.3f, 0.f, 1.f);          // SIZE faellt drastisch
    g.render(96);
    const float short_every = g.e.spawn_interval_samples();
    REQUIRE(short_every < long_every / 10.f);

    // Innerhalb zweier neuer Intervalle muss wieder gespawnt werden.
    const int before = g.e.spawn_count();
    g.render(int(short_every) * 2 + 96);
    CHECK(g.e.spawn_count() > before);
}
```

- [ ] **Step 2: Tests laufen lassen und Fehlschlag bestätigen**

```bash
source env.sh && cmake --build build --target spky_tests && ./build/spky_tests.exe -tc="F-01*,F-03*" -s
```

Erwartet: `F-01: the spawn rate matches nominal…` FAIL (actual ≈ 479 gegen nominal 400 bei SIZE 0.5), `F-03: …` FAIL (shortest = 2 gegen Boden 8). Der dritte Test (`a shrinking SIZE…`) ist bereits GRÜN — er pinnt bestehendes Verhalten, das der Fix nicht zerstören darf.

- [ ] **Step 3: Den Helper deklarieren**

In `engine/sampler/sampler_engine.h`, in den privaten Block direkt unter `void _update_control();`:

```cpp
    // Das effektive Intervall bis zum naechsten Spawn: Grundintervall mal
    // Timing-Jitter, danach auf kSpawnMinSamples gebodet. Zwei Aufrufer, und
    // dass es DIESELBE Zahl ist, ist der ganze Punkt -- process() zaehlt
    // damit herunter, _update_control clamped dagegen. Rechnete der Clamp mit
    // dem ungejitterten _spawn_every, kappte er jedes zu LANGE Intervall
    // weg und liess jedes zu kurze stehen, womit ein symmetrisch gezogener
    // Jitter die Wolke systematisch beschleunigte (bis +21 % bei MOTION 1).
    // Der Boden gehoert hierher und nicht in spawn_interval(): dort steht er
    // vor dem Jitter, und der Jitter unterlief ihn anschliessend bis auf 2
    // Samples -- das Vierfache der in sampler_config.h zugesagten 6 kHz.
    float _next_interval() const;
```

- [ ] **Step 4: Den Helper definieren**

In `engine/sampler/sampler_engine.cpp`, direkt vor `void SamplerEngine::_update_control() {`:

```cpp
float SamplerEngine::_next_interval() const {
    const float n = _spawn_every * (1.f + _spawn_jitter);
    return n < kSpawnMinSamples ? kSpawnMinSamples : n;
}

```

- [ ] **Step 5: Den Clamp in `_update_control` auf den Helper umstellen**

In `engine/sampler/sampler_engine.cpp` ersetzen:

```cpp
    // A shrinking interval must not leave a stale long countdown pending:
    // sweeping SIZE down would otherwise gap the carpet for up to the old
    // interval while grains retire at the new, much shorter length.
    if (_spawn_ctr > _spawn_every) _spawn_ctr = _spawn_every;
```

durch:

```cpp
    // A shrinking interval must not leave a stale long countdown pending:
    // sweeping SIZE down would otherwise gap the carpet for up to the old
    // interval while grains retire at the new, much shorter length.
    //
    // Gegen _next_interval() und NICHT gegen _spawn_every: der laufende
    // Countdown traegt den Timing-Jitter, und ein Clamp auf das ungejitterte
    // Grundintervall schneidet jedes zu lange Intervall weg, waehrend jedes
    // zu kurze stehenbleibt. Aus einem symmetrischen Jitter wird so eine
    // einseitige Beschleunigung. _next_interval() folgt SIZE- und
    // DENS-Aenderungen ueber _spawn_every weiterhin sofort, der Zweck dieser
    // Zeile bleibt also erhalten.
    const float ceiling = _next_interval();
    if (_spawn_ctr > ceiling) _spawn_ctr = ceiling;
```

- [ ] **Step 6: Den Countdown in `process()` auf den Helper umstellen**

In `engine/sampler/sampler_engine.cpp` ersetzen:

```cpp
        _spawn_ctr -= 1.f;
        if (_spawn_ctr <= 0.f) {
            _spawn_one();
            _spawn_ctr += _spawn_every * (1.f + _spawn_jitter);
            if (_spawn_ctr < 1.f) _spawn_ctr = 1.f;
        }
```

durch:

```cpp
        _spawn_ctr -= 1.f;
        if (_spawn_ctr <= 0.f) {
            _spawn_one();                    // zieht _spawn_jitter neu
            // _next_interval() bodet bereits auf kSpawnMinSamples, und
            // _spawn_ctr ist an dieser Stelle > -1, also bleibt die Summe
            // sicher positiv -- die alte `if (_spawn_ctr < 1.f)`-Klemme war
            // genau die Stelle, an der der Jitter den CPU-Boden unterlief.
            _spawn_ctr += _next_interval();
        }
```

- [ ] **Step 7: Tests laufen lassen und Erfolg bestätigen**

```bash
source env.sh && cmake --build build --target spky_tests && ./build/spky_tests.exe -tc="F-01*,F-03*" -s
```

Erwartet: alle drei PASS.

- [ ] **Step 8: Die ganze Suite laufen lassen**

```bash
./build/spky_tests.exe
```

Erwartet: `Status: SUCCESS!`, 513 Cases. Besonders im Blick: `sampler: the spawn interval never falls below its floor` und `sampler: FLOW is a standing cloud -- RMS never drops out` müssen grün bleiben.

- [ ] **Step 9: Commit**

```bash
git add engine/sampler/sampler_engine.h engine/sampler/sampler_engine.cpp tests/test_sampler_engine.cpp
git commit -m "fix(sampler): the spawn jitter no longer only accelerates (F-01, F-03)

Der Clamp in _update_control rechnete gegen das ungejitterte
_spawn_every und schnitt damit jedes zu lange Intervall weg, waehrend
jedes zu kurze stehenblieb -- aus einem symmetrisch gezogenen Jitter
wurde eine einseitige Beschleunigung von bis zu +21 % bei MOTION 1.
Derselbe Ausdruck unterlief den kSpawnMinSamples-Boden bis auf 2
Samples (24 kHz statt der zugesagten 6 kHz), weil der Jitter erst nach
dem Boden multiplizierte.

Beide Aufrufer teilen sich jetzt _next_interval().

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 2: Gate-Flanken re-phasen den FLOW nicht mehr (F-02)

**Files:**
- Modify: `engine/sampler/sampler_engine.cpp:141-156`
- Test: `tests/test_sampler_engine.cpp` (ans Ende anhängen)

**Interfaces:**
- Consumes: `SamplerEngine::_next_interval()` aus Task 1 (nur indirekt — dieser Task fasst den Countdown nicht an).
- Produces: nichts, worauf spätere Tasks aufbauen.

**Hintergrund für den Umsetzer:** `set_gate(true)` setzt `_spawn_ctr = 0`, damit ein STEP-Burst auf der Flanke beginnt statt bis zu ein Intervall später. Im FLOW läuft der Scheduler aber ohnehin, und `Part::process` liefert dort trotzdem eine steigende Flanke pro PITCH-Zyklus (`part.cpp:226-229` setzt `_gate_ctr` ohne STEP-Prüfung). Jede dieser Flanken erzwingt einen Sofort-Spawn. Bei SIZE 1.0 und DENS min ist das Grundintervall 42 s — gemessen wurden 1 Spawn in 10 s ohne Flanken gegen 50 mit einer Flanke alle 2 s. Die untere DENS-Hälfte ist damit wirkungslos.

- [ ] **Step 1: Die fehlschlagenden Tests schreiben**

Ans Ende von `tests/test_sampler_engine.cpp` anhängen:

```cpp
TEST_CASE("F-02: a gate edge does not re-phase the FLOW scheduler") {
    // Im FLOW laeuft der Scheduler ohnehin. _spawn_ctr = 0 auf der Flanke
    // erzwingt dort einen Sofort-Spawn, und Part liefert eine Flanke pro
    // PITCH-Zyklus auch im FLOW -- bei langem SIZE und kleinem DENS spawnt
    // die Wolke dadurch im Phrasenrhythmus statt nach DENS.
    Rig g;
    g.feed(0.5f, 0.f, 1.0f, 0.f, 1.f);          // SIZE max
    g.e.set_overlap(0.f);                       // DENS min -> overlap 1
    g.e.set_flow(true);
    g.render(96);
    const float every = g.e.spawn_interval_samples();
    REQUIRE(every > 48000.f * 20.f);            // ~42 s Grundintervall

    // Fuenf Gate-Flanken, eine alle 9600 Samples (0.2 s), wie ein PITCH-Zyklus
    // sie liefert -- zusammen 1 s, also ein Vierzigstel des 42-s-Intervalls.
    // Jede Flanke, die durchkaeme, waere ein Spawn, den DENS nicht bestellt hat.
    for (int k = 0; k < 5; ++k) {
        g.e.set_gate(true);  g.render(240);
        g.e.set_gate(false); g.render(9360);
    }
    const int total = g.e.spawn_count() + g.e.dropped_spawns();
    INFO("spawn_every=" << every << " total spawns in 1 s=" << total);
    CHECK(total <= 2);                          // der Anfangsspawn, sonst nichts
}

TEST_CASE("F-02: a gate edge still starts a STEP burst immediately") {
    // Die Gegenrichtung, und der Grund, warum _spawn_ctr = 0 ueberhaupt da
    // ist: ausserhalb des FLOW muss die Flanke sofort feuern, damit STEP die
    // komponierte Rhythmik trifft statt bis zu ein Intervall spaeter.
    Rig g;
    g.feed(0.5f, 0.f, 0.5f, 0.f, 1.f);
    g.e.set_overlap(0.f);                       // langes Intervall
    g.e.set_flow(false);
    g.render(4800);                             // Scheduler steht (kein Gate)
    const int before = g.e.spawn_count() + g.e.dropped_spawns();

    g.e.set_gate(true);
    g.render(4);                                // wenige Samples nach der Flanke
    const int after = g.e.spawn_count() + g.e.dropped_spawns();
    INFO("before=" << before << " after=" << after);
    CHECK(after > before);
}
```

- [ ] **Step 2: Tests laufen lassen und Fehlschlag bestätigen**

```bash
source env.sh && cmake --build build --target spky_tests && ./build/spky_tests.exe -tc="F-02*" -s
```

Erwartet: `F-02: a gate edge does not re-phase the FLOW scheduler` FAIL mit total ≈ 6. Der zweite Test ist bereits GRÜN und schützt das Verhalten, das erhalten bleiben muss.

- [ ] **Step 3: Den Fix schreiben**

In `engine/sampler/sampler_engine.cpp` in `set_gate` ersetzen:

```cpp
    } else {
        // Start the burst on the edge, not up to _spawn_every samples late:
        // leaving FLOW mid-cycle (or a prior STEP burst) can leave _spawn_ctr
        // anywhere in [0, _spawn_every), and STEP is supposed to reproduce
        // the phrase generator's composed rhythm exactly.
        _spawn_ctr = 0.f;
    }
```

durch:

```cpp
    } else if (!_flow) {
        // Start the burst on the edge, not up to _spawn_every samples late:
        // leaving FLOW mid-cycle (or a prior STEP burst) can leave _spawn_ctr
        // anywhere in [0, _spawn_every), and STEP is supposed to reproduce
        // the phrase generator's composed rhythm exactly.
        //
        // Nur ausserhalb des FLOW. Im FLOW laeuft der Scheduler bereits, und
        // Part liefert dort trotzdem eine steigende Flanke pro PITCH-Zyklus
        // (part.cpp:226-229 setzt _gate_ctr ohne STEP-Pruefung). Jede davon
        // erzwang einen Sofort-Spawn und haengte die Wolkendichte an den
        // Phrasenrhythmus statt an DENS: bei SIZE 1.0 / DENS min sind das 50
        // Spawns in 10 s gegen den einen, den das 42-s-Intervall vorsieht.
        // Die untere DENS-Haelfte war dadurch praktisch wirkungslos.
        _spawn_ctr = 0.f;
    }
```

- [ ] **Step 4: Tests laufen lassen und Erfolg bestätigen**

```bash
source env.sh && cmake --build build --target spky_tests && ./build/spky_tests.exe -tc="F-02*" -s
```

Erwartet: beide PASS.

- [ ] **Step 5: Die ganze Suite laufen lassen**

```bash
./build/spky_tests.exe
```

Erwartet: `Status: SUCCESS!`. Besonders im Blick: `part: the composed gate reaches the sampler in STEP` (`tests/test_sampler_part.cpp:163`) und `sampler: STEP …`-Cases müssen grün bleiben.

- [ ] **Step 6: Commit**

```bash
git add engine/sampler/sampler_engine.cpp tests/test_sampler_engine.cpp
git commit -m "fix(sampler): a gate edge re-phases STEP, not the running FLOW cloud (F-02)

Im FLOW laeuft der Scheduler ohnehin, und Part liefert dort trotzdem
eine steigende Flanke pro PITCH-Zyklus. Jede davon setzte _spawn_ctr
auf 0 und erzwang einen Sofort-Spawn: bei SIZE 1.0 und DENS min 50
Spawns in 10 s statt des einen, den das 42-s-Intervall vorsieht. Die
untere DENS-Haelfte war damit wirkungslos.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 3: MOTION pinnt den Sampler nicht mehr auf halben Scatter (F-04)

**Files:**
- Modify: `engine/parts/part.cpp` (in `_control_tick`, direkt nach der `_tg[LANE_PITCH]`-Zeile)
- Test: `tests/test_sampler_part.cpp` (ans Ende anhängen)

**Interfaces:**
- Consumes: nichts.
- Produces: nichts. Verändert aber den Wert, den `SamplerEngine` als `_targets[LANE_MOTION]` sieht — Tests späterer Tasks, die MOTION brauchen, müssen es explizit über `Instrument::set_depth` (MOD) anfordern.

> **ENTSCHEIDUNGSPUNKT — braucht eine Höraussage von Bastian.**
> Der Fix nimmt dem Sampler-Deck den permanenten Positions-, Timing- und Pan-Scatter, den es heute unabänderlich hat. Die Textur wird dadurch hörbar anders: fokussierter, weniger Nebel. Zwei Varianten stehen zur Wahl:
>
> **(a) Empfohlen — Basis 0, MOD bringt den Scatter zurück.** `_tg[LANE_MOTION]` verliert für Sampler-Decks nur den Basisanteil 0.5; die Lane-Modulation bleibt und schiebt bei MOD > 0 nach oben (0…0.7). ORGANIZE und SCAN wirken bei MOD = 0 exakt, und MOD wird zum MOTION-Regler des Decks. Kein Panel-Eingriff, respektiert den Hardware-Constraint.
>
> **(b) Scatter ganz entfernen.** `_tg[LANE_MOTION] = 0.f` hart. Einfacher, aber MOD verliert im Sampler eine Wirkung, und der Nebel ist nicht mehr erreichbar.
>
> Der Plan setzt (a) um. Wird nach dem Hörtest in Step 7 (b) bevorzugt, ist es eine Ein-Zeilen-Änderung; der Test in Step 1 gilt für beide Varianten unverändert, weil er bei MOD = 0 misst.

**Hintergrund für den Umsetzer:** `part.h:263-265` setzt `_base[LANE_MOTION] = 0.5f`, und weder Host noch Instrument schreiben diese Basis je. `SuperModulator::set_range` trifft nur `LANE_PITCH`, die Texturlanes behalten also `_range = 1`. Bei MOD = 0 ist `_targets[LANE_MOTION]` damit exakt und unabänderlich 0.5. In `_spawn_one` ist `jitter = next_bipolar() * motion * kScatterPosFrac * content` dann gleichverteilt über ein Intervall der Breite genau `content` — und `(SOURCE·span + _scan_pos + jitter) mod content` ist mathematisch exakt gleichverteilt, unabhängig von SOURCE und `_scan_pos`. Gemessen: Mittelwert der Spawn-Position 12036 / 11896 / 11951 bei SOURCE 0 / 0.25 / 0.9 (content 24000), Histogramm flach.

Wichtig: COLOR (`cmod`) und DENS (`omod`) in `_control_tick` lesen `_mod.lane_output(LANE_MOTION)` **direkt**, nicht `_tg[LANE_MOTION]`. Der Fix an `_tg` berührt sie deshalb nicht. Das ist beabsichtigt und der Grund, warum er dort und nicht an der Basis ansetzt.

- [ ] **Step 1: Den fehlschlagenden Test schreiben**

Ans Ende von `tests/test_sampler_part.cpp` anhängen:

```cpp
// --- Review 2026-07-22: MOTION pinnt den Sampler nicht mehr ---

TEST_CASE("F-04: ORGANIZE reaches the spawn position on a sampler deck") {
    // LANE_MOTION hat die Basis 0.5, und niemand schreibt sie -- weder Host
    // noch Instrument. Der Positions-Scatter ist damit +-content und die
    // Spawn-Position exakt gleichverteilt, egal was ORGANIZE sagt. Der Test
    // misst bei MOD = 0, wo gar kein Scatter sein darf.
    //
    // A single last_spawn_pos() read after the render is not enough: pre-fix,
    // that position is uniform over the whole buffer (kSFrames = 48000), and
    // the pass window here is Approx(want).epsilon(0.02), i.e. roughly
    // +-864 around want ~= 43199 -- about 3.6% of the 48000-wide range. A
    // uniform draw lands inside a 3.6%-wide window by pure chance about 1
    // time in 28, so a single sample is nowhere near enough to tell "fixed"
    // from "unfixed": it would pass on the broken code about as often as it
    // fails on the fixed one, purely on which RNG seed the run happens to
    // draw. That is exactly what happened during Task 3 -- this case only
    // went red because that particular seed happened to land outside the
    // window; a different seed would have shipped the bug silently.
    //
    // Collecting every spawn across the whole render and requiring ALL of
    // them inside the window (the same shape the MOD sibling below uses to
    // track lo/hi) closes that hole: with the fix, MOD = 0 means every spawn
    // reads the exact same centre (no scatter at all), so every one of them
    // must land in the window. A still-uniform (unfixed) distribution would
    // need EVERY collected spawn to land in the same 3.6% sliver to slip
    // past -- for more than a handful of spawns that is not a realistic
    // false pass, unlike the single-draw version above. (Measured: neutralizing
    // the fix and re-running this test failed 155 of 161 assertions -- only 6
    // spawns happened to land inside the window by chance, consistent with
    // the ~3.6% prediction.)
    //
    // A bare Part, not InstRig: this needs sampler().spawn_count(), the
    // engine's own cumulative counter (already used by the punch() test
    // above), to detect each real spawn. last_spawn_pos() itself cannot do
    // that job here -- it holds its value between spawns, and WITH the fix
    // MOD = 0 means every spawn reads the exact same centre (no scatter at
    // all, which is the whole point). So consecutive spawns are bit-identical
    // and a "value changed" test would see only the very first one and then
    // go quiet, undercounting real spawns down to one -- checked empirically
    // while building this test. spawn_count() has no such blind spot: it
    // increments once per spawn regardless of whether the position moved.
    std::vector<SampleBuffer::Frame> sbuf(kSFrames, SampleBuffer::Frame{ 0.f, 0.f });
    Part p;
    p.init(48000.f, 0, nullptr, nullptr, sbuf.data(), sbuf.size());
    p.set_engine(ENGINE_SAMPLER);
    p.set_depth(0.f);                    // MOD = 0

    std::vector<float> l(kSFrames), r(kSFrames);
    for (size_t i = 0; i < kSFrames; ++i) {
        l[i] = std::sin(6.2831853f * 220.f * float(i) / 48000.f);
        r[i] = l[i];
    }
    p.sampler().load_sample(l.data(), r.data(), kSFrames);

    // ORGANIZE ans obere Ende: alle Spawns muessen dort landen.
    p.set_target_base(LANE_SOURCE, 0.9f);

    int last_count = p.sampler().spawn_count();
    std::vector<float> spawns;
    for (int i = 0; i < 48000 * 4; ++i) {
        float a = 0.f, b = 0.f;
        p.process(a, b);
        const int count = p.sampler().spawn_count();
        if (count != last_count) {
            last_count = count;
            spawns.push_back(p.sampler().last_spawn_pos());
        }
    }

    // Guard against passing vacuously: if the deck never actually spawned
    // (a broken rig, an engine that silently stayed on ENGINE_SYNTH, an empty
    // buffer, etc.) `spawns` would be empty and every CHECK below would
    // trivially pass by never running. Four seconds of render at these grain
    // settings produces well over a thousand spawns (measured while building
    // this test); 20 is a generous floor that only trips if spawning itself
    // is broken, not if the count merely varies with grain-length settings.
    REQUIRE(spawns.size() > 20);

    const float want = 0.9f * float(kSFrames - 1);
    for (float pos : spawns) {
        INFO("spawn pos=" << pos << " want~" << want);
        CHECK(pos == doctest::Approx(want).epsilon(0.02));
    }
}

TEST_CASE("F-04: MOD brings the sampler's scatter back") {
    // Die Gegenprobe: der Nebel muss erreichbar bleiben, sonst hat der Fix
    // eine Klangfarbe entfernt statt sie steuerbar zu machen.
    InstRig g;
    const int p = 0;
    g.inst.set_engine(p, ENGINE_SAMPLER);
    g.inst.set_depth(p, 1.f);                   // MOD = 1

    std::vector<float> l(kSFrames), r(kSFrames);
    for (size_t i = 0; i < kSFrames; ++i) {
        l[i] = std::sin(6.2831853f * 220.f * float(i) / 48000.f);
        r[i] = l[i];
    }
    g.inst.load_sample(p, l.data(), r.data(), kSFrames);
    g.inst.set_target_base(p, spky::LANE_SOURCE, 0.9f);

    // Ueber viele Spawns hinweg muss die Position wandern.
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 48000 * 4; ++i) {
        g.render(1);
        const float pos = g.inst.sampler_last_spawn_pos(p);
        if (pos < lo) lo = pos;
        if (pos > hi) hi = pos;
    }
    INFO("spawn position range " << lo << " .. " << hi);
    CHECK(hi - lo > 0.2f * float(kSFrames));
}
```

- [ ] **Step 2: Test laufen lassen und Fehlschlag bestätigen**

```bash
source env.sh && cmake --build build --target spky_tests && ./build/spky_tests.exe -tc="F-04*" -s
```

Erwartet: `F-04: ORGANIZE reaches the spawn position…` FAIL, und zwar auf der überwiegenden Mehrheit der gesammelten Spawns (nicht nur einer einzelnen Messung) — gemessen beim Neutralisieren des Fixes: 155 von 161 Assertions schlagen fehl, nur 6 Spawns landeten zufällig im Toleranzfenster. `F-04: MOD brings the sampler's scatter back` ist bereits GRÜN.

- [ ] **Step 3: Den Fix schreiben**

In `engine/parts/part.cpp`, in `_control_tick`, direkt **nach** der Zeile

```cpp
    _tg[LANE_PITCH] = clampf(_pitch_q + _detune_cents * (1.f / 3600.f), 0.f, 1.f);
```

einfügen:

```cpp
    // MOTION's Scatter startet auf einem Sampler-Deck bei null, nicht bei der
    // Lane-Basis 0.5. Dieselbe Schicht und dieselbe Begruendung wie
    // _flatten_for_sampler und die abgeschaltete PITCH-Lane: die INSTRUMENT-
    // Schicht entscheidet, was ein Sampler-Deck nicht tut.
    //
    // Der Grund ist messbar, nicht aesthetisch. Die Basis 0.5 schreibt
    // niemand -- weder Host noch Instrument -- und SuperModulator::set_range
    // trifft nur LANE_PITCH, die Texturlanes behalten also _range = 1. Bei
    // MOD = 0 stand _targets[LANE_MOTION] damit unabaenderlich auf 0.5, und
    // in SamplerEngine::_spawn_one ist der Positions-Jitter dann
    // gleichverteilt ueber ein Intervall der Breite GENAU content. Damit ist
    // (SOURCE*span + _scan_pos + jitter) mod content exakt gleichverteilt,
    // unabhaengig von beiden Summanden: ORGANIZE und SCAN hatten auf die
    // Spawn-Position nachweislich null Effekt (gemessen: Mittelwert 12036 /
    // 11896 / 11951 bei SOURCE 0 / 0.25 / 0.9 ueber content 24000).
    //
    // Nur der Basisanteil faellt weg, die Lane-Modulation bleibt: bei MOD > 0
    // schiebt sie von 0 nach oben, MOD wird also zum MOTION-Regler des Decks
    // und der Nebel bleibt erreichbar.
    //
    // Bewusst an _tg und nicht an _base: COLOR (cmod) und DENS (omod) unten
    // lesen _mod.lane_output(LANE_MOTION) direkt und bleiben davon unberuehrt.
    if (_engine_id == ENGINE_SAMPLER) {
        const float mmod = _active[LANE_MOTION]
            ? _mod.lane_output(LANE_MOTION) * _depth * _tdepth[LANE_MOTION]
            : 0.f;
        _tg[LANE_MOTION] = clampf(mmod, 0.f, 1.f);
    }
```

- [ ] **Step 4: Tests laufen lassen und Erfolg bestätigen**

```bash
source env.sh && cmake --build build --target spky_tests && ./build/spky_tests.exe -tc="F-04*" -s
```

Erwartet: beide PASS.

- [ ] **Step 5: Die ganze Suite laufen lassen**

```bash
./build/spky_tests.exe
```

Erwartet: `Status: SUCCESS!`. Besonders im Blick: `sampler part: the MOTION lane breathes the grain overlap` (`test_sampler_part.cpp:323`) und `sampler part: an inactive MOTION target leaves the overlap on the knob` (`:348`) — beide betreffen `omod`, nicht `_tg`, und müssen grün bleiben. Bleiben sie es nicht, ist die Annahme aus dem Hintergrund-Abschnitt falsch und der Fix gehört revidiert, nicht der Test.

- [ ] **Step 6: Commit**

```bash
git add engine/parts/part.cpp tests/test_sampler_part.cpp
git commit -m "fix(part): MOTION's scatter starts at zero on a sampler deck (F-04)

Die Lane-Basis 0.5 schreibt niemand, und SuperModulator::set_range
trifft nur LANE_PITCH. Bei MOD = 0 stand _targets[LANE_MOTION] damit
unabaenderlich auf 0.5 -- der Positions-Jitter war gleichverteilt ueber
ein Intervall der Breite genau content, womit die Spawn-Position exakt
gleichverteilt wurde, unabhaengig von ORGANIZE und SCAN. Beide Knoepfe
hatten nachweislich null Effekt.

Nur der Basisanteil faellt weg; MOD schiebt den Scatter von 0 nach oben.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

- [ ] **Step 7: Hörprobe für den Entscheidungspunkt**

```bash
source env.sh && cmake --build build --target render
./build/render.exe host/render/scenarios/sampler_scan.json /tmp/scan_after.wav
```

`/tmp/scan_after.wav` anhören und gegen die Erinnerung an das alte Verhalten stellen. Die Frage an Bastian: reicht Variante (a), oder soll der Scatter ganz weg (b)? Antwort im Plan unter diesem Step notieren, dann weiter.

---

### Task 4: `read_linear` faltet ohne Knall (F-05)

**Files:**
- Modify: `engine/sampler/sample_buffer.cpp:220-228`
- Test: `tests/test_sample_buffer.cpp` (ans Ende anhängen)

**Interfaces:**
- Consumes: nichts. Produces: nichts.

**Hintergrund für den Umsetzer:** Nach dem Fold wird `i0` gegen `>= _size` abgesichert und auf 0 gesetzt, `frac` aber danach aus dem **ungeclampten** `frame` gebildet. Ein leicht negatives `frame` faltet in float32 auf exakt `fsz` (weil `fsz - epsilon` bei großem `fsz` auf `fsz` rundet), dann ist `i0 = 0` und `frac = fsz`. Gemessen an einem Puffer, der nur ±0.3 enthält: Ausgang 14 400 bei 24 000 Frames, 1 209 600 bei 2 016 000 Frames. Erreichbar über REVERSE-Grains, sobald `_start + _off` unter 0 läuft (`grain.h:111` subtrahiert `_ratio` im Reverse).

- [ ] **Step 1: Den fehlschlagenden Test schreiben**

Ans Ende von `tests/test_sample_buffer.cpp` anhängen:

```cpp
// --- Review 2026-07-22: Faltkante ---

TEST_CASE("F-05: read_linear stays in range at the negative fold seam") {
    // frac wurde aus dem ungeclampten frame gebildet: ein knapp negatives
    // frame faltet in float32 auf exakt fsz, dann ist i0 = 0 aber frac = fsz
    // -- eine Interpolation mit dem Faktor der Puffergroesse. Erreichbar
    // ueber REVERSE-Grains, sobald _start + _off unter 0 laeuft.
    for (size_t sz : {size_t(4800), size_t(24000), size_t(2016000)}) {
        std::vector<SampleBuffer::Frame> mem(sz);
        for (size_t i = 0; i < sz; ++i) {
            mem[i].l = (i % 2) ? 0.3f : -0.3f;
            mem[i].r = mem[i].l;
        }
        SampleBuffer b;
        b.init(mem.data(), sz, 48000.f);
        b.set_rec_size(sz);

        const float fsz = static_cast<float>(sz);
        float worst = 0.f, worst_at = 0.f;
        auto probe = [&](float frame) {
            float o0 = 0.f, o1 = 0.f;
            b.read_linear(frame, o0, o1);
            if (std::fabs(o0) > std::fabs(worst)) { worst = o0; worst_at = frame; }
        };

        // Die negative Naht: das ist der Weg, auf dem der Fehler real
        // auftrat (REVERSE-Grain laeuft unter 0).
        for (int k = 1; k <= 4000; ++k) probe(-float(k) * 0.0001f);

        // Die positive Naht, obwohl dort heute nichts schiefgeht. Die
        // Faltung ist auf dieser Seite eine Subtraktion nahezu gleich
        // grosser Zahlen und nach Sterbenz exakt, waehrend sie auf der
        // negativen Seite eine verlustbehaftete Addition ist -- der Fehler
        // ist also von Natur aus einseitig. Der Guard deckt trotzdem beide
        // Kanten ab, und ohne diese Proben wuerde niemand merken, wenn
        // jemand ihn spaeter auf `if (frame < 0.f)` zurueck vereinfacht:
        // die negativen Proben blieben gruen, und der alte Knall waere
        // fuer jeden kuenftigen Aufrufer wieder offen.
        probe(fsz);
        probe(2.f * fsz);
        probe(std::nextafter(fsz, 0.f));
        probe(fsz - 0.5f);

        INFO("size=" << sz << " worst=" << worst << " at frame=" << worst_at);
        // Lineare Interpolation zwischen Werten aus [-0.3, 0.3] kann diesen
        // Bereich nicht verlassen. Kleine Toleranz fuer Float-Rundung.
        CHECK(std::fabs(worst) <= 0.31f);
    }
}
```

- [ ] **Step 2: Test laufen lassen und Fehlschlag bestätigen**

```bash
source env.sh && cmake --build build --target spky_tests && ./build/spky_tests.exe -tc="F-05*" -s
```

Erwartet: FAIL mit worst ≈ 14 400 (size 24000) bzw. ≈ 1.2e6 (size 2016000).

- [ ] **Step 3: Den Fix schreiben**

In `engine/sampler/sample_buffer.cpp` ersetzen:

```cpp
    frame -= fsz * std::floor(frame / fsz);
    if (frame < 0.f) frame = 0.f;         // -0.0 and rounding at the seam

    size_t i0 = static_cast<size_t>(frame);
    if (i0 >= _size) i0 = 0;                     // float edge at fsz - epsilon
    size_t i1 = i0 + 1;
    if (i1 >= _size) i1 = 0;

    const float frac = frame - static_cast<float>(i0);
```

durch:

```cpp
    frame -= fsz * std::floor(frame / fsz);
    // BEIDE Kanten hier, vor i0 und frac, und nicht nur i0 spaeter: bei
    // grossem fsz rundet fsz - epsilon in float32 auf exakt fsz, was ein
    // knapp negatives frame genau hierher bringt. Wurde nur i0 korrigiert,
    // blieb frac = fsz stehen und die Interpolation lief mit dem Faktor der
    // Puffergroesse -- an einem Puffer mit nur +-0.3 Inhalt gemessene
    // Ausgaenge von 14 400 (24 000 Frames) bis 1 209 600 (2 016 000 Frames).
    // Erreichbar ueber REVERSE-Grains, sobald _start + _off unter 0 laeuft
    // (grain.h:111). frame und i0 muessen dieselbe Zahl beschreiben.
    if (!(frame >= 0.f) || frame >= fsz) frame = 0.f;

    size_t i0 = static_cast<size_t>(frame);
    if (i0 >= _size) i0 = 0;                     // Guertel zum Hosentraeger
    size_t i1 = i0 + 1;
    if (i1 >= _size) i1 = 0;

    const float frac = frame - static_cast<float>(i0);
```

- [ ] **Step 4: Test laufen lassen und Erfolg bestätigen**

```bash
source env.sh && cmake --build build --target spky_tests && ./build/spky_tests.exe -tc="F-05*" -s
```

Erwartet: PASS.

- [ ] **Step 5: Die ganze Suite laufen lassen**

```bash
./build/spky_tests.exe
```

Erwartet: `Status: SUCCESS!`. Besonders im Blick: alle `read_linear`- und Fold-Cases in `test_sample_buffer.cpp`.

- [ ] **Step 6: Commit**

```bash
git add engine/sampler/sample_buffer.cpp tests/test_sample_buffer.cpp
git commit -m "fix(sampler): read_linear clamps frame before frac, not after (F-05)

Ein knapp negatives frame faltet in float32 auf exakt fsz. i0 wurde
korrigiert, frac aber aus dem ungeclampten frame gebildet -- die
Interpolation lief dann mit dem Faktor der Puffergroesse. An einem
Puffer mit nur +-0.3 Inhalt gemessen: Ausgang 14 400 bei 24 000
Frames, 1 209 600 bei 2 016 000. Erreichbar ueber REVERSE-Grains.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 5: Feedback sättigt vor Unity, nicht erst darüber (F-06)

**Files:**
- Modify: `engine/sampler/sampler_config.h` (neue Konstante, unter `kFbMaxDb`)
- Modify: `engine/sampler/sample_buffer.cpp:132-141`
- Test: `tests/test_sample_buffer.cpp` (ans Ende anhängen)

**Interfaces:**
- Consumes: nichts.
- Produces: `spky::sampler_cfg::kFbSatKnee` — die Koeffizientenschwelle, ab der `fast_tanh` greift.

> **ENTSCHEIDUNGSPUNKT — berührt einen by-ear-Bereich. Gemessen entschieden, siehe unten.**
> Die Sättigung färbt, und wo sie einsetzt ist eine Klangfrage. Gemessen wurde der Pufferpeak nach 30 s Overdub eines 0.5-Signals über den Knopfweg 0.88…1.00:
>
> | Knopf | ohne Fix | Knee 0.98 | Knee 0.90 | tanh unbedingt |
> |---|---|---|---|---|
> | 0.95 = Auslieferungs-Default | 2.74 | 2.74 | 2.74 | **1.18** |
> | höchste Spitze | 132.2 @ 0.9705 | 16.79 @ 0.9675 | 4.16 @ 0.9575 | keine |
> | Anschlag 1.0 | 1.756 | 1.756 | 1.756 | 1.756 |
> | Inversionsfaktor | 75 | 9.6 | 2.4 | 1.0 |
>
> Der Anschlag ist überall dieselbe Zahl, weil der Koeffizient dort 1.33 beträgt und über jeder Schwelle liegt — alle Varianten laufen an dieser Stelle durch denselben Code.
>
> **Umgesetzt ist Knee 0.90.** Es schlägt 0.98 ohne Gegenleistung — halb so hohe Spitze bei gleicher Bauart, und der Default bleibt bei beiden unberührt (0.95 bildet auf ~0.817 ab, unter jeder der beiden Schwellen). Es macht außerdem die Testschranke ableitbar statt willkürlich: `0.5/(1−0.9) = 5`, und der Test prüft diese Schranke zusätzlich gegen eine feste Obergrenze von 6, damit eine Rückkehr zu 0.98 (Schranke 25) sofort auffällt.
>
> **Offen für Bastians Ohr:** eine Restunstetigkeit bleibt. Direkt unter der Schwelle steht der ungesättigte Fixpunkt bei 5, direkt darüber fängt tanh bei ~1.3 — ein Sprung um Faktor 3.2 gegen Faktor 75 vorher. Ganz verschwindet sie nur mit unbedingtem tanh, und das kostet den Auslieferungs-Default 57 % seines Pegels. Das ist eine Hörentscheidung, keine technische.

**Hintergrund für den Umsetzer:** `fast_tanh` greift nur bei `_feedback > 1.f`. Direkt darunter ist der Overdub ein unbegrenzter Integrator mit Fixpunkt `in/(1-fb)`. Gemessen nach 60 s Overdub eines 0.5-Signals: Knob 0.9700 → Peak ~87, Knob 0.9705 → 234 (asymptotisch ~579), Knob 0.9710 → 2.3 (tanh greift), Knob 1.0 → 2.31. Das lauteste erreichbare Verhalten liegt also in einem ~0.001 breiten Fenster **unterhalb** des Anschlags, und das Überschreiten von Unity macht den Puffer um Faktor ~100 leiser. `sampler_config.h:22-25` verspricht dort „ein Loop, der ewig steht" — er wächst.

- [ ] **Step 1: Den fehlschlagenden Test schreiben**

Ans Ende von `tests/test_sample_buffer.cpp` anhängen:

```cpp
// --- Review 2026-07-22: Feedback-Saettigung ---

TEST_CASE("F-06: no feedback setting lets the buffer grow without bound") {
    // Die Saettigung hing an _feedback > 1. Direkt darunter war der Overdub
    // ein unbegrenzter Integrator mit Fixpunkt in/(1-fb): Knob 0.9705 gab
    // nach 60 s Peak 234, waehrend Knob 1.0 bei 2.31 blieb -- das lauteste
    // Verhalten lag in einem 0.001 breiten Fenster UNTER dem Anschlag.
    constexpr size_t kSz = 4800;
    for (int step = 0; step <= 40; ++step) {
        const float knob = 0.90f + 0.0025f * float(step);   // 0.90 .. 1.00
        std::vector<SampleBuffer::Frame> mem(kSz);
        SampleBuffer b;
        b.init(mem.data(), kSz, 48000.f);
        b.set_feedback(knob);
        b.set_recording(true);
        for (int i = 0; i < 48000 * 60; ++i) b.write(0.5f, 0.5f);

        float peak = 0.f;
        for (size_t i = 0; i < kSz; ++i) {
            const float a = std::fabs(mem[i].l);
            if (a > peak) peak = a;
        }
        INFO("knob=" << knob << " peak=" << peak);
        CHECK(peak < 5.f);
    }
}

TEST_CASE("F-06: the shipped feedback default is untouched by the saturation knee") {
    // kDefaultFeedback = 0.95 ist ein by-ear-Wert. Der Test haelt fest, dass
    // die Schwelle ihn nicht erreicht -- wer sie spaeter verschiebt, sieht
    // hier sofort, ob er in gehoerte Einstellungen greift.
    using namespace spky::sampler_cfg;
    constexpr size_t kSz = 4800;
    std::vector<SampleBuffer::Frame> mem(kSz);
    SampleBuffer b;
    b.init(mem.data(), kSz, 48000.f);
    b.set_feedback(kDefaultFeedback);
    b.set_recording(true);
    for (int i = 0; i < 48000 * 10; ++i) b.write(0.5f, 0.5f);

    float peak = 0.f;
    for (size_t i = 0; i < kSz; ++i) {
        const float a = std::fabs(mem[i].l);
        if (a > peak) peak = a;
    }
    // Ohne Saettigung: Fixpunkt 0.5/(1-0.817) = 2.73. Die Schwelle darf
    // diesen Wert nicht nach unten druecken.
    INFO("default feedback peak=" << peak);
    CHECK(peak > 2.f);
    CHECK(peak < 5.f);
}
```

- [ ] **Step 2: Tests laufen lassen und Fehlschlag bestätigen**

```bash
source env.sh && cmake --build build --target spky_tests && ./build/spky_tests.exe -tc="F-06*" -s
```

Erwartet: `F-06: no feedback setting lets the buffer grow without bound` FAIL bei knob ≈ 0.9700–0.9725 mit peak weit über 5. Der zweite Test ist bereits GRÜN.

- [ ] **Step 3: Die Konstante anlegen**

In `engine/sampler/sampler_config.h`, direkt unter `constexpr float kFbMaxDb = 2.5f;` (der Kommentar trägt die vollständige Messreihe — siehe die Datei im Repo für den finalen Wortlaut):

```cpp
// Ab welchem KOEFFIZIENTEN (nicht Knopfwert) der Overdub in fast_tanh
// laeuft. Frueher war das implizit 1.0, und genau dort lag der Fehler: der
// Overdub ist ein Integrator mit Fixpunkt in/(1-fb), der knapp UNTER Unity
// unbegrenzt waechst, waehrend er ueber Unity von tanh gefangen wird. Nach
// 60 s Overdub eines 0.5-Signals gemessen: Knob 0.9700 -> Peak 87,
// 0.9705 -> 234 (asymptotisch ~579), 0.9710 -> 2.3, 1.0 -> 2.31. Das
// lauteste Verhalten des Geraets lag damit in einem ~0.001 breiten Fenster
// unterhalb des Anschlags.
//
// 0.98 laesst den Auslieferungs-Default in Ruhe: kDefaultFeedback = 0.95
// bildet ueber die kFbKnee-Kennlinie auf ~0.817 ab, weit darunter. Der
// Klang aendert sich nur im Fenster 0.98 .. 1.0, das vorher der Ausreisser
// war. Ear-tunable, aber nach oben durch 1.0 gebunden -- darueber ist der
// Befund wieder offen.
constexpr float  kFbSatKnee = 0.90f;
```

- [ ] **Step 4: Den Fix schreiben**

In `engine/sampler/sample_buffer.cpp` ersetzen:

```cpp
    if (_feedback > 1.f) {
        f.l = fast_tanh(f.l);
        f.r = fast_tanh(f.r);
    }
```

durch:

```cpp
    if (_feedback > sampler_cfg::kFbSatKnee) {
        f.l = fast_tanh(f.l);
        f.r = fast_tanh(f.r);
    }
```

Und im Kommentarblock darüber den letzten Satz ersetzen:

```cpp
    // Saturate what was read back BEFORE the feedback multiply -- the order
    // that keeps EchoDelay::Process stable at its 1.2 coefficient
    // (engine/fx/flux.h:129-141). Saturating after the multiply would not
    // bound the write. Only above unity: fast_tanh compresses audibly from
    // about half scale up, so running it unconditionally would give every
    // overdub a tape character it does not have today.
```

durch:

```cpp
    // Saturate what was read back BEFORE the feedback multiply -- the order
    // that keeps EchoDelay::Process stable at its 1.2 coefficient
    // (engine/fx/flux.h:129-141). Saturating after the multiply would not
    // bound the write. Nicht unbedingt: fast_tanh komprimiert ab etwa halber
    // Aussteuerung hoerbar, und jeder Overdub bekaeme sonst einen
    // Tape-Charakter, den er heute nicht hat.
    //
    // Die Schwelle liegt bei kFbSatKnee und NICHT bei 1.0. An 1.0 gebunden
    // war der Bereich knapp darunter voellig unbegrenzt -- ein Integrator
    // mit Fixpunkt in/(1-fb) -- und lauter als jede Einstellung darueber.
```

- [ ] **Step 5: Tests laufen lassen und Erfolg bestätigen**

```bash
source env.sh && cmake --build build --target spky_tests && ./build/spky_tests.exe -tc="F-06*" -s
```

Erwartet: beide PASS.

- [ ] **Step 6: Die ganze Suite laufen lassen**

```bash
./build/spky_tests.exe
```

Erwartet: `Status: SUCCESS!`. Besonders im Blick: `sampler: maximum record bloom into maximum resonance` (`test_sampler_engine.cpp:1430`) und die Bloom-Cases in `test_sample_buffer.cpp:285,346` — sie messen bei Knob 1.0 und 0.9 und dürfen sich nicht verschieben.

- [ ] **Step 7: Commit**

```bash
git add engine/sampler/sampler_config.h engine/sampler/sample_buffer.cpp tests/test_sample_buffer.cpp
git commit -m "fix(sampler): the overdub saturates before unity, not after it (F-06)

fast_tanh hing an _feedback > 1. Direkt darunter war der Overdub ein
unbegrenzter Integrator mit Fixpunkt in/(1-fb): nach 60 s gab Knob
0.9705 einen Pufferpeak von 234, Knob 1.0 dagegen 2.31. Das lauteste
Verhalten des Geraets lag in einem 0.001 breiten Fenster unter dem
Anschlag, und das Ueberschreiten von Unity machte es 100x leiser.

kDefaultFeedback = 0.95 (~0.817) bleibt unberuehrt.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 6: SCAN rastet beim ENG-Flip ein (F-07, K-03)

**Files:**
- Modify: `host/vcv/src/Spotymod.cpp:458-461`
- Modify: `host/vcv/src/Spotymod.cpp` (Member-Deklaration bei den anderen Per-Part-Zuständen, in der Nähe von `smp[]`/`principleIdx[]`)

**Interfaces:**
- Consumes: nichts.
- Produces: nichts.

> **ENTSCHEIDUNGSPUNKT — braucht eine Entscheidung von Bastian.**
> Es gibt zwei Wege, und der Plan setzt (b) um, weil (a) einen by-ear-Wert anfasst:
>
> **(a) MELO-Defaults ändern.** `defaultFor()` liefert `MELODY_A = -0.728`, `MELODY_B = -1.0`. Diese Werte stammen aus dem `init.vcvm`-Snapshot und sind im Synth die VARIATION-Einstellung des Init-Patches. Sie zu ändern heißt, das Init-Patch neu by-ear zu setzen.
>
> **(b) Empfohlen — Soft-Takeover.** SCAN bleibt nach einem ENG-Flip auf 0, bis MELO einmal bewegt wurde. Das ist das übliche Verhalten für umgewidmete Knöpfe, greift kein Init-Patch an und löst das Problem unabhängig davon, welchen Wert der Knopf gerade hat.

**Hintergrund für den Umsetzer:** MELO trägt im Sampler SCAN. Mit `MELODY_B = -1.0` liefert `scan_rate()` den Linearast bis `kScanMaxRate`, also −8× Realtime rückwärts; `MELODY_A = -0.728` liegt im Exponentialast bei ~−0.81×. Beim ersten ENG-Flip auf Sampler lädt der Host im selben Control-Tick die Factory-Drone und der Lesekopf rast sofort los, ohne einen einzigen Nutzer-Gestus. Getrennt davon (K-03) läuft `sampler_scan()` unbedingt für beide Parts, auch für Synth-Parts — `scan_rate()` enthält im Exponentialast ein `std::pow`, bei `ctrlDiv = 16` also bis zu 6000 Aufrufe/s im Audio-Callback, die niemand nutzt.

**Hinweis:** Diese Task hat keinen doctest-Test, weil `Spotymod.cpp` nicht Teil der Testsuite ist (VCV-Rack-Abhängigkeit). Die Verifikation ist manuell und in Step 4 ausgeschrieben. Task 12 kennt diese Ausnahme und verlangt für F-07 und K-03 stattdessen einen Eintrag in `host/vcv/README.md`.

- [ ] **Step 1: Den Per-Part-Zustand anlegen**

In `host/vcv/src/Spotymod.cpp`, bei den übrigen Per-Part-Membern (dort, wo `principleIdx[PART_COUNT]` steht):

```cpp
    // Soft-Takeover fuer MELO, das im Sampler SCAN traegt. Nach einem
    // ENG-Flip bleibt SCAN auf 0, bis der Knopf einmal bewegt wurde. Ohne
    // das rast der Lesekopf beim ersten Flip sofort los: die Init-Defaults
    // stehen an den Extremen (MELODY_A = -0.728 -> ~-0.81x, MELODY_B = -1.0
    // -> -8x Realtime rueckwaerts), und die Factory-Drone laedt im selben
    // Control-Tick. Die Defaults selbst bleiben unangetastet -- sie sind der
    // VARIATION-Wert des Init-Patches und by-ear gesetzt.
    bool  scanArmed[PART_COUNT]    = { false, false };
    float scanLastKnob[PART_COUNT] = { 0.f, 0.f };
```

- [ ] **Step 2: Den Fix schreiben**

In `host/vcv/src/Spotymod.cpp` ersetzen:

```cpp
            const bool samplerPart = eng2 && !smp[p].testTone;
            inst.sampler_overlap(p, pp(DENSITY_A, p));
            inst.sampler_scan(p, pp(MELODY_A, p));
```

durch:

```cpp
            const bool samplerPart = eng2 && !smp[p].testTone;
            inst.sampler_overlap(p, pp(DENSITY_A, p));

            // SCAN nur fuer Sampler-Parts, und erst nach einer Knopfbewegung.
            //
            // Das "nur fuer Sampler-Parts" ist nicht bloss Kosmetik: set_scan
            // -> scan_rate enthaelt im Exponentialast ein std::pow, und bei
            // ctrlDiv = 16 sind das bis zu 6000 Aufrufe/s im Audio-Callback
            // fuer eine Engine, die niemand hoert (K-03).
            //
            // Das Soft-Takeover deckt F-07 ab: MELO ist im Synth VARIATION
            // und steht im Init-Patch an den Extremen. Ohne diese Sperre
            // laedt der erste ENG-Flip die Factory-Drone und laesst den
            // Lesekopf im selben Control-Tick mit bis zu -8x Realtime
            // rueckwaerts losrasen, ohne dass jemand etwas angefasst hat.
            if (samplerPart) {
                const float scanKnob = pp(MELODY_A, p);
                if (!scanArmed[p]) {
                    if (std::fabs(scanKnob - scanLastKnob[p]) > 1e-4f) scanArmed[p] = true;
                    else                                              inst.sampler_scan(p, 0.f);
                }
                if (scanArmed[p]) inst.sampler_scan(p, scanKnob);
            } else {
                // Beim Verlassen der Sampler-Engine entwaffnen, damit der
                // naechste Flip wieder bei stehendem Kopf beginnt.
                scanArmed[p]    = false;
                scanLastKnob[p] = pp(MELODY_A, p);
            }
```

- [ ] **Step 3: Den VCV-Host bauen**

```bash
cd host/vcv && ./build-local.sh
```

Erwartet: Build ohne Fehler. **Nie** von Hand kompilieren — der System-`g++` ist der ARM-Cross-Compiler und meldet „MinGW not found".

- [ ] **Step 4: Manuell verifizieren**

1. VCV Rack starten, Spotymod neu instanziieren (Init-Zustand).
2. ENG auf Part B auf Sampler klicken. **Erwartet:** die Factory-Drone lädt, der Ring-Punkt steht still. Vor dem Fix rast er rückwärts.
3. MELO an Part B bewegen. **Erwartet:** der Punkt läuft los und folgt dem Knopf.
4. ENG zurück auf Synth, dann wieder auf Sampler. **Erwartet:** der Punkt steht wieder still, bis MELO erneut bewegt wird.

- [ ] **Step 5: Das Verhalten dokumentieren**

In `host/vcv/README.md`, im Abschnitt über die Sampler-Bedienoberfläche, anfügen:

```markdown
**SCAN (MELO) rastet nach einem ENG-Flip ein.** Der Knopf trägt im Synth
VARIATION und im Sampler SCAN, und die Init-Werte stehen für VARIATION an
den Extremen. Ohne Sperre würde der Lesekopf beim ersten Flip auf Sampler
mit bis zu −8× Realtime rückwärts losrasen. SCAN bleibt deshalb auf 0,
bis MELO nach dem Flip einmal bewegt wurde (F-07, Review 2026-07-22).
Getestet wird das von Hand — `Spotymod.cpp` ist nicht Teil der doctest-Suite.
```

- [ ] **Step 6: Commit**

```bash
git add host/vcv/src/Spotymod.cpp host/vcv/README.md
git commit -m "fix(vcv): SCAN stays put until MELO is moved after an ENG flip (F-07, K-03)

MELO traegt im Sampler SCAN, steht im Init-Patch aber an den Extremen
seiner VARIATION-Bedeutung (-0.728 und -1.0). Der erste Flip auf
Sampler liess den Lesekopf deshalb sofort mit bis zu -8x Realtime
rueckwaerts losrasen, waehrend im selben Control-Tick die
Factory-Drone lud. Die Defaults bleiben unangetastet -- sie sind
by-ear gesetzt.

sampler_scan() laeuft ausserdem nicht mehr fuer Synth-Parts: das waren
bis zu 6000 ungenutzte std::pow/s im Audio-Callback.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 7: Punch-in im Fade-out kürzt den Loop nicht mehr (F-08)

**Files:**
- Modify: `engine/sampler/sample_buffer.cpp:67-68`
- Test: `tests/test_sample_buffer.cpp` (ans Ende anhängen)

**Interfaces:**
- Consumes: nichts. Produces: nichts.

**Hintergrund für den Umsetzer:** `set_recording(true)` fällt im Zustand `fadeout` in `case State::fadeout: break;` und wird verworfen. Der Fade-out läuft danach zu Ende und ruft `cut()`, das die Loop-Länge festnagelt. Der VCV-Host vergleicht level-basiert gegen `is_recording()`, das im Fade-out noch `true` liefert — der Re-Arm-Wunsch erreicht den Buffer also gar nicht, und beim nächsten Tick beginnt stattdessen ein Overdub des gekürzten Loops. Gemessen: REC an, 1000 Samples, REC aus, 5 Samples, REC an → `rec_size()` bleibt für immer bei 1000. Der Fade-in-Zähler steht beim Punch-in mitten in der Hann-Kurve; ihn stehen zu lassen und wieder aufwärts zu zählen ist symmetrisch zu dem, was der umgekehrte Weg (`sustain`/`fadein` → `fadeout`) bereits tut.

- [ ] **Step 1: Den fehlschlagenden Test schreiben**

Ans Ende von `tests/test_sample_buffer.cpp` anhängen:

```cpp
// --- Review 2026-07-22: Punch-in im Fade-out ---

TEST_CASE("F-08: a punch-in during the record fade-out resumes the recording") {
    // Der fadeout-Zweig verwarf set_recording(true) stillschweigend. Danach
    // lief der Fade-out zu Ende und cut() nagelte die Loop-Laenge fest -- ein
    // REC-Doppelklick innerhalb von 4 ms kuerzte die Aufnahme dauerhaft.
    constexpr size_t kSz = 48000;
    std::vector<SampleBuffer::Frame> mem(kSz);
    SampleBuffer b;
    b.init(mem.data(), kSz, 48000.f);

    b.set_recording(true);
    for (int i = 0; i < 1000; ++i) b.write(0.5f, 0.5f);
    b.set_recording(false);
    for (int i = 0; i < 5; ++i) b.write(0.5f, 0.5f);   // mitten im Fade-out
    b.set_recording(true);                              // Punch-in
    for (int i = 0; i < 20000; ++i) b.write(0.5f, 0.5f);

    INFO("rec_size=" << b.rec_size() << " recording=" << b.is_recording());
    CHECK(b.is_recording());
    CHECK(b.rec_size() > 1000u);
}

TEST_CASE("F-08: a completed fade-out still cuts the loop") {
    // Die Gegenprobe: wird der Fade-out NICHT unterbrochen, muss er wie
    // bisher zu Ende laufen und die Laenge festnageln.
    constexpr size_t kSz = 48000;
    std::vector<SampleBuffer::Frame> mem(kSz);
    SampleBuffer b;
    b.init(mem.data(), kSz, 48000.f);

    b.set_recording(true);
    for (int i = 0; i < 1000; ++i) b.write(0.5f, 0.5f);
    b.set_recording(false);
    for (int i = 0; i < 1000; ++i) b.write(0.5f, 0.5f);   // Fade-out laeuft aus

    INFO("rec_size=" << b.rec_size() << " recording=" << b.is_recording());
    CHECK_FALSE(b.is_recording());
    CHECK(b.rec_size() == 1000u);
}
```

- [ ] **Step 2: Tests laufen lassen und Fehlschlag bestätigen**

```bash
source env.sh && cmake --build build --target spky_tests && ./build/spky_tests.exe -tc="F-08*" -s
```

Erwartet: `F-08: a punch-in during the record fade-out resumes the recording` FAIL (`recording = false`, `rec_size = 1000`). Der zweite Test ist bereits GRÜN.

- [ ] **Step 3: Den Fix schreiben**

In `engine/sampler/sample_buffer.cpp` ersetzen:

```cpp
        case State::fadeout:
            break;                       // already stopping; ignore
```

durch:

```cpp
        case State::fadeout:
            // Punch-in mitten im Fade-out: zurueck in den Fade-in, mit dem
            // Zaehler, der gerade steht. Das ist symmetrisch zu dem, was der
            // umgekehrte Weg unten schon tut (Stopp im Fade-in uebernimmt den
            // Teilzaehler), und die Hann-Kurve blendet einfach von dem Pegel
            // wieder auf, den sie erreicht hat -- kein Sprung.
            //
            // Vorher wurde der Wunsch verworfen, der Fade-out lief zu Ende
            // und cut() nagelte die Loop-Laenge fest. Der VCV-Host vergleicht
            // level-basiert gegen is_recording(), das im Fade-out noch true
            // liefert, sodass der Re-Arm hier gar nicht mehr ankam und
            // stattdessen ein Overdub des gekuerzten Loops begann: ein
            // REC-Doppelklick innerhalb von 4 ms kuerzte die Aufnahme
            // dauerhaft.
            if (on) _state = State::fadein;
            break;
```

- [ ] **Step 4: Tests laufen lassen und Erfolg bestätigen**

```bash
source env.sh && cmake --build build --target spky_tests && ./build/spky_tests.exe -tc="F-08*" -s
```

Erwartet: beide PASS.

- [ ] **Step 5: Die ganze Suite laufen lassen**

```bash
./build/spky_tests.exe
```

Erwartet: `Status: SUCCESS!`. Besonders im Blick: alle Record-/Cut-Cases in `test_sample_buffer.cpp`, insbesondere der Fall „zwei REC-Toggles in einem Audio-Block" (`_fade_ctr == 0` in `write`).

- [ ] **Step 6: Commit**

```bash
git add engine/sampler/sample_buffer.cpp tests/test_sample_buffer.cpp
git commit -m "fix(sampler): a punch-in during the record fade-out resumes it (F-08)

Der fadeout-Zweig verwarf set_recording(true) stillschweigend. Danach
lief der Fade-out zu Ende und cut() nagelte die Loop-Laenge fest. Der
Host vergleicht level-basiert gegen is_recording(), das im Fade-out
noch true liefert -- der Re-Arm kam nie an, und ein REC-Doppelklick
innerhalb von 4 ms kuerzte die Aufnahme dauerhaft.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 8: Die Grain-Länge bleibt unter der `_off`-Stall-Schranke (F-09)

**Files:**
- Modify: `engine/sampler/sampler_config.h` (neue Konstante, unter `kSpawnMinSamples`)
- Modify: `engine/sampler/sampler_engine.cpp:503-507`
- Modify: `engine/sampler/grain.h:137-142` (Kommentar berichtigen)
- Test: `tests/test_sampler_engine.cpp` (ans Ende anhängen)

**Interfaces:**
- Consumes: nichts.
- Produces: `spky::sampler_cfg::kGrainLenCeil`.

**Hintergrund für den Umsetzer:** `grain.h:137-142` erklärt den `_off`-Stall für unerreichbar und rechnet dafür mit der Pool-Decke `kGrains * _spawn_every = 4 032 000` — eine Zahl, die gegen das damals feste `kOverlap = 8` gilt. Seit DENS zur Laufzeit auf `_overlap = 1` gehen kann, ist `len_ceil = _grain_len * 16 / 1 = 32 256 000`, weit über 2²³ = 8 388 608. Erreichbar mit: ENG Sampler, GENE SIZE 1.0, TAPE an, TUNE 0 (ratio 2⁻⁴ = 0.0625), DENS 0. Dann friert `_off` ein, sobald es die lokale ulp unterschreitet, und das Grain gibt für den Rest seiner ~11 Minuten DC aus. Der Kommentar in `sampler_config.h:76-82` („lowering overlap is safe") gilt fürs Trimmen, übersieht aber genau diese Lockerung.

- [ ] **Step 1: Den fehlschlagenden Test schreiben**

Ans Ende von `tests/test_sampler_engine.cpp` anhängen:

```cpp
TEST_CASE("F-09: grain length stays under the _off stall bound at any DENS") {
    // grain.h begruendet die Stall-Freiheit mit der Pool-Decke bei kOverlap
    // = 8 (4 032 000). Seit DENS zur Laufzeit auf overlap 1 gehen kann, ist
    // die Decke 32 256 000 -- weit ueber 2^23, wo _off in float32 einfriert
    // und das Grain fuer den Rest seiner Lebensdauer DC ausgibt.
    using namespace spky::sampler_cfg;
    Rig g;
    g.feed(0.f, 0.f, 1.0f, 0.f, 1.f);       // PITCH 0 -> ratio 2^-4, SIZE max
    g.e.set_tape_mode(true);                // Tape: lenf = _grain_len / ratio
    g.e.set_overlap(0.f);                   // DENS min -> overlap 1
    g.e.set_flow(true);
    g.render(48000);

    REQUIRE(g.e.spawn_count() > 0);
    INFO("last_spawn_len=" << g.e.last_spawn_len()
         << " ceil=" << kGrainLenCeil);
    CHECK(float(g.e.last_spawn_len()) <= kGrainLenCeil);
}

TEST_CASE("F-09: a stalled grain would emit DC -- the guard keeps it moving") {
    // Der Verhaltenstest hinter der Zahl: das laengste erreichbare Grain
    // darf sein Fenster nicht auf einem konstanten Wert verbringen.
    Rig g;
    g.feed(0.f, 0.f, 1.0f, 0.f, 1.f);
    g.e.set_tape_mode(true);
    g.e.set_overlap(0.f);
    g.e.set_flow(true);
    g.render(4800);                          // ein Grain laeuft an

    // Weit in das Grain hineinrendern und pruefen, dass sich das Signal
    // noch bewegt. Bei eingefrorenem _off ist es exakt konstant.
    auto v = g.render(48000 * 5);
    float lo = 1e9f, hi = -1e9f;
    for (size_t i = v.size() / 2; i < v.size(); ++i) {
        if (v[i] < lo) lo = v[i];
        if (v[i] > hi) hi = v[i];
    }
    INFO("signal range in the second half: " << lo << " .. " << hi);
    CHECK(hi - lo > 1e-4f);
}
```

- [ ] **Step 2: Tests laufen lassen und Fehlschlag bestätigen**

```bash
source env.sh && cmake --build build --target spky_tests && ./build/spky_tests.exe -tc="F-09*" -s
```

Erwartet: der erste Test kompiliert noch nicht (`kGrainLenCeil` fehlt) — das ist der Fehlschlag. Nach Step 3 erneut laufen lassen: dann FAIL mit `last_spawn_len` ≈ 32 256 000.

- [ ] **Step 3: Die Konstante anlegen**

In `engine/sampler/sampler_config.h`, direkt unter dem `kSpawnMinSamples`-Block:

```cpp
// Harte Obergrenze fuer die Grain-Laenge in Ausgangssamples.
//
// Grain haelt die Leseposition als Startframe plus relativen Offset, und
// dieser Offset friert ein, sobald die lokale float32-ulp die Schrittweite
// _ratio erreicht (grain.h). Ab _len >= 2^23 = 8 388 608 ist das fuer die
// erreichbaren Ratios der Fall, und das Grain gibt fuer den Rest seines
// Fensters DC aus.
//
// grain.h hielt das fuer unerreichbar und rechnete dafuer mit der
// Pool-Decke bei kOverlap = 8 (kGrains * _spawn_every = 4 032 000). Seit
// DENS den Overlap zur Laufzeit auf 1 stellen kann, ist dieselbe Decke
// _grain_len * kGrains / 1 = 32 256 000 -- viermal ueber der Schranke.
// Erreichbar mit ENG Sampler, GENE SIZE 1.0, TAPE an, TUNE 0, DENS 0.
//
// 2^22 laesst eine Oktave Reserve zur Stall-Schranke. NICHT ear-tunable:
// das ist eine Float-Grenze, kein Klangwert.
constexpr float  kGrainLenCeil = 4194304.f;   // 2^22
```

- [ ] **Step 4: Den Fix schreiben**

In `engine/sampler/sampler_engine.cpp` ersetzen:

```cpp
    const float len_ceil = _spawn_every * static_cast<float>(kGrains);
    if (lenf > len_ceil) lenf = len_ceil;
```

durch:

```cpp
    const float len_ceil = _spawn_every * static_cast<float>(kGrains);
    if (lenf > len_ceil) lenf = len_ceil;

    // Zweite, absolute Decke. Die Pool-Decke oben skaliert mit 1 / _overlap
    // und wird bei DENS min viermal so gross wie die float32-Schranke, ab
    // der Grain::_off einfriert (kGrainLenCeil). Die Pool-Decke allein
    // reicht also nicht mehr, seit der Overlap zur Laufzeit veraenderlich
    // ist -- was grain.h's Stall-Argument voraussetzt.
    if (lenf > kGrainLenCeil) lenf = kGrainLenCeil;
```

- [ ] **Step 5: Den Kommentar in `grain.h` berichtigen**

In `engine/sampler/grain.h`, im Kommentarblock ab „The read position is kept as a start frame plus a RELATIVE offset", den Satz über die Pool-Decke ersetzen. Aus:

```cpp
    // below the local ulp fails to move it at all and the grain emits DC for
    // its entire window. Both the reachable minimum ratio (~0.0154) and a
```

wird — den nachfolgenden Text unverändert lassen und diesen Absatz davor einfügen:

```cpp
    // ACHTUNG: Die Rechnung unten stammt aus der Zeit, als kOverlap eine
    // Compile-Time-Konstante von 8 war, und die dort genannte Pool-Decke
    // (kGrains * _spawn_every = 4 032 000) gilt nur fuer diesen Overlap.
    // Seit DENS ihn zur Laufzeit auf 1 stellen kann, waere dieselbe Decke
    // 32 256 000 und der Stall wieder erreichbar. Was ihn heute ausschliesst,
    // ist die zusaetzliche absolute Grenze kGrainLenCeil = 2^22 in
    // SamplerEngine::_spawn_one, nicht die Pool-Decke.
```

- [ ] **Step 6: Tests laufen lassen und Erfolg bestätigen**

```bash
source env.sh && cmake --build build --target spky_tests && ./build/spky_tests.exe -tc="F-09*" -s
```

Erwartet: beide PASS.

- [ ] **Step 7: Die ganze Suite laufen lassen**

```bash
./build/spky_tests.exe
```

Erwartet: `Status: SUCCESS!`. Besonders im Blick: `sampler: lowering overlap only loosens the pool ceiling` — dieser Test darf grün bleiben, denn die neue Decke ist zusätzlich, nicht ersetzend. Schlägt er fehl, ist er anzupassen und die Anpassung im Commit zu begründen.

- [ ] **Step 8: Commit**

```bash
git add engine/sampler/sampler_config.h engine/sampler/sampler_engine.cpp engine/sampler/grain.h tests/test_sampler_engine.cpp
git commit -m "fix(sampler): an absolute grain-length ceiling closes the _off stall (F-09)

grain.h erklaert den _off-Stall fuer unerreichbar und rechnet dafuer
mit der Pool-Decke bei kOverlap = 8. Seit DENS den Overlap zur Laufzeit
auf 1 stellen kann, ist dieselbe Decke 32 256 000 statt 4 032 000 --
viermal ueber der 2^23-Schranke, ab der _off in float32 einfriert und
das Grain minutenlang DC ausgibt. Erreichbar mit SIZE 1.0, TAPE, TUNE
0, DENS 0.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 9: Der Pool-Cap sagt, wo er wirklich bindet (F-10)

**Files:**
- Modify: `engine/sampler/sampler_engine.cpp:456-504` (Kommentar)
- Test: `tests/test_sampler_engine.cpp` (ans Ende anhängen)

**Interfaces:**
- Consumes: `kGrainLenCeil` aus Task 8 (nur im Kommentar erwähnt).
- Produces: nichts.

**Hintergrund für den Umsetzer:** Das ist ein Doku-Befund mit einem pinnenden Test, kein Verhaltensfix. Der Cap selbst ist richtig — ein Grain darf die Zeit nicht überleben, in der alle Slots vollaufen. Falsch ist, was der Kommentar über seine Reichweite behauptet: er rechtfertigt ihn mit 45-Minuten-Extremen bei SIZE 1.0, tatsächlich bindet er im Tape-Modus bei DENS max schon ab `ratio = 0.5`, also bei jeder Transposition über eine Oktave abwärts, bei ganz normalem SIZE. `len_ceil = _spawn_every * kGrains = _grain_len * 16 / 8 = 2 * _grain_len`, und Tape gibt `lenf = _grain_len / ratio`. Die zugesagte Tape-Charakteristik („low notes smear long") endet dort still. Voice-Stealing würde das lösen und wird bewusst nicht gemacht.

- [ ] **Step 1: Den pinnenden Test schreiben**

Ans Ende von `tests/test_sampler_engine.cpp` anhängen:

```cpp
TEST_CASE("F-10: the tape ceiling binds at one octave down, not at an extreme") {
    // Kein Verhaltensfix, sondern die Zahl, die der Kommentar am Cap nennen
    // muss. Bei DENS max ist len_ceil = 2 * _grain_len, und Tape gibt
    // lenf = _grain_len / ratio -- die Decke greift also ab ratio = 0.5.
    Rig g;
    g.e.set_tape_mode(true);
    g.e.set_overlap(1.f);                   // DENS max -> overlap 8
    g.e.set_flow(true);

    // Eine halbe Oktave abwaerts: die Tape-Laenge muss noch durchkommen.
    g.feed(0.375f, 0.f, 0.5f, 0.f, 1.f);    // ratio = 8^(0.375-0.5) ~ 0.7715
    g.render(48000);
    const float len_half = float(g.e.last_spawn_len());
    const float base     = g.e.grain_len_samples();
    INFO("half octave: len=" << len_half << " base=" << base);
    CHECK(len_half > 1.2f * base);          // Tape streckt noch

    // Zwei Oktaven abwaerts: die Decke bindet und kappt auf 2 * base.
    Rig g2;
    g2.e.set_tape_mode(true);
    g2.e.set_overlap(1.f);
    g2.e.set_flow(true);
    g2.feed(0.125f, 0.f, 0.5f, 0.f, 1.f);   // ratio ~ 0.125, Tape wollte 8x
    g2.render(48000);
    const float len_deep = float(g2.e.last_spawn_len());
    const float base2    = g2.e.grain_len_samples();
    INFO("two octaves: len=" << len_deep << " base=" << base2);
    CHECK(len_deep == doctest::Approx(2.f * base2).epsilon(0.01));
}
```

- [ ] **Step 2: Test laufen lassen**

```bash
source env.sh && cmake --build build --target spky_tests && ./build/spky_tests.exe -tc="F-10*" -s
```

Erwartet: PASS. Dieser Test dokumentiert bestehendes Verhalten — er ist von Anfang an grün und hält es fest, damit eine spätere Änderung am Cap sichtbar wird.

- [ ] **Step 3: Den Kommentar berichtigen**

In `engine/sampler/sampler_engine.cpp`, im Kommentarblock über `len_ceil`, direkt nach dem Absatz „Without this, tape at the top of SIZE starves the cloud…" einfügen:

```cpp
    // Wo die Decke WIRKLICH bindet, und das ist nicht der Extremfall oben:
    // bei DENS max ist _spawn_every = _grain_len / 8, also len_ceil =
    // 2 * _grain_len. Tape gibt lenf = _grain_len / ratio, und das erreicht
    // die Decke schon bei ratio = 0.5 -- bei JEDER Transposition ueber eine
    // Oktave abwaerts, bei ganz normalem SIZE. Die Tape-Zusage "low notes
    // smear long" endet dort still: ab einer Oktave abwaerts wird das Grain
    // nicht laenger, sondern gekappt (gepinnt von "F-10: the tape ceiling
    // binds at one octave down" in tests/test_sampler_engine.cpp).
    //
    // Bewusst so belassen. Was es loesen wuerde, ist Voice-Stealing -- das
    // aelteste Grain verdraengen statt den Spawn fallenzulassen -- und das
    // ist eine breite Verhaltensaenderung am haeufigen dichten Pfad fuer
    // einen Gewinn am duennen Rand. Siehe den Absatz zum sparse case unten.
```

- [ ] **Step 4: Die ganze Suite laufen lassen**

```bash
source env.sh && cmake --build build --target spky_tests && ./build/spky_tests.exe
```

Erwartet: `Status: SUCCESS!`.

- [ ] **Step 5: Commit**

```bash
git add engine/sampler/sampler_engine.cpp tests/test_sampler_engine.cpp
git commit -m "docs(sampler): the pool ceiling binds at one octave, not at an extreme (F-10)

Der Kommentar rechtfertigte den Cap mit 45-Minuten-Grains bei SIZE 1.0.
Tatsaechlich ist len_ceil bei DENS max genau 2 * _grain_len, und Tape
erreicht das schon bei ratio = 0.5 -- die Tape-Zusage endet ab einer
Oktave abwaerts still. Verhalten unveraendert, jetzt gepinnt und
benannt.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 10: TRIG flattet, und NaN vergiftet den Puffer nicht (K-01, K-02)

**Files:**
- Modify: `engine/parts/part.cpp:92-98`
- Modify: `engine/sampler/sample_buffer.cpp:142-144`
- Test: `tests/test_sampler_part.cpp` (K-01), `tests/test_sample_buffer.cpp` (K-02)

**Interfaces:**
- Consumes: nichts. Produces: nichts.

**Hintergrund für den Umsetzer:** `Part::trigger_manual` baut den Chord und ruft `_engine->trigger_chord(chord, n)` ohne `_flatten_for_sampler`. Bei COLOR > 0 landen dadurch bis zu vier Töne in `SamplerEngine::_chord[]`, bis der nächste `_control_tick` (≤ 96 Samples) über `set_chord` korrigiert — genug für bis zu zwölf Spawns mit Oktavsprüngen beim TRIG-Druck. Getrennt davon prüft der Buffer die Sample-Werte nie auf Endlichkeit: ein einziges NaN im Aufnahmeeingang bleibt für immer, weil der Overdub es wieder einliest, und nur `clear()` heilt.

- [ ] **Step 1: Die fehlschlagenden Tests schreiben**

Ans Ende von `tests/test_sampler_part.cpp` anhängen:

```cpp
TEST_CASE("K-01: trigger_manual flattens the chord on a sampler deck") {
    // trigger_manual ruft trigger_chord ohne _flatten_for_sampler. Bei
    // COLOR > 0 landen bis zu vier Toene in der SamplerEngine, bis der
    // naechste _control_tick korrigiert -- bis zu zwoelf Spawns weit.
    InstRig g;
    const int p = 0;
    g.inst.set_engine(p, ENGINE_SAMPLER);
    g.inst.set_color(p, 1.f);                   // maximaler Chord
    g.inst.set_depth(p, 0.f);

    std::vector<float> l(kSFrames), r(kSFrames);
    for (size_t i = 0; i < kSFrames; ++i) {
        l[i] = std::sin(6.2831853f * 220.f * float(i) / 48000.f);
        r[i] = l[i];
    }
    g.inst.load_sample(p, l.data(), r.data(), kSFrames);
    g.render(4800);

    g.inst.trigger_manual(p);
    // Direkt nach dem Trigger, vor dem naechsten Control-Tick, muessen alle
    // Spawns auf demselben Verhaeltnis landen.
    float first = 0.f;
    bool  have  = false;
    for (int i = 0; i < 90; ++i) {
        g.render(1);
        const float ratio = g.inst.sampler_last_spawn_ratio(p);
        if (!have) { first = ratio; have = true; }
        INFO("i=" << i << " ratio=" << ratio << " first=" << first);
        CHECK(ratio == doctest::Approx(first).epsilon(1e-4));
    }
}
```

Ans Ende von `tests/test_sample_buffer.cpp` anhängen:

```cpp
TEST_CASE("K-02: a NaN in the input does not poison the buffer") {
    // read_linear prueft die POSITION auf Endlichkeit, nie die Samples. Ein
    // einziges NaN blieb fuer immer, weil der Overdub es wieder einliest.
    constexpr size_t kSz = 4800;
    std::vector<SampleBuffer::Frame> mem(kSz);
    SampleBuffer b;
    b.init(mem.data(), kSz, 48000.f);
    b.set_feedback(0.9f);
    b.set_recording(true);

    const float nan_v = std::numeric_limits<float>::quiet_NaN();
    for (int i = 0; i < 500; ++i) b.write(nan_v, nan_v);
    for (int i = 0; i < 20000; ++i) b.write(0.2f, 0.2f);   // sauberer Overdub

    float o0 = 0.f, o1 = 0.f;
    b.read_linear(10.f, o0, o1);
    INFO("read after NaN then clean overdub: " << o0);
    CHECK(o0 == o0);            // kein NaN
    CHECK(std::isfinite(o0));
}
```

`tests/test_sample_buffer.cpp` braucht dafür oben `#include <limits>`, falls noch nicht vorhanden.

- [ ] **Step 2: Tests laufen lassen und Fehlschlag bestätigen**

```bash
source env.sh && cmake --build build --target spky_tests && ./build/spky_tests.exe -tc="K-01*,K-02*" -s
```

Erwartet: beide FAIL. Falls `K-01` grün ist, weil `sampler_last_spawn_ratio` innerhalb der 90 Samples gar nicht wechselt, das Fenster auf 200 Samples erhöhen und erneut prüfen — der Befund ist zeitkritisch.

- [ ] **Step 3: K-01 fixen**

In `engine/parts/part.cpp` ersetzen:

```cpp
void Part::trigger_manual() {
    _gate_ctr = _gate_len;
    float chord[ChordBuilder::kMaxNotes];
    const int n = _chord.build(target_value(LANE_PITCH), _chord_mask(),
                               _quant.root_semis(), chord);
    _engine->trigger_chord(chord, n);
}
```

durch:

```cpp
void Part::trigger_manual() {
    _gate_ctr = _gate_len;
    float chord[ChordBuilder::kMaxNotes];
    const int n = _chord.build(target_value(LANE_PITCH), _chord_mask(),
                               _quant.root_semis(), chord);
    // Durch _flatten_for_sampler, genau wie der Fire-Pfad in process()
    // (part.cpp:270). Ohne das landeten bei COLOR > 0 bis zu vier Toene in
    // der SamplerEngine, bis der naechste _control_tick (<= 96 Samples) ueber
    // set_chord korrigiert -- weit genug fuer rund ein Dutzend Spawns mit
    // Oktavspruengen beim TRIG-Druck, auf einem Deck, das ausdruecklich EINE
    // Tonhoehe halten soll. Auf einer Synth-Part gibt der Helper nch
    // unveraendert zurueck, dort aendert sich also nichts.
    _engine->trigger_chord(chord, _flatten_for_sampler(chord, n));
}
```

- [ ] **Step 4: K-02 fixen**

In `engine/sampler/sample_buffer.cpp` ersetzen:

```cpp
    f.l = in0 * fade + f.l * fb_fade;
    f.r = in1 * fade + f.r * fb_fade;
    _buffer[_write_head] = f;
```

durch:

```cpp
    f.l = in0 * fade + f.l * fb_fade;
    f.r = in1 * fade + f.r * fb_fade;
    // Ein einziges nicht-endliches Sample bliebe sonst fuer immer: der
    // Overdub liest es zurueck, multipliziert und schreibt es wieder, und nur
    // clear() heilt das. In VCV kann es vom Nachbarmodul kommen. Zwei
    // Vergleiche pro Sample, und sie fangen NaN wie Inf (jeder Vergleich mit
    // NaN ist falsch, also greift die Negation).
    if (!(f.l > -1e6f && f.l < 1e6f)) f.l = 0.f;
    if (!(f.r > -1e6f && f.r < 1e6f)) f.r = 0.f;
    _buffer[_write_head] = f;
```

- [ ] **Step 5: Tests laufen lassen und Erfolg bestätigen**

```bash
source env.sh && cmake --build build --target spky_tests && ./build/spky_tests.exe -tc="K-01*,K-02*" -s
```

Erwartet: beide PASS.

- [ ] **Step 6: Die ganze Suite laufen lassen**

```bash
./build/spky_tests.exe
```

Erwartet: `Status: SUCCESS!`. Besonders im Blick: `part: the sampler granulates at ONE pitch whatever COLOR says` (`test_sampler_part.cpp:497`) und alle Bloom-Cases (die 1e6-Grenze darf den absichtlichen Bloom nicht kappen — Task 5 hält ihn unter 5).

- [ ] **Step 7: Commit**

```bash
git add engine/parts/part.cpp engine/sampler/sample_buffer.cpp tests/test_sampler_part.cpp tests/test_sample_buffer.cpp
git commit -m "fix(sampler): TRIG flattens the chord, and a NaN cannot poison the buffer (K-01, K-02)

trigger_manual rief trigger_chord ohne _flatten_for_sampler: bei COLOR
> 0 landeten bis zu vier Toene in der SamplerEngine, bis der naechste
Control-Tick korrigierte -- rund ein Dutzend Spawns weit.

write() prueft ausserdem die Samples auf Endlichkeit. Ein einziges NaN
im Eingang blieb sonst fuer immer, weil der Overdub es zurueckliest.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 11: Kommentare und Listening-Szenarien berichtigen (K-04, K-05)

**Files:**
- Modify: `engine/sampler/sampler_config.h:50-56`
- Modify: `host/render/scenarios/sampler_scan.json`

**Interfaces:**
- Consumes: nichts. Produces: nichts.

**Hintergrund für den Umsetzer:** Der 96-kHz-Absatz behauptet, `kSizeCeilS = 42 s` verlange bei 96 kHz doppelt so viel Kapazität wie vorhanden. Beide Hosts allozieren aber sekundenbasiert (`host/render/main.cpp:23,48` und `host/vcv/src/Spotymod.cpp:105,304` rechnen `42.0 * sample_rate`), die Kapazität ist also bei jeder Rate 42 s und das „genau einmal unter einem Fenster"-Argument bleibt gültig. Der Absatz lädt dazu ein, eine Konstante zu „reparieren", die in Ordnung ist. Ratenabhängig ist etwas anderes: `kRecordFade = 192` ist eine Sample-Zahl, während `_cut` (SoftSwitch) über `init(sample_rate)` echte 4 ms hält — bei 96 kHz laufen die beiden auseinander.

- [ ] **Step 1: Den 96-kHz-Absatz ersetzen**

In `engine/sampler/sampler_config.h` den Absatz ab „That justification is RATE-SPECIFIC" bis „Recorded so nobody re-derives the 42 as rate-independent." ersetzen durch:

```cpp
// Die Rechtfertigung ist ratenspezifisch, die Konstante aber eine DAUER und
// die Kapazitaet folgt ihr: beide Hosts allozieren sekundenbasiert
// (host/render/main.cpp:23,48 und host/vcv/src/Spotymod.cpp:105,304 rechnen
// 42.0 * sample_rate). Der Puffer fasst damit bei jeder Rate 42 s, und das
// "genau einmal unter einem Fenster"-Argument oben bleibt gueltig.
//
// (Eine frueherer Fassung dieses Absatzes behauptete das Gegenteil -- bei
// 96 kHz reiche die Kapazitaet nur fuer die Haelfte. Das war falsch und lud
// dazu ein, eine gesunde Konstante zu reparieren.)
//
// Was bei 96 kHz tatsaechlich auseinanderlaeuft, steht bei kRecordFade: das
// ist eine Sample-Zahl (4 ms bei 48 kHz, 2 ms bei 96 kHz), waehrend _cut
// (SoftSwitch) ueber init(sample_rate) echte 4 ms haelt.
```

- [ ] **Step 2: Den `kRecordFade`-Kommentar präzisieren**

In `engine/sampler/sampler_config.h` ersetzen:

```cpp
// 192 samples == 4 ms @ 48 kHz, and == the hann table size in fx_util.h, so
// the fade counter indexes the curve 1:1. Both facts are load-bearing.
```

durch:

```cpp
// 192 samples == 4 ms @ 48 kHz, and == the hann table size in fx_util.h, so
// the fade counter indexes the curve 1:1. TRAGEND ist die zweite Gleichung:
// die Tabellengroesse. Die 4 ms gelten nur bei 48 kHz -- dies ist eine
// Sample-Zahl, waehrend _cut (SoftSwitch) ueber init(sample_rate) echte 4 ms
// haelt, sodass die beiden bei anderen Raten auseinanderlaufen.
```

- [ ] **Step 3: Das SCAN-Szenario reparieren**

In `host/render/scenarios/sampler_scan.json` die `_comment`-Zeile ersetzen:

```json
  "_comment": "Listening aid for SCAN (spec 2026-07-21 morphagene-controls). One long grain, overlap 1, the playhead walked from frozen to realtime to reverse. Part A is silenced so the deck can be judged alone.",
```

durch:

```json
  "_comment": "Listening aid for SCAN (spec 2026-07-21 morphagene-controls). One long grain, overlap ~3.1 (sampler_overlap 0.3 lerps 1..8), the playhead walked from frozen to realtime to reverse. Part A is silenced so the deck can be judged alone. Slot 0 (SOURCE) is deactivated as well as based at 0: its lane modulation otherwise walks the read position on its own, in the one scenario meant to isolate the walk SCAN produces.",
```

und im `init`-Block direkt nach der Zeile

```json
    { "action": "set_target_base", "part": 1, "slot": 0, "value": 0.0 },
```

einfügen:

```json
    { "action": "set_target_active", "part": 1, "slot": 0, "flag": false },
```

Begründung für den Umsetzer: `_spawn_one` liest `_targets[LANE_SOURCE]` inklusive Lane-Modulation. Die Basis auf 0 zu setzen reicht nicht — nur `set_target_active … false` legt die Modulation still (siehe `Part::target_raw`, `part.cpp:44-49`). Die MOTION-Lane (slot 3) hat im selben Block bereits beides und ist der Beleg für das Muster.

- [ ] **Step 4: Das Szenario rendern und hören**

```bash
source env.sh && cmake --build build --target render
./build/render.exe host/render/scenarios/sampler_scan.json /tmp/scan_fixed.wav
```

Erwartet: kein Fehler, eine Datei entsteht. Anhören: die Leseposition soll erkennbar SCAN folgen statt zu mäandern.

- [ ] **Step 5: Die ganze Suite laufen lassen**

```bash
./build/spky_tests.exe
```

Erwartet: `Status: SUCCESS!`. `tests/test_scenario.cpp` liest die Szenariodateien mit.

- [ ] **Step 6: Commit**

```bash
git add engine/sampler/sampler_config.h host/render/scenarios/sampler_scan.json
git commit -m "docs(sampler): the 96 kHz note was wrong; SCAN's scenario now isolates SCAN (K-04, K-05)

Beide Hosts allozieren sekundenbasiert (42.0 * sample_rate), die
Kapazitaet ist also bei jeder Rate 42 s -- der Absatz lud dazu ein, eine
gesunde Konstante zu reparieren. Ratenabhaengig ist stattdessen
kRecordFade, das als Sample-Zahl neben dem sekundenbasierten _cut steht.

sampler_scan.json liess die SOURCE-Lane aktiv, deren Modulation die
Leseposition im Szenario wandern liess, das genau SCAN isolieren soll.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 12: Das Register gegen die Tests prüfen

**Files:**
- Create: `tests/test_review_register.cpp`
- Modify: `CMakeLists.txt` (die neue Testdatei zum `spky_tests`-Target hinzufügen)

**Interfaces:**
- Consumes: die Testnamen aller vorangegangenen Tasks.
- Produces: nichts.

**Hintergrund für den Umsetzer:** Das ist der Mechanismus, der dem Plan seinen Namen gibt. Ein Register in einer Markdown-Datei schützt vor gar nichts — es muss eine Prüfung geben, die fehlschlägt, wenn ein Befund ohne Test bleibt. doctest kennt seine eigenen Testnamen zur Laufzeit nicht, deshalb wird die Liste hier ausgeschrieben und gegen die Testdateien im Quellbaum geprüft.

- [ ] **Step 1: Die Registerprüfung schreiben**

Neue Datei `tests/test_review_register.cpp`:

```cpp
#include <doctest/doctest.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// Jeder Befund aus dem Review vom 2026-07-22 braucht einen Test, dessen Name
// mit seiner ID beginnt. Diese Pruefung ist der Grund, warum das Register in
// docs/superpowers/plans/2026-07-22-sampler-fixes.md nicht bloss eine Liste
// ist: sie schlaegt fehl, sobald ein Befund ohne Test dasteht -- auch dann,
// wenn jemand einen Test spaeter entfernt.
//
// F-07 und K-03 stehen bewusst NICHT hier: sie liegen in
// host/vcv/src/Spotymod.cpp, das die VCV-Rack-Abhaengigkeit hat und nicht
// Teil dieser Suite ist. Ihre Verifikation ist die manuelle Prozedur in
// host/vcv/README.md, und die zweite Pruefung unten haelt fest, dass sie
// dort auch wirklich beschrieben steht.

namespace {

const char* kSourceFiles[] = {
    "tests/test_sampler_engine.cpp",
    "tests/test_sample_buffer.cpp",
    "tests/test_sampler_part.cpp",
};

// Ohne F-07 und K-03 -- siehe oben.
const char* kIdsNeedingATest[] = {
    "F-01", "F-02", "F-03", "F-04", "F-05",
    "F-06", "F-08", "F-09", "F-10",
    "K-01", "K-02",
};

std::string slurp(const std::string& path) {
    // Der Test laeuft aus dem Build-Verzeichnis; beide Lagen probieren.
    for (const std::string prefix : { std::string(""), std::string("../") }) {
        std::ifstream f(prefix + path);
        if (f) {
            std::ostringstream ss;
            ss << f.rdbuf();
            return ss.str();
        }
    }
    return {};
}

}  // namespace

TEST_CASE("review register: every finding from 2026-07-22 has a test") {
    std::string all;
    for (const char* p : kSourceFiles) {
        const std::string body = slurp(p);
        INFO("reading " << p);
        REQUIRE(!body.empty());          // Datei fehlt oder wurde verschoben
        all += body;
    }

    for (const char* id : kIdsNeedingATest) {
        // Gesucht wird die Form TEST_CASE("F-01: ...
        const std::string needle = std::string("TEST_CASE(\"") + id + ":";
        INFO("no test case named \"" << id << ": ...\" found in the sampler tests");
        CHECK(all.find(needle) != std::string::npos);
    }
}

TEST_CASE("review register: the host-only findings are documented instead") {
    // F-07 und K-03 koennen hier nicht getestet werden. Statt sie stumm
    // fallenzulassen, wird ihre manuelle Verifikationsprozedur eingefordert.
    const std::string readme = slurp("host/vcv/README.md");
    INFO("host/vcv/README.md not found");
    REQUIRE(!readme.empty());
    CHECK(readme.find("F-07") != std::string::npos);
}
```

- [ ] **Step 2: Die Datei ins Build-Target aufnehmen**

In `CMakeLists.txt`, in der Quellenliste von `add_executable(spky_tests …)`, neben den anderen Sampler-Tests einfügen:

```cmake
    tests/test_review_register.cpp
```

- [ ] **Step 3: Bauen und laufen lassen**

```bash
source env.sh && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target spky_tests
./build/spky_tests.exe -tc="review register*" -s
```

Erwartet: beide PASS. Schlägt eine ID fehl, fehlt ihr Test — dann ist die zugehörige Task nicht fertig, und der Fehlschlag ist genau die Warnung, für die diese Prüfung existiert.

- [ ] **Step 4: Die ganze Suite laufen lassen**

```bash
./build/spky_tests.exe
```

Erwartet: `Status: SUCCESS!`.

- [ ] **Step 5: Commit**

```bash
git add tests/test_review_register.cpp CMakeLists.txt
git commit -m "test(sampler): the review register fails if a finding loses its test

Ein Register in einer Markdown-Datei schuetzt vor nichts. Diese Pruefung
liest die Sampler-Testdateien und verlangt fuer jeden Befund aus dem
Review vom 2026-07-22 einen TEST_CASE, dessen Name mit der ID beginnt --
auch dann noch, wenn jemand einen Test spaeter entfernt. Die beiden
host-only-Befunde fordert sie stattdessen in host/vcv/README.md ein.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 13: Abschluss — Hörprobe und Zusammenfassung

**Files:**
- Modify: `docs/superpowers/plans/2026-07-22-sampler-fixes.md` (dieses Dokument, Ergebnisabschnitt)

**Interfaces:**
- Consumes: alles. Produces: nichts.

- [ ] **Step 1: Die Rate am Ende noch einmal messen**

Denselben Messweg fahren, mit dem der Befund gefunden wurde — jetzt mit allen Fixes:

```bash
source env.sh && ./build/spky_tests.exe -tc="F-01*,F-02*,F-03*" -s
```

Erwartet: alle grün, insbesondere die Rate innerhalb ±3 % bei MOTION = 1.

- [ ] **Step 2: Die Listening-Szenarien rendern**

```bash
cmake --build build --target render
./build/render.exe host/render/scenarios/sampler_scan.json    /tmp/final_scan.wav
./build/render.exe host/render/scenarios/sampler_overlap.json /tmp/final_overlap.wav
```

Beide anhören. Erwartet: ORGANIZE und SCAN bewegen die Leseposition hörbar, DENS ändert die Dichte über den ganzen Knopfweg (vor Task 2 tat die untere Hälfte nichts), keine Knackser.

- [ ] **Step 3: Den VCV-Host bauen und von Hand prüfen**

```bash
cd host/vcv && ./build-local.sh
```

Dann die Prozedur aus Task 6 Step 4 durchlaufen.

- [ ] **Step 4: Das Ergebnis in diesen Plan schreiben**

Unter „Ergebnis" (unten) für jede Register-ID eine Zeile eintragen: behoben / bewusst offen gelassen / als Verhalten gepinnt, mit dem Testnamen daneben.

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/plans/2026-07-22-sampler-fixes.md
git commit -m "docs(plans): record the outcome of the sampler fix pass

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

## Ergebnis

*(Von Task 13 auszufüllen.)*

| ID | Status | Test |
|---|---|---|
| F-01 | | |
| F-02 | | |
| F-03 | | |
| F-04 | | |
| F-05 | | |
| F-06 | | |
| F-07 | | |
| F-08 | | |
| F-09 | | |
| F-10 | | |
| K-01 | | |
| K-02 | | |
| K-03 | | |
| K-04 | | |
| K-05 | | |

## Bewusst nicht in diesem Plan

- **Voice-Stealing statt Spawn-Drop** bei vollem Grain-Pool. Würde F-10 wirklich lösen (die Tape-Charakteristik bliebe über eine Oktave hinaus erhalten), ist aber eine breite Verhaltensänderung am häufigen dichten Pfad. Eigener Entwurf, eigene Hörentscheidung.
- **MOTION auf einen eigenen Knopf legen.** Der Hardware-Constraint sagt, das Panel muss auf die reale Hardware reduzierbar bleiben; ein weiterer Knopf geht in die falsche Richtung. Task 3 macht MOD zum MOTION-Regler des Sampler-Decks, was ohne Panel-Eingriff auskommt.
- **`_burst_latched` wird nie zurückgesetzt.** Folge: TUNE-Änderungen sind in STEP zwischen zwei Noten unhörbar, erst die nächste Note übernimmt sie. Im FLOW live. Vermutlich gewollt — aber unbestätigt, deshalb hier notiert statt angefasst.
- **`kRecordFade` ratenunabhängig machen.** Bei 96 kHz laufen Record-Fade (Samples) und `_cut` (Sekunden) auseinander. Kein Klick, kein Fehlverhalten bei 48 kHz; Task 11 benennt es im Kommentar, statt die Konstante anzufassen.
- **Die `±1e9`-Schranke in `read_linear` auf `1e7` senken.** Die Faltung verliert bei `1e8` durch Auslöschung ihre Auflösung, die Schranke dokumentiert also eine Sicherheit, die sie nicht gibt. Nach Task 8 (`kGrainLenCeil`) ist der Bereich unerreichbar; die Verschärfung wäre Gürtel zum Hosenträger.

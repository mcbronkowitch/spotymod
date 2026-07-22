# Sampler: Morphagene-nahe Bedienfläche — Implementierungsplan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Vier im Sampler-Modus wirkungslose Regler bekommen eine dem Grain-Sampler eigene Aufgabe — Tonkopfvorschub, Kornüberlappung, Kornlänge, Leseposition — und die Tonhöhe wird stabil, damit Sampler-Material und Synth in derselben Tonart zusammenklingen.

**Architecture:** Alle Änderungen an der Engine leben in `SamplerEngine`; der Synth-Pfad wird strukturell nicht berührt, weil sampler-eigene Setter dem Bestandsmuster `Instrument` → `_parts[p].sampler()` folgen und `engine_iface.h` unangetastet bleibt. Der laufende Tonkopf ist ein Phasenakkumulator im Kontrolltakt, dessen Position auf das SOURCE-Ziel addiert wird. Die Umdeutung der Regler passiert im Host, nicht in der Engine.

**Tech Stack:** C++17, doctest, CMake + Ninja + clang (Engine); VCV Rack 2 SDK (Plugin); Python 3 (Panel-Generator).

**Spec:** `docs/superpowers/specs/2026-07-21-sampler-morphagene-controls-design.md` — bei jedem Widerspruch zwischen Plan und Spec gilt die Spec, und der Widerspruch gehört gemeldet.

## Global Constraints

- **Synth-Neutralität ist ein Byte-Gatter.** Die acht angehefteten Szenarien müssen byte-identisch bleiben. Keine Änderung darf den Synth-Pfad berühren.
- **Determinismus ist ein Byte-Gatter.** Doppelter Render derselben Szene muss byte-identisch sein.
- **`CMAKE_BUILD_TYPE` muss zwischen verglichenen Läufen übereinstimmen.** Ein Debug/Release-Unterschied erzeugt einen Fehlalarm, der in M5a eine komplette Untersuchung gekostet hat. Der vorhandene `build/`-Ordner steht auf **Debug**.
- **Kein Heap in `engine/`.** Keine Allokation, keine `std::`-Mathematik und kein `assert()` auf dem Pro-Sample-Pfad. `std::pow`, `std::sqrt` und `std::floor` sind im Kontrolltakt erlaubt und dort bereits üblich.
- **`engine/` darf libDaisy nicht einbinden.**
- **`src/` ist eingefrorene Referenz** — niemals ändern.
- **`PART_STRIDE` bleibt 23.** Es kommen **keine** neuen VCV-Parameter hinzu; `PARAM_ORDER` und `LIGHT_ORDER` in `host/vcv/res/test_panel.py` bleiben unverändert.
- **`host/vcv/src/generated_panel.hpp` und `host/vcv/res/Spotymod.svg` niemals von Hand bearbeiten** — beide werden von `host/vcv/res/gen_panel.py` erzeugt.
- **`host/vcv/plugin.json` niemals ändern** (bleibt 2.7.0). **Niemals ein `v*`-Tag anlegen. Niemals pushen.**
- **Niemals `git add -A`** — immer nur die genannten Pfade stagen.
- Commit-Trailer: `Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>`
- **Namenskollision beachten:** `Instrument::set_morph` existiert bereits (globaler A/B-Überblendregler, `Spotymod.cpp:443`). Die neue Kornüberlappung heißt **`sampler_overlap`**, nie `morph`. Die Panel-Beschriftung des DENSITY-Reglers bleibt **`DENS`**.

## Kommandos

```bash
# Engine bauen und testen (aus dem Repo-Wurzelverzeichnis)
cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build --output-on-failure

# Einzelner Testfall
./build/spky_tests.exe -tc="sampler: <Name des Testfalls>"

# Render-Host
./build/render.exe host/render/scenarios/<name>.json out.wav mods.csv

# Panel (aus host/vcv/)
python res/gen_panel.py
python res/test_panel.py
```

## Dateien

| Datei | Verantwortung | Aufgabe |
|---|---|---|
| `engine/sampler/sampler_config.h` | Konstanten für Scan-Kurve und Überlappungsbereich | 1, 2 |
| `engine/sampler/sampler_engine.h` | Deklaration `set_overlap`, `set_scan`, `punch`, neue Member | 1, 2, 3 |
| `engine/sampler/sampler_engine.cpp` | Scan-Akkumulator, Überlappung im Spawn-Intervall, `punch` | 1, 2, 3 |
| `engine/parts/part.h` / `.cpp` | Überlappungsknopf + MOTION-Anteil; SUB/DTUN nicht mehr an den Sampler | 4 |
| `engine/instrument.h` | Weiterleitungen `sampler_overlap`, `sampler_scan`, `sampler_punch` | 4 |
| `host/render/scenario.cpp` | JSON-Aktionen für die drei neuen Setter | 5 |
| `host/render/scenarios/` | Hörszenarien je Regler | 5 |
| `host/vcv/src/Spotymod.cpp` | Reglerumdeutung im Sampler-Modus, `punch` auf NEW/TRIG, Ring-Punkt | 6, 7 |
| `host/vcv/res/gen_panel.py` | Sampler-Beschriftungen als eigene Textzeilen | 7 |
| `host/vcv/res/test_panel.py` | Prüfungen für die neuen Beschriftungen | 7 |
| `host/vcv/README.md` | Dokumentation der Bedienfläche | 8 |
| `tests/test_sampler_engine.cpp` | Engine-Tests | 1, 2, 3 |
| `tests/test_sampler_part.cpp` | Part-/Instrument-Tests | 4 |
| `tests/test_scenario.cpp` | Szenario-Aktionen | 5 |

---

### Task 1: Kornüberlappung wird einstellbar

**Files:**
- Modify: `engine/sampler/sampler_config.h`
- Modify: `engine/sampler/sampler_engine.h:91` (Umfeld), Setter-Block bei `:143-149`, Member-Block bei `:186-190`
- Modify: `engine/sampler/sampler_engine.cpp:64-67` (`spawn_interval`), `:275` (Aufruf)
- Test: `tests/test_sampler_engine.cpp`

**Interfaces:**
- Produces: `void SamplerEngine::set_overlap(float n)` — `n` ist eine Knopfstellung 0..1, abgebildet auf 1.0…8.0 Überlappungen. Aufgerufen von `Part` (Task 4).
- Produces: `float SamplerEngine::overlap() const` — Beobachtung für Tests.

**Kontext:** `kOverlap = 8` (`sampler_engine.h:91`) hat im gesamten Produktivcode **genau eine** ausführende Fundstelle: `sampler_engine.cpp:275`. Alles andere sind Kommentare und Tests. `spawn_interval` nimmt die Überlappung bereits als Parameter entgegen; der Test-Seam `test_spawn_interval(float, int)` wurde ausdrücklich dafür angelegt.

**Wichtig:** `kOverlap` bleibt als Konstante bestehen und wird der Vorgabewert des neuen Members. Bestehende Tests lesen `SamplerEngine::kOverlap` (u. a. `tests/test_sampler_engine.cpp:1284`) und müssen unverändert compilieren.

- [ ] **Step 1: Konstanten ergänzen**

In `engine/sampler/sampler_config.h`, direkt nach `kSpawnMinSamples` (heute Zeile 73):

```cpp
// Overlap range for the DENS knob in the sampler (spec 2026-07-21
// morphagene-controls). The ceiling stays at kGrains / 2 = 8 and does NOT
// rise to kGrains: above 8 the pool-throughput bound
// len_ceil = _spawn_every * kGrains (sampler_engine.cpp) starts trimming
// grain length silently. Downward the bound only gets looser, so lowering
// overlap is safe under all conditions.
constexpr float  kOverlapMin = 1.f;
constexpr float  kOverlapMax = 8.f;
```

- [ ] **Step 2: Den fehlschlagenden Test schreiben**

An das Ende von `tests/test_sampler_engine.cpp` anhängen:

```cpp
TEST_CASE("sampler: DENS sets the grain overlap, and the spawn interval follows") {
    Rig g;
    g.e.set_flow(true);
    g.feed(0.5f, 0.f, 0.5f);          // SIZE 0.5 -> 0.2 s -> 9600 samples
    g.render(200);
    const float len = g.e.grain_len_samples();
    REQUIRE(len == doctest::Approx(9600.f).epsilon(0.05));

    // Knob 1.0 -> overlap 8: the shipped density, unchanged from M5b.
    g.e.set_overlap(1.f);
    g.render(200);
    CHECK(g.e.overlap() == doctest::Approx(8.f));
    CHECK(g.e.spawn_interval_samples() == doctest::Approx(len / 8.f).epsilon(0.01));

    // Knob 0.0 -> overlap 1: one grain at a time, back to back. This is the
    // sparse regime where the ATK/DEC window shape becomes audible at all
    // (measured: 23% crest-factor swing isolated vs 1-16% in the dense cloud).
    g.e.set_overlap(0.f);
    g.render(200);
    CHECK(g.e.overlap() == doctest::Approx(1.f));
    CHECK(g.e.spawn_interval_samples() == doctest::Approx(len).epsilon(0.01));
}

TEST_CASE("sampler: the CPU floor on the spawn interval survives at every overlap") {
    // kSpawnMinSamples caps the spawn RATE, so it must bind hardest at the
    // shortest grain and the highest overlap. 1 ms at 48 kHz is 48 samples;
    // 48 / 8 = 6, which is below the floor of 8.
    CHECK(test_spawn_interval(48.f, 8) == doctest::Approx(sampler_cfg::kSpawnMinSamples));
    CHECK(test_spawn_interval(48.f, 1) == doctest::Approx(48.f));
    CHECK(test_spawn_interval(9600.f, 8) == doctest::Approx(1200.f));
    CHECK(test_spawn_interval(9600.f, 1) == doctest::Approx(9600.f));
}

TEST_CASE("sampler: lowering overlap only loosens the pool ceiling, never tightens it") {
    // len_ceil = _spawn_every * kGrains, and _spawn_every grows as overlap
    // falls -- so a lower overlap can never trim a grain that a higher one
    // would have allowed. This is the invariant that makes DENS safe to turn
    // down under any SIZE.
    const float len = 9600.f;
    float prev = 0.f;
    for (int ov = 8; ov >= 1; --ov) {
        const float ceil_v = test_spawn_interval(len, ov) * float(SamplerEngine::kGrains);
        CHECK(ceil_v >= prev);
        prev = ceil_v;
    }
}
```

- [ ] **Step 3: Test laufen lassen und Fehlschlag bestätigen**

```bash
cmake --build build && ./build/spky_tests.exe -tc="sampler: DENS sets the grain overlap*"
```
Erwartet: Compile-Fehler — `set_overlap`, `overlap` und `spawn_interval_samples` sind keine Member von `SamplerEngine`.

- [ ] **Step 4: Setter und Beobachter deklarieren**

In `engine/sampler/sampler_engine.h`, im Block `// --- edit layer ---` (heute Zeile 138-141), nach `set_feedback`:

```cpp
    // DENS in the sampler: grain overlap, 1..8 (spec 2026-07-21
    // morphagene-controls). n is a knob position 0..1. This is the density
    // control that kOverlap used to fix at compile time; at low overlap the
    // grain window stops being a Constant-OverLap-Add system and ATK/DEC
    // become audible.
    void set_overlap(float n);
```

Im Beobachtungsblock (heute Zeile 151-162), nach `grain_len_samples()`:

```cpp
    float overlap() const               { return _overlap; }
    float spawn_interval_samples() const { return _spawn_every; }
```

Im Member-Block, direkt nach `_spawn_every` (heute Zeile 188):

```cpp
    float _overlap     = static_cast<float>(kOverlap);   // 1..8, DENS
```

- [ ] **Step 5: `spawn_interval` auf float umstellen und den Setter definieren**

In `engine/sampler/sampler_engine.cpp`, die Signatur bei Zeile 64 ändern — **der Test-Seam bleibt unverändert**, weil `int` implizit nach `float` konvertiert:

```cpp
inline float spawn_interval(float grain_len, float overlap) {
    const float raw = grain_len / (overlap > 0.f ? overlap : 1.f);
    return raw < kSpawnMinSamples ? kSpawnMinSamples : raw;
}
```

Der Aufruf bei Zeile 275 wird:

```cpp
    _spawn_every = spawn_interval(_grain_len, _overlap);
```

Und im Setter-Block am Dateiende (nach `set_detune`, heute Zeile 529):

```cpp
void SamplerEngine::set_overlap(float n) {
    _overlap = lerpf(kOverlapMin, kOverlapMax, clampf(n, 0.f, 1.f));
}
```

- [ ] **Step 6: Tests laufen lassen**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```
Erwartet: alle Tests grün, einschließlich der drei neuen und der bestehenden Überlappungstests bei `tests/test_sampler_engine.cpp:1084-1324`.

- [ ] **Step 7: Byte-Gatter prüfen**

Die Synth-Neutralität darf sich nicht bewegt haben — `SamplerEngine` ist nicht am Synth-Pfad beteiligt, aber das ist eine Behauptung, die das Gatter belegen muss und nicht der Implementierer.

```bash
./build/render.exe host/render/scenarios/ctrl_identity.json /tmp/a.wav /tmp/a.csv
sha256sum /tmp/a.wav
```
Erwartet: der Hash stimmt mit `host/render/scenarios/ctrl_identity.sha256` überein. Falls nicht: **NICHT weiterarbeiten**, sondern als BLOCKED melden.

- [ ] **Step 8: Commit**

```bash
git add engine/sampler/sampler_config.h engine/sampler/sampler_engine.h \
        engine/sampler/sampler_engine.cpp tests/test_sampler_engine.cpp
git commit -m "feat(sampler): grain overlap becomes settable (DENS)"
```

---

### Task 2: Laufender Tonkopf

**Files:**
- Modify: `engine/sampler/sampler_config.h`
- Modify: `engine/sampler/sampler_engine.h` (Setter-Block, Beobachtungsblock, Member-Block)
- Modify: `engine/sampler/sampler_engine.cpp` — Kurvenhelfer bei den anderen Helfern (~Zeile 31-46), `_update_control` (`:261-310`), `_spawn_one` (`:336`), `_kill_all` (`:196-204`)
- Test: `tests/test_sampler_engine.cpp`

**Interfaces:**
- Consumes: nichts aus Task 1.
- Produces: `void SamplerEngine::set_scan(float bipolar)` — `bipolar` ist −1…+1, Vorzeichen ist die Richtung. Aufgerufen von `Instrument::sampler_scan` (Task 4).
- Produces: `float SamplerEngine::scan_pos() const` — die akkumulierte Position in Frames, für Tests und den Ring.
- Produces: `float spky::test_scan_rate(float n)` — Test-Seam auf den Kurvenhelfer, nach dem Vorbild von `test_spawn_interval` (`sampler_engine.h:23-30`).

**Kontext:** Die Leseposition wird heute bei **jedem** Spawn frisch aus `_targets[LANE_SOURCE]` berechnet (`sampler_engine.cpp:332-338`); es gibt keinen Zustand, der zwischen Spawns fortgeschrieben wird. `_kill_all()` (`:196-204`) ist der gemeinsame Reset-Punkt, den `init`, `clear` und `load_sample` alle durchlaufen — dort setzt heute schon `_spawn_ctr = 0.f`.

- [ ] **Step 1: Konstanten ergänzen**

In `engine/sampler/sampler_config.h`, nach den Überlappungskonstanten aus Task 1:

```cpp
// SCAN: the running playhead (spec 2026-07-21 morphagene-controls). The knob
// is bipolar; the sign is the direction. The curve is piecewise, mirroring
// the SIZE curve's shape:
//   |n| < kScanDead          -> exactly 0. A real dead zone, so a frozen head
//                               stays frozen under knob noise.
//   kScanDead .. kScanKnee   -> exponential, kScanMinRate .. 1.0x realtime.
//   above kScanKnee          -> linear, 1.0x .. kScanMaxRate.
// Realtime (1.0x) therefore lands on a fixed, findable knob position instead
// of somewhere in the sweep. The top quarter carries the factor 8 and is the
// steepest stretch of the curve -- if it plays too nervously, the fix is an
// exponential top segment, not a smaller range (spec "Nicht in diesem
// Entwurf" / listening notes).
constexpr float  kScanDead    = 0.02f;
constexpr float  kScanKnee    = 0.75f;
constexpr float  kScanMinRate = 0.001f;
constexpr float  kScanMaxRate = 8.f;
```

- [ ] **Step 2: Die fehlschlagenden Tests schreiben**

An `tests/test_sampler_engine.cpp` anhängen:

```cpp
TEST_CASE("sampler: the SCAN curve has a dead centre, hits 1.0x at the knee, tops at 8x") {
    using namespace sampler_cfg;
    // Dead zone: not "small", exactly zero. A creeping playhead at knob
    // centre would make the frozen state unreachable.
    CHECK(test_scan_rate(0.f)      == 0.f);
    CHECK(test_scan_rate(0.019f)   == 0.f);
    CHECK(test_scan_rate(-0.019f)  == 0.f);

    // Knee: realtime, on both sides, exactly.
    CHECK(test_scan_rate(kScanKnee)  == doctest::Approx(1.f).epsilon(0.001));
    CHECK(test_scan_rate(-kScanKnee) == doctest::Approx(-1.f).epsilon(0.001));

    // Ends.
    CHECK(test_scan_rate(1.f)  == doctest::Approx(kScanMaxRate).epsilon(0.001));
    CHECK(test_scan_rate(-1.f) == doctest::Approx(-kScanMaxRate).epsilon(0.001));
    CHECK(test_scan_rate(kScanDead) == doctest::Approx(kScanMinRate).epsilon(0.01));

    // Monotone over the whole travel -- the property that makes the knob
    // playable. Checked with a step fine enough to catch a kink at either knee.
    float prev = -1e9f;
    for (int i = 0; i <= 400; ++i) {
        const float n = -1.f + 2.f * float(i) / 400.f;
        const float v = test_scan_rate(n);
        CHECK(v >= prev - 1e-6f);
        prev = v;
    }
}

TEST_CASE("sampler: SCAN advances the playhead, folds at the content edge, and reverses") {
    Rig g(24000);                     // 0.5 s of content
    g.e.set_flow(true);
    CHECK(g.e.scan_pos() == 0.f);

    // Realtime forward: after 24000 samples the head has travelled one full
    // content length and folded back to ~0.
    g.e.set_scan(sampler_cfg::kScanKnee);
    g.render(12000);
    const float half = g.e.scan_pos();
    CHECK(half == doctest::Approx(12000.f).epsilon(0.02));
    g.render(12000);
    CHECK(g.e.scan_pos() < 600.f);    // folded, not run past the end

    // Reverse walks it back down.
    g.e.set_scan(-sampler_cfg::kScanKnee);
    g.render(6000);
    const float back = g.e.scan_pos();
    CHECK(back > 12000.f);            // folded downward through zero
}

TEST_CASE("sampler: a frozen SCAN leaves the playhead exactly where it was") {
    Rig g(24000);
    g.e.set_flow(true);
    g.e.set_scan(sampler_cfg::kScanKnee);
    g.render(4800);
    const float parked = g.e.scan_pos();
    REQUIRE(parked > 0.f);

    g.e.set_scan(0.f);
    g.render(48000);                  // a full second of nothing happening
    CHECK(g.e.scan_pos() == parked);  // exactly, not approximately
}

TEST_CASE("sampler: an empty buffer parks the playhead instead of drifting") {
    Rig g(0);
    REQUIRE(g.e.is_empty());
    g.e.set_flow(true);
    g.e.set_scan(1.f);
    g.render(48000);
    CHECK(g.e.scan_pos() == 0.f);
}

TEST_CASE("sampler: clear() and load_sample() send the playhead home") {
    Rig g(24000);
    g.e.set_flow(true);
    g.e.set_scan(sampler_cfg::kScanKnee);
    g.render(4800);
    REQUIRE(g.e.scan_pos() > 0.f);

    g.e.clear();
    CHECK(g.e.scan_pos() == 0.f);

    g.e.load_sample(g.l.data(), g.r.data(), 24000);
    g.render(4800);
    REQUIRE(g.e.scan_pos() > 0.f);
    g.e.load_sample(g.l.data(), g.r.data(), 24000);
    CHECK(g.e.scan_pos() == 0.f);
}

TEST_CASE("sampler: SCAN moves what the grains actually read") {
    // The point of the whole feature: not that a counter advances, but that
    // the spawn position follows it. ORGANIZE stays put; only SCAN moves.
    Rig g(24000);
    g.e.set_flow(true);
    g.feed(0.5f, 0.f, 0.5f);          // SOURCE 0 -- the head sits at the start
    g.e.set_scan(0.f);
    g.render(4800);
    const float parked = g.e.last_spawn_pos();

    g.e.set_scan(sampler_cfg::kScanKnee);
    g.render(9600);
    CHECK(g.e.last_spawn_pos() > parked + 1000.f);
}
```

- [ ] **Step 3: Tests laufen lassen und Fehlschlag bestätigen**

```bash
cmake --build build && ./build/spky_tests.exe -tc="sampler: the SCAN curve*"
```
Erwartet: Compile-Fehler — `test_scan_rate`, `set_scan` und `scan_pos` existieren nicht.

- [ ] **Step 4: Kurvenhelfer und Test-Seam**

In `engine/sampler/sampler_engine.cpp`, im anonymen Namespace zu den anderen Kurvenhelfern (nach `size_seconds`, heute Zeile 43):

```cpp
// SCAN: knob position -> playhead rate in frames per sample (i.e. multiples
// of realtime). Control rate only -- std::pow must never reach the per-sample
// path. See the curve comment in sampler_config.h.
inline float scan_rate(float n) {
    n = clampf(n, -1.f, 1.f);
    const float a = n < 0.f ? -n : n;
    if (a < kScanDead) return 0.f;
    const float sign = n < 0.f ? -1.f : 1.f;
    if (a <= kScanKnee) {
        const float t = (a - kScanDead) / (kScanKnee - kScanDead);
        return sign * kScanMinRate * std::pow(1.f / kScanMinRate, t);
    }
    const float t = (a - kScanKnee) / (1.f - kScanKnee);
    return sign * lerpf(1.f, kScanMaxRate, t);
}
```

Den Test-Seam direkt neben `test_spawn_interval` (heute Zeile 72):

```cpp
float test_scan_rate(float n) { return scan_rate(n); }
```

Und die Deklaration in `engine/sampler/sampler_engine.h`, neben der von `test_spawn_interval` (Zeile 30):

```cpp
// Test seam only: forwards to the anonymous-namespace SCAN curve helper in
// sampler_engine.cpp, so the dead zone, both knees and the endpoints can be
// pinned without driving a whole engine.
float test_scan_rate(float n);
```

- [ ] **Step 5: Setter, Beobachter und Member**

In `engine/sampler/sampler_engine.h`, im `// --- edit layer ---`-Block nach `set_overlap`:

```cpp
    // SCAN: the running playhead (spec 2026-07-21 morphagene-controls).
    // bipolar is -1..+1; the sign is the direction, the centre is a real dead
    // zone. The accumulated position is ADDED to the SOURCE target in
    // _spawn_one, so ORGANIZE sets where the head starts and SCAN moves it.
    void set_scan(float bipolar);
```

Im Beobachtungsblock:

```cpp
    // Accumulated playhead offset in frames, folded into [0, rec_size).
    // Drives the VCV ring's read-position dot as well as the tests.
    float scan_pos() const { return _scan_pos; }
```

Im Member-Block, nach `_overlap`:

```cpp
    float _scan_rate   = 0.f;     // frames per sample, signed
    float _scan_pos    = 0.f;     // accumulated offset in frames, folded
```

Und der Setter am Dateiende von `sampler_engine.cpp`, nach `set_overlap`:

```cpp
void SamplerEngine::set_scan(float bipolar) { _scan_rate = scan_rate(bipolar); }
```

- [ ] **Step 6: Den Akkumulator vorrücken lassen**

In `engine/sampler/sampler_engine.cpp`, in `_update_control()`, direkt nach dem Normalisierungsblock (nach heutiger Zeile 292, vor dem FILT-Block):

```cpp
    // --- SCAN: advance the playhead one control tick's worth ---
    // Control rate, not per sample: kCtrlInterval is 96 samples (~2 ms), far
    // finer than any audible playhead motion, and it keeps std::floor off the
    // per-sample path. The fold is the same O(1) form read_linear uses
    // (sample_buffer.cpp) -- deliberately NOT a subtract loop, which would
    // spin once per content length after a long freeze at a high rate.
    // An empty buffer parks the head: there is nothing to read, and a
    // position accumulated against no content would be meaningless the moment
    // a recording started.
    const float scan_content = static_cast<float>(_buf.rec_size());
    if (scan_content > 0.f) {
        _scan_pos += _scan_rate * static_cast<float>(kCtrlInterval);
        _scan_pos -= scan_content * std::floor(_scan_pos / scan_content);
        if (_scan_pos < 0.f) _scan_pos = 0.f;      // -0.0 and rounding at the seam
    } else {
        _scan_pos = 0.f;
    }
```

- [ ] **Step 7: Die Leseposition folgt dem Kopf**

In `_spawn_one()`, Zeile 336 ändern:

```cpp
    float centre = clampf(_targets[LANE_SOURCE], 0.f, 1.f) * span + _scan_pos + jitter;
```

Die beiden vorhandenen `while`-Schleifen darunter (Zeilen 337-338) bleiben **unverändert** und falten weiterhin. Sie laufen höchstens dreimal: `_scan_pos` ist bereits in `[0, content)` gefaltet, `span < content` und `|jitter| <= content`.

- [ ] **Step 8: Reset am gemeinsamen Punkt**

In `_kill_all()` (heute Zeile 196-204), nach `_spawn_ctr = 0.f;`:

```cpp
    // The playhead goes home with the grains. This covers init(), clear() and
    // load_sample() in one place -- they are _kill_all's only three callers --
    // so no reset path can be forgotten. punch() deliberately does NOT come
    // through here: it rewinds the head without killing what is sounding.
    _scan_pos = 0.f;
```

- [ ] **Step 9: Tests laufen lassen**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```
Erwartet: alle Tests grün.

- [ ] **Step 10: Byte-Gatter prüfen**

```bash
./build/render.exe host/render/scenarios/ctrl_identity.json /tmp/b.wav /tmp/b.csv
sha256sum /tmp/b.wav
```
Erwartet: Übereinstimmung mit `host/render/scenarios/ctrl_identity.sha256`. Der Vorgabewert `_scan_rate = 0.f` macht den Akkumulator zu einem No-op, solange niemand `set_scan` ruft — genau das muss dieser Lauf belegen.

- [ ] **Step 11: Commit**

```bash
git add engine/sampler/sampler_config.h engine/sampler/sampler_engine.h \
        engine/sampler/sampler_engine.cpp tests/test_sampler_engine.cpp
git commit -m "feat(sampler): running playhead (SCAN)"
```

---

### Task 3: `punch()` — die Rückkehr-Geste

**Files:**
- Modify: `engine/sampler/sampler_engine.h` (Setter-Block)
- Modify: `engine/sampler/sampler_engine.cpp` (Definition beim Setter-Block am Dateiende)
- Test: `tests/test_sampler_engine.cpp`

**Interfaces:**
- Consumes: `_scan_pos` aus Task 2, `_overlap` aus Task 1.
- Produces: `void SamplerEngine::punch()` — Tonkopf zurück auf ORGANIZE, sofortiger Spawn. Aufgerufen von `Instrument::sampler_punch` (Task 4).

**Warum das existiert:** Position, Verhältnis und Länge werden beim Spawn gelatcht (`sampler_engine.cpp:409`), die nächste Gelegenheit kommt erst nach `_spawn_every`. Bei Überlappung 1 und GENE SIZE 10 s antwortet das Deck bis zu zehn Sekunden lang auf gar nichts. Ohne diese Geste ist der lange Pol kein spielbarer Zustand.

- [ ] **Step 1: Die fehlschlagenden Tests schreiben**

An `tests/test_sampler_engine.cpp` anhängen:

```cpp
TEST_CASE("sampler: punch() forces a grain now, even where the next one is seconds away") {
    // This is the test that carries the feature's reason for existing. At
    // overlap 1 and a long SIZE the scheduler is idle for the whole grain
    // length, so every knob is dead until the next spawn. punch() is the
    // gesture that says "read again now".
    Rig g(kFrames);
    g.e.set_flow(true);
    g.feed(0.5f, 0.f, 1.f);           // SIZE 1.0 -> 42 s
    g.e.set_overlap(0.f);             // overlap 1 -> _spawn_every == grain length
    g.render(4800);

    const int before = g.e.spawn_count();
    g.render(4800);                   // 100 ms: nowhere near the next spawn
    REQUIRE(g.e.spawn_count() == before);

    g.e.punch();
    g.render(200);                    // a couple of control ticks
    CHECK(g.e.spawn_count() > before);
}

TEST_CASE("sampler: punch() rewinds the playhead without killing what is sounding") {
    Rig g(24000);
    g.e.set_flow(true);
    g.feed(0.5f, 0.f, 0.5f);
    g.e.set_scan(sampler_cfg::kScanKnee);
    g.render(9600);
    REQUIRE(g.e.scan_pos() > 0.f);
    const int sounding = g.e.active_grains();
    REQUIRE(sounding > 0);

    g.e.punch();
    CHECK(g.e.scan_pos() == 0.f);
    // The distinction from clear()/load_sample(): those go through _kill_all
    // and silence the deck. punch() is a musical gesture on a running cloud.
    CHECK(g.e.active_grains() == sounding);
}
```

- [ ] **Step 2: Tests laufen lassen und Fehlschlag bestätigen**

```bash
cmake --build build && ./build/spky_tests.exe -tc="sampler: punch()*"
```
Erwartet: Compile-Fehler — `punch` ist kein Member von `SamplerEngine`.

- [ ] **Step 3: Deklaration**

In `engine/sampler/sampler_engine.h`, im `// --- edit layer ---`-Block nach `set_scan`:

```cpp
    // "New gene now" (spec 2026-07-21 morphagene-controls): the playhead
    // returns to ORGANIZE and a grain spawns immediately. Wired to NEW and
    // TRIG in the sampler.
    //
    // Deliberately NOT routed through _kill_all: this is a gesture on a
    // running cloud, so grains that are already sounding keep sounding. It is
    // what makes the long-grain end of SIZE playable at all -- without it,
    // every knob is dead until the next scheduled spawn, which at overlap 1
    // and SIZE near the top is tens of seconds away.
    void punch();
```

- [ ] **Step 4: Definition**

In `engine/sampler/sampler_engine.cpp`, am Dateiende nach `set_scan`:

```cpp
void SamplerEngine::punch() {
    _scan_pos  = 0.f;
    _spawn_ctr = 0.f;   // the next process() spawns; see the scheduling block
}
```

- [ ] **Step 5: Tests laufen lassen**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```
Erwartet: alle Tests grün.

- [ ] **Step 6: Commit**

```bash
git add engine/sampler/sampler_engine.h engine/sampler/sampler_engine.cpp \
        tests/test_sampler_engine.cpp
git commit -m "feat(sampler): punch() -- rewind the playhead and read now"
```

---

### Task 4: Part- und Instrument-Verdrahtung

**Files:**
- Modify: `engine/parts/part.h` — Setter bei `:84-85`, neue Setter, Konstante, Member
- Modify: `engine/parts/part.cpp` — `_control_tick()` bei `:117-148`
- Modify: `engine/instrument.h` — Sampler-Block bei `:95-117`
- Test: `tests/test_sampler_part.cpp`

**Interfaces:**
- Consumes: `SamplerEngine::set_overlap(float)`, `set_scan(float)`, `punch()` aus den Tasks 1-3.
- Produces:
  - `void Part::set_sampler_overlap(float n)` — speichert die Knopfstellung; der wirksame Wert entsteht in `_control_tick`.
  - `float Part::overlap_eff() const` — Knopf plus MOTION-Anteil, wie zuletzt gepusht.
  - `void Instrument::sampler_overlap(int p, float n)`
  - `void Instrument::sampler_scan(int p, float bipolar)`
  - `void Instrument::sampler_punch(int p)`

**Kontext:** `set_voice_sub` und `set_voice_detune` (`part.h:84-85`) leiten heute an **beide** Engines weiter. Da SUB und DTUN im Sampler ihre Aufgabe abgeben, dürfen sie den Sampler nicht mehr erreichen; `SamplerEngine::_sub_n` und `_detune_n` stehen ab Werk auf `0.f` und bleiben damit stumm. Das MOTION-Muster für die Überlappung ist eine Kopie der COLOR-Stelle (`part.cpp:129-134`).

- [ ] **Step 1: Die fehlschlagenden Tests schreiben**

An `tests/test_sampler_part.cpp` anhängen (das Rig dieser Datei mit `g2.inst` ist dort bereits etabliert, vgl. `:227`):

```cpp
TEST_CASE("sampler part: the MOTION lane breathes the grain overlap") {
    // DENS would otherwise be the deck's only completely static control --
    // it hangs off no lane and no jack. This mirrors how MOTION already
    // reaches COLOR (part.cpp:129-134).
    Part p;
    p.init(48000.f, 0);
    p.set_engine(ENGINE_SAMPLER);
    p.set_sampler_overlap(0.5f);
    p.set_depth(1.f);
    p.set_target_active(LANE_MOTION, true);

    float lo = 2.f, hi = -2.f;
    for (int i = 0; i < 48000; ++i) {
        float a = 0.f, b = 0.f;
        p.process(a, b);
        const float e = p.overlap_eff();
        if (e < lo) lo = e;
        if (e > hi) hi = e;
    }
    CHECK(hi > lo + 0.02f);           // it actually moves
    CHECK(lo >= 0.f);
    CHECK(hi <= 1.f);
}

TEST_CASE("sampler part: an inactive MOTION target leaves the overlap on the knob") {
    Part p;
    p.init(48000.f, 0);
    p.set_engine(ENGINE_SAMPLER);
    p.set_sampler_overlap(0.4f);
    p.set_target_active(LANE_MOTION, false);
    for (int i = 0; i < 4800; ++i) { float a = 0.f, b = 0.f; p.process(a, b); }
    CHECK(p.overlap_eff() == doctest::Approx(0.4f));
}

TEST_CASE("sampler part: a deactivated PITCH lane holds pitch but keeps firing") {
    // The whole point of the pitch decision: material and a synth deck stay
    // in one key, WITHOUT losing rhythmic triggering. _active gates the
    // VALUE (part.cpp:44-57); lane_fired is independent of it (part.cpp:183),
    // which is why STEP still triggers.
    Part p;
    p.init(48000.f, 0);
    p.set_engine(ENGINE_SAMPLER);
    p.set_step(true, 8);
    p.set_depth(1.f);
    p.set_target_active(LANE_PITCH, false);

    const float pitch0 = p.target_raw(LANE_PITCH);
    bool fired_at_least_once = false;
    for (int i = 0; i < 48000 * 4; ++i) {
        float a = 0.f, b = 0.f;
        p.process(a, b);
        if (p.mod().lane_fired(LANE_PITCH)) fired_at_least_once = true;
        CHECK(p.target_raw(LANE_PITCH) == pitch0);   // exactly, never drifting
    }
    CHECK(fired_at_least_once);
}

TEST_CASE("sampler part: TRIG produces a grain in the FLOW cloud") {
    // Regression pin for an M5b defect this plan fixes as a side effect:
    // trigger_manual only latches _burst_ratio, and _next_ratio reads that
    // latch solely when !_flow (sampler_engine.cpp:226) -- so the pad has
    // been inert in the standing cloud. THIS TEST IS EXPECTED TO FAIL BEFORE
    // the Task 6 host change and to pass after; if it already passes at Task
    // 4, say so in the report rather than deleting it.
    Part p;
    p.init(48000.f, 0);
    p.set_engine(ENGINE_SAMPLER);
    p.set_step(false, 8);                   // FLOW
    for (int i = 0; i < 4800; ++i) { float a = 0.f, b = 0.f; p.process(a, b); }

    const int before = p.sampler().spawn_count();
    p.sampler().punch();                    // what NEW/TRIG will call
    for (int i = 0; i < 400; ++i) { float a = 0.f, b = 0.f; p.process(a, b); }
    CHECK(p.sampler().spawn_count() > before);
}

TEST_CASE("sampler part: SUB and DTUN no longer reach the sampler") {
    // They are GENE SIZE and ORGANIZE on the panel now (spec 2026-07-21
    // morphagene-controls). The sampler's own sub/detune stay at their
    // silent defaults, and the synth keeps both.
    Part p;
    p.init(48000.f, 0);
    p.set_voice_sub(1.f);
    p.set_voice_detune(1.f);
    CHECK(p.sampler().sub() == doctest::Approx(0.f));
    CHECK(p.sampler().detune() == doctest::Approx(0.f));
}
```

Damit der letzte Test etwas prüfen kann, braucht `SamplerEngine` zwei Beobachter — in `engine/sampler/sampler_engine.h`, im Beobachtungsblock:

```cpp
    float sub() const    { return _sub_n; }
    float detune() const { return _detune_n; }
```

- [ ] **Step 2: Tests laufen lassen und Fehlschlag bestätigen**

```bash
cmake --build build && ./build/spky_tests.exe -tc="sampler part: the MOTION lane*"
```
Erwartet: Compile-Fehler — `set_sampler_overlap` und `overlap_eff` existieren nicht.

- [ ] **Step 3: SUB und DTUN vom Sampler abklemmen**

In `engine/parts/part.h`, Zeilen 84-85:

```cpp
    // SUB and DETUNE are synth-only from the morphagene-controls spec on
    // (2026-07-21): on the panel these two knobs are GENE SIZE and ORGANIZE
    // in the sampler, so forwarding them here as well would give one knob two
    // simultaneous jobs in the same engine. SamplerEngine::_sub_n and
    // _detune_n default to 0 and now stay there.
    void set_voice_sub(float n)       { _synth.set_sub(n); }
    void set_voice_detune(float n)    { _synth.set_detune(n); }
```

- [ ] **Step 4: Überlappungsknopf, Konstante und Member**

In `engine/parts/part.h`, bei den anderen Knopf-Settern (neben `set_color`, Zeile 38):

```cpp
    // DENS in the sampler: the grain-overlap knob. Stored, not pushed --
    // _control_tick combines it with the MOTION lane and hands the sampler
    // the effective value, exactly as COLOR does.
    void set_sampler_overlap(float n) { _overlap = clampf(n, 0.f, 1.f); }
    float overlap_eff() const { return _overlap_eff; }
```

Bei den COLOR-Konstanten (Zeile 240-241):

```cpp
    // DENS is a fourth destination of the MOTION lane (spec 2026-07-21
    // morphagene-controls), so the cloud breathes in density instead of
    // standing still. Bipolar and additive, same shape as kColorMod. No gate
    // twin to kColorGate: the sampler has no bit-identity guarantee to
    // protect at knob 0, and overlap 1 is a musical value, not an off state.
    // Ear-tunable.
    static constexpr float kOverlapMod = 0.2f;
```

Bei den Membern (nach `_color_eff`, Zeile 247):

```cpp
    float _overlap = 1.f;        // DENS knob; effective value computed in _control_tick
    float _overlap_eff = 1.f;    // knob + MOTION swing, as last pushed to _sampler
```

Der Vorgabewert `1.f` ergibt Überlappung 8 — der Zustand aus M5b, unverändert für jeden, der den Regler nie anfasst.

- [ ] **Step 5: Den wirksamen Wert im Kontrolltakt bilden**

In `engine/parts/part.cpp`, in `_control_tick()`, direkt nach dem COLOR-Block (nach heutiger Zeile 134):

```cpp
    // DENS -> grain overlap, with MOTION's swing on top (spec 2026-07-21
    // morphagene-controls). Pushed straight at _sampler rather than through
    // _engine: it is a sampler-only parameter, and _sampler is a concrete
    // member here just as it is for the voice row (part.h). On a synth part
    // this is one float store into an engine nobody is listening to.
    const float omod = _active[LANE_MOTION]
        ? _mod.lane_output(LANE_MOTION) * _depth * kOverlapMod
        : 0.f;
    _overlap_eff = clampf(_overlap + omod, 0.f, 1.f);
    _sampler.set_overlap(_overlap_eff);
```

- [ ] **Step 6: Instrument-Weiterleitungen**

In `engine/instrument.h`, im M5-Sampler-Block nach `sampler_feedback` (Zeile 110):

```cpp
    // --- M5c sampler controls (spec 2026-07-21 morphagene-controls) ---
    // NOTE: not "morph" -- set_morph is already taken by the global A/B
    // control (see set_morph above). This is the grain overlap.
    void  sampler_overlap(int p, float n)  { _parts[p].set_sampler_overlap(n); }
    void  sampler_scan(int p, float bipolar) { _parts[p].sampler().set_scan(bipolar); }
    void  sampler_punch(int p)             { _parts[p].sampler().punch(); }
    float sampler_scan_pos(int p) const    { return _parts[p].sampler().scan_pos(); }
```

`sampler_overlap` geht bewusst über `Part` und nicht über `sampler()`: der MOTION-Anteil wird in `Part` gebildet.

- [ ] **Step 7: Tests laufen lassen**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```
Erwartet: alle Tests grün. Falls Tests in `tests/test_sampler_engine.cpp` oder `tests/test_sampler_part.cpp` fehlschlagen, weil sie SUB oder DTUN am Sampler erwartet haben: das ist eine echte Verhaltensänderung aus der Spec — den Test anpassen und die Anpassung im Bericht benennen, nicht die Weiterleitung wiederherstellen.

- [ ] **Step 8: Byte-Gatter prüfen**

```bash
./build/render.exe host/render/scenarios/ctrl_identity.json /tmp/d.wav /tmp/d.csv
sha256sum /tmp/d.wav
```
Erwartet: Übereinstimmung mit `host/render/scenarios/ctrl_identity.sha256`. Dieser Task fasst `_control_tick` an, also den geteilten Pfad — hier ist das Gatter am schärfsten.

- [ ] **Step 9: Commit**

```bash
git add engine/parts/part.h engine/parts/part.cpp engine/instrument.h \
        engine/sampler/sampler_engine.h tests/test_sampler_part.cpp
git commit -m "feat(sampler): wire overlap, scan and punch through Part and Instrument"
```

---

### Task 5: Szenario-Aktionen und Hörszenarien

**Files:**
- Modify: `host/render/scenario.cpp:153-158` (Sampler-Aktionsblock)
- Create: `host/render/scenarios/sampler_scan.json`
- Create: `host/render/scenarios/sampler_overlap.json`
- Test: `tests/test_scenario.cpp`

**Interfaces:**
- Consumes: `Instrument::sampler_overlap(int, float)`, `sampler_scan(int, float)`, `sampler_punch(int)` aus Task 4.
- Produces: JSON-Aktionen `sampler_overlap`, `sampler_scan`, `sampler_punch`.

**Kontext:** `apply_event` (`host/render/scenario.cpp:100-168`) ist eine `else if`-Kette; unbekannte Aktionen werden absichtlich ignoriert. `parse_event` (`:12-47`) legt fest, welche JSON-Schlüssel existieren: `action`, `part`, `slot`, `flag`, `ivalue`, `value` (Zahl **oder** String), `t`.

**Wichtig:** `sampler_scan` nimmt einen **bipolaren** Wert −1…+1. `parse_event` klemmt `value` nicht, also kommt er unverändert an.

- [ ] **Step 1: Den fehlschlagenden Test schreiben**

An `tests/test_scenario.cpp` anhängen:

**Zum Aufsetzen:** `tests/test_scenario.cpp` und `tests/test_sampler_part.cpp` richten bereits ein `Instrument` mit injiziertem Sampler-Speicher ein. **Übernimm das dortige Muster wörtlich**, statt die Signatur aus dem Gedächtnis zu bauen — der Block unten zeigt die Absicht, nicht zwingend die exakte Aufsetz-Syntax dieses Repos.

```cpp
TEST_CASE("scenario: the sampler control actions reach the engine") {
    // Setup: follow the existing Instrument rig in this file / in
    // tests/test_sampler_part.cpp -- injected sampler memory, then init.
    Instrument inst;
    std::vector<SampleBuffer::Frame> mem(48000);
    Instrument::Config cfg;
    cfg.sampler_buf[0] = mem.data();
    cfg.sampler_frames = mem.size();
    inst.init(48000.f, cfg);
    inst.set_engine(0, ENGINE_SAMPLER);

    Event e;
    e.part = 0;

    e.action = "sampler_overlap"; e.value = 0.f;
    apply_event(inst, e);
    // Part stores the knob; the effective value reaches the engine on the
    // next control tick, so drive one.
    for (int i = 0; i < 200; ++i) { float a = 0.f, b = 0.f; inst.process(a, b); }
    CHECK(inst.sampler_overlap_eff(0) == doctest::Approx(0.f));

    e.action = "sampler_scan"; e.value = -1.f;
    apply_event(inst, e);
    for (int i = 0; i < 4800; ++i) { float a = 0.f, b = 0.f; inst.process(a, b); }
    // Reverse from a home position folds upward, so any motion at all proves
    // the action landed.
    CHECK(inst.sampler_scan_pos(0) != 0.f);

    e.action = "sampler_punch";
    apply_event(inst, e);
    CHECK(inst.sampler_scan_pos(0) == 0.f);
}
```

Dazu braucht `Instrument` einen Beobachter — in `engine/instrument.h`, im neuen Sampler-Block:

```cpp
    float sampler_overlap_eff(int p) const { return _parts[p].overlap_eff(); }
```

- [ ] **Step 2: Test laufen lassen und Fehlschlag bestätigen**

```bash
cmake --build build && ./build/spky_tests.exe -tc="scenario: the sampler control actions*"
```
Erwartet: Fehlschlag — die drei Aktionen sind unbekannt und werden ignoriert, also bleibt `sampler_overlap_eff` auf dem Vorgabewert 1.0.

- [ ] **Step 3: Aktionen ergänzen**

In `host/render/scenario.cpp`, im Sampler-Block nach `sampler_feedback` (Zeile 158):

```cpp
    else if (a == "sampler_overlap")    inst.sampler_overlap(e.part, e.value);
    else if (a == "sampler_scan")       inst.sampler_scan(e.part, e.value);
    else if (a == "sampler_punch")      inst.sampler_punch(e.part);
```

- [ ] **Step 4: Test laufen lassen**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```
Erwartet: alle Tests grün.

- [ ] **Step 5: Hörszenario für SCAN**

`host/render/scenarios/sampler_scan.json` anlegen:

```json
{
  "_comment": "Listening aid for SCAN (spec 2026-07-21 morphagene-controls). One long grain, overlap 1, the playhead walked from frozen to realtime to reverse. Part A is silenced so the deck can be judged alone.",
  "sample_rate": 48000,
  "bpm": 96,
  "duration_s": 48.0,
  "input_wav": "host/render/scenarios/assets/in_drone.wav",
  "init": [
    { "action": "set_engine", "part": 1, "value": "sampler" },
    { "action": "set_target_base", "part": 0, "slot": 4, "value": 0.0 },
    { "action": "set_target_active", "part": 0, "slot": 4, "flag": false },
    { "action": "set_target_base", "part": 1, "slot": 4, "value": 1.0 },
    { "action": "set_target_active", "part": 1, "slot": 2, "flag": false },
    { "action": "set_target_base", "part": 1, "slot": 3, "value": 0.0 },
    { "action": "set_target_active", "part": 1, "slot": 3, "flag": false },
    { "action": "set_target_base", "part": 1, "slot": 0, "value": 0.0 },
    { "action": "set_target_base", "part": 1, "slot": 1, "value": 0.6 },
    { "action": "sampler_overlap", "part": 1, "value": 0.3 }
  ],
  "events": [
    { "t": 1.0,  "action": "sampler_record", "part": 1, "flag": true },
    { "t": 9.0,  "action": "sampler_record", "part": 1, "flag": false },
    { "t": 10.0, "action": "sampler_scan", "part": 1, "value": 0.0 },
    { "t": 16.0, "action": "sampler_scan", "part": 1, "value": 0.4 },
    { "t": 24.0, "action": "sampler_scan", "part": 1, "value": 0.75 },
    { "t": 32.0, "action": "sampler_scan", "part": 1, "value": 1.0 },
    { "t": 38.0, "action": "sampler_scan", "part": 1, "value": -0.75 },
    { "t": 44.0, "action": "sampler_punch", "part": 1 }
  ]
}
```

- [ ] **Step 6: Hörszenario für Überlappung und die Rückkehr-Geste**

`host/render/scenarios/sampler_overlap.json` anlegen:

```json
{
  "_comment": "Listening aid for DENS (grain overlap) and punch(). Sweeps overlap 8 -> 1 at a mid grain length, then parks at overlap 1 with a long grain and punches four times -- the case where nothing else in the instrument responds. Also the scenario for the suspected 'pumping' zone at low overlap and 100-500 ms grains (spec, listening notes).",
  "sample_rate": 48000,
  "bpm": 96,
  "duration_s": 44.0,
  "input_wav": "host/render/scenarios/assets/in_drone.wav",
  "init": [
    { "action": "set_engine", "part": 1, "value": "sampler" },
    { "action": "set_target_base", "part": 0, "slot": 4, "value": 0.0 },
    { "action": "set_target_active", "part": 0, "slot": 4, "flag": false },
    { "action": "set_target_base", "part": 1, "slot": 4, "value": 1.0 },
    { "action": "set_target_active", "part": 1, "slot": 2, "flag": false },
    { "action": "set_target_active", "part": 1, "slot": 3, "flag": false },
    { "action": "set_target_base", "part": 1, "slot": 0, "value": 0.2 },
    { "action": "set_target_base", "part": 1, "slot": 1, "value": 0.5 }
  ],
  "events": [
    { "t": 1.0,  "action": "sampler_record", "part": 1, "flag": true },
    { "t": 9.0,  "action": "sampler_record", "part": 1, "flag": false },
    { "t": 10.0, "action": "sampler_overlap", "part": 1, "value": 1.0 },
    { "t": 14.0, "action": "sampler_overlap", "part": 1, "value": 0.66 },
    { "t": 18.0, "action": "sampler_overlap", "part": 1, "value": 0.33 },
    { "t": 22.0, "action": "sampler_overlap", "part": 1, "value": 0.0 },
    { "t": 26.0, "action": "set_target_base", "part": 1, "slot": 1, "value": 0.85 },
    { "t": 30.0, "action": "sampler_punch", "part": 1 },
    { "t": 34.0, "action": "sampler_punch", "part": 1 },
    { "t": 38.0, "action": "sampler_punch", "part": 1 },
    { "t": 41.0, "action": "sampler_punch", "part": 1 }
  ]
}
```

- [ ] **Step 7: Beide Szenarien rendern**

```bash
./build/render.exe host/render/scenarios/sampler_scan.json /tmp/scan.wav /tmp/scan.csv
./build/render.exe host/render/scenarios/sampler_overlap.json /tmp/ovl.wav /tmp/ovl.csv
```
Erwartet: beide laufen ohne Fehlermeldung durch und erzeugen Dateien deutlich über 0 Byte. Diese Renders sind Plausibilitätsprüfung und Hörmaterial, **kein Byte-Gatter** — es werden keine `.sha256`-Dateien dazu angelegt.

- [ ] **Step 8: Determinismus prüfen**

```bash
./build/render.exe host/render/scenarios/sampler_scan.json /tmp/scan2.wav /tmp/scan2.csv
cmp /tmp/scan.wav /tmp/scan2.wav && echo DETERMINISTIC
```
Erwartet: `DETERMINISTIC`.

- [ ] **Step 9: Commit**

```bash
git add host/render/scenario.cpp engine/instrument.h tests/test_scenario.cpp \
        host/render/scenarios/sampler_scan.json host/render/scenarios/sampler_overlap.json
git commit -m "feat(render): scenario actions and listening scenes for the sampler controls"
```

---

### Task 6: Reglerumdeutung im VCV-Host

**Files:**
- Modify: `host/vcv/src/Spotymod.cpp` — `pushParams()` bei `:335-441`
- Test: manuell, plus `python res/test_panel.py` (unverändert grün)

**Interfaces:**
- Consumes: `Instrument::sampler_overlap`, `sampler_scan`, `sampler_punch`, `set_target_base`, `set_target_active`.
- Produces: nichts für spätere Tasks.

**Kontext:** `pushParams()` wird jeden Block gerufen; ENG wird **gepollt, nicht flankengetriggert** (`Spotymod.cpp:377-380`). `pp(baseA, part)` liest einen Parameter aus dem Stride, `ppb` dasselbe als bool. `MELODY_A` liefert **−1…+1** (siehe Kommentar bei `:342`).

**Der `samplerPart`-Ausdruck existiert bereits** in dieser Datei bei `:519` als `ppb(ENGINE_A, p) && !smp[p].testTone` — dieselbe Bedingung gilt hier.

- [ ] **Step 1: Die vier Regler umdeuten**

In `host/vcv/src/Spotymod.cpp`, in `pushParams()`, direkt nach dem REC-Block (nach heutiger Zeile 429) einfügen:

```cpp
            // --- sampler control surface (spec 2026-07-21 morphagene-controls) ---
            // Four knobs that do nothing in the sampler's FLOW cloud get a
            // job of their own. The param ids do not change, so no saved
            // patch moves; only what the knob means when ENG says Sampler.
            //
            // set_variation and set_density above keep firing unconditionally
            // -- the "push to both, let the inactive side ignore it" pattern
            // the voice row already uses. DENS is the one knob that genuinely
            // does two things in sampler STEP mode: it still thins the groove
            // gate AND now sets grain overlap. Both point the same direction
            // (sparser), so this is left as-is and flagged for the listening
            // pass rather than special-cased.
            const bool samplerPart = eng2 && !smp[p].testTone;
            inst.sampler_overlap(p, pp(DENSITY_A, p));
            inst.sampler_scan(p, pp(MELODY_A, p));

            // GENE SIZE and ORGANIZE ride the lane BASES, so they must be
            // gated: in the synth these two slots drive the filter and the
            // timbre position, and writing SUB/DTUN into them there would be
            // wrong. The else branch is load-bearing -- a base left behind on
            // an engine flip would silently stick.
            if (samplerPart) {
                inst.set_target_base(p, spky::LANE_SIZE,   pp(SUB_A, p));
                inst.set_target_base(p, spky::LANE_SOURCE, pp(DETUNE_A, p));
            } else {
                inst.set_target_base(p, spky::LANE_SIZE,   0.5f);
                inst.set_target_base(p, spky::LANE_SOURCE, 0.5f);
            }

            // Stable pitch in the sampler: the lane still FIRES (that is what
            // keeps STEP triggering alive, part.cpp:183), it just stops
            // moving the pitch. Sample material and a synth deck can then sit
            // in the same key.
            inst.set_target_active(p, spky::LANE_PITCH, !samplerPart);
```

- [ ] **Step 2: NEW und TRIG auf die Rückkehr-Geste legen**

Die beiden Zeilen bei `:439-440` ersetzen:

```cpp
            // NEW is "new gene now" in the sampler: the playhead returns to
            // ORGANIZE and a grain spawns immediately. Without it the long
            // end of GENE SIZE is unplayable -- every knob stays dead until
            // the next scheduled spawn, tens of seconds away at overlap 1.
            if (newPhraseTrig[p].process(ppb(NEWPHRASE_A, p))) {
                if (samplerPart) inst.sampler_punch(p);
                else             inst.new_phrase(p);
            }
            // TRIG punches AND triggers. trigger_manual alone is inert in the
            // sampler's FLOW cloud -- _next_ratio reads the burst latch only
            // when !_flow (sampler_engine.cpp:226) -- so the pad has been
            // dead there since M5b. The punch fixes FLOW; the trigger keeps
            // STEP behaving as it does today.
            if (triggerTrig[p].process(ppb(TRIGGER_A, p))) {
                if (samplerPart) inst.sampler_punch(p);
                inst.trigger_manual(p);
            }
```

- [ ] **Step 3: Plugin bauen**

Aus `host/vcv/`, in einer MSYS2-Shell:

```bash
make CC=gcc CXX=g++ SHELL=/usr/bin/bash \
  TMP="$LOCALAPPDATA/Temp" TEMP="$LOCALAPPDATA/Temp" -j4
```
Erwartet: `plugin.dll` entsteht ohne Fehler.

Falls der Rack-SDK nicht auffindbar ist: `RACK_DIR=/pfad/zum/Rack-SDK` ergänzen. Falls der Build aus einem anderen Grund nicht läuft, **als BLOCKED melden** — nicht raten.

- [ ] **Step 4: Panel-Test läuft weiter durch**

Aus `host/vcv/`:

```bash
python res/test_panel.py
```
Erwartet: Exit-Code 0. Dieser Task fügt keine Parameter hinzu, `PARAM_ORDER` und `PART_STRIDE` bleiben unberührt — falls dieser Test etwas meldet, ist versehentlich am Panel gedreht worden.

- [ ] **Step 5: Engine-Tests bleiben grün**

```bash
cd ../.. && cmake --build build && ctest --test-dir build --output-on-failure
```
Erwartet: alle Tests grün. Der Host ist nicht Teil der Testsuite, aber dieser Task darf nichts an der Engine verändert haben.

- [ ] **Step 6: Commit**

```bash
git add host/vcv/src/Spotymod.cpp
git commit -m "feat(vcv): remap DENS/MELO/SUB/DTUN in the sampler, punch on NEW and TRIG"
```

---

### Task 7: Der Tonkopf wird auf dem Ring sichtbar

**Files:**
- Modify: `host/vcv/src/Spotymod.cpp` — `SpkyRing::drawLayer` bei `:806-846`

**Interfaces:**
- Consumes: `Instrument::sampler_scan_pos(int)` aus Task 4, `Instrument::sampler_rec_size(int)` (existiert, `instrument.h:117`), `Instrument::engine_id(int)` (existiert, `instrument.h:93`).

**Kontext:** `bright[kRingDots]` trägt **nur Helligkeit**; alle Punkte werden in einer Farbe pro Part gezeichnet (`kColGlow[part]`). Ein Tonkopf-Punkt in derselben Farbe wäre von den fünf Lane-Punkten nicht zu unterscheiden — deshalb wird er **nach** der Schleife separat gezeichnet, außerhalb des `bright[]`-Mechanismus.

**Warum:** Ein driftender Tonkopf ohne Anzeige ist blind zu spielen; man weiß nicht, wo im Band man liest, und findet keine Stelle wieder.

- [ ] **Step 1: Den Punkt zeichnen**

In `host/vcv/src/Spotymod.cpp`, in `SpkyRing::drawLayer`, **nach** der bestehenden Zeichenschleife (nach heutiger Zeile 845, vor der schließenden Klammer der Methode):

```cpp
        // --- sampler read position (spec 2026-07-21 morphagene-controls) ---
        // Drawn separately rather than through bright[]: that array carries
        // brightness only, and every dot in it is kColGlow[part], so a head
        // folded into it would be indistinguishable from a lane. Warm white
        // at full brightness reads as "this one is different" without a
        // second palette. On a synth part, or with nothing recorded, nothing
        // is drawn -- an idle ring stays dark, as it does today.
        if (module && module->inst.engine_id(part) == spky::ENGINE_SAMPLER) {
            const size_t content = module->inst.sampler_rec_size(part);
            if (content > 0) {
                const float frac =
                    module->inst.sampler_scan_pos(part) / float(content);
                const float a = TWO_PI * frac;
                Vec hp = c.plus(Vec(std::sin(a), -std::cos(a)).mult(R));
                nvgBeginPath(args.vg);
                nvgCircle(args.vg, hp.x, hp.y, dr * 3.0f);
                nvgFillColor(args.vg, nvgRGBAf(1.f, 0.95f, 0.85f, 0.20f));
                nvgFill(args.vg);
                nvgBeginPath(args.vg);
                nvgCircle(args.vg, hp.x, hp.y, dr * 1.15f);
                nvgFillColor(args.vg, nvgRGBAf(1.f, 0.95f, 0.85f, 0.95f));
                nvgFill(args.vg);
            }
        }
```

- [ ] **Step 2: Plugin bauen**

Aus `host/vcv/`, in einer MSYS2-Shell:

```bash
make CC=gcc CXX=g++ SHELL=/usr/bin/bash \
  TMP="$LOCALAPPDATA/Temp" TEMP="$LOCALAPPDATA/Temp" -j4
```
Erwartet: `plugin.dll` entsteht ohne Fehler.

- [ ] **Step 3: Commit**

```bash
git add host/vcv/src/Spotymod.cpp
git commit -m "feat(vcv): the LED ring shows the sampler read position"
```

---

### Task 8: Sampler-Beschriftungen auf dem Panel

**Files:**
- Modify: `host/vcv/res/gen_panel.py` — `TEXTS` bei `:381`, ggf. `PanelTxt`-Emission bei `:591-594`
- Modify: `host/vcv/src/Spotymod.cpp` — die Zeichenroutine für `kPanelTexts`, falls eine Ausrichtungsspalte nötig wird
- Modify: `host/vcv/res/test_panel.py`
- Regenerate: `host/vcv/res/Spotymod.svg`, `host/vcv/src/generated_panel.hpp`

**Kontext:** `TEXTS` (`gen_panel.py:381`) ist eine Liste von Tupeln `(x, y, size, spacing, colour, text)`, emittiert als `kPanelTexts` (`:591-594`). Die vier umgedeuteten Regler sind **Orbit-Regler**; ihre Hauptbeschriftung entsteht radial über `orbit_label(cx, cy, ang, mir)` (`:139-150`) und trägt eine Ausrichtung (`start`/`end`/`middle`), die `TEXTS` heute nicht kennt.

**Die Anforderung, nicht die Koordinaten:** Für jeden der vier Regler entsteht eine zweite Bildunterschrift mit der Sampler-Bedeutung, **2.2 mm weiter außen** als die Hauptbeschriftung, in Größe **1.5**, in der Part-Farbe (`GREEN` für A, `COPPER` für B). Die Positionen werden aus `orbit_label` **abgeleitet**, nicht von Hand eingetragen — dieser Plan gibt bewusst keine Zahlen vor, weil sie sonst bei der nächsten Layout-Änderung stillschweigend falsch würden.

Die Texte:

| Regler | Hauptbeschriftung | Sampler-Zeile |
|---|---|---|
| `MELODY_A` / `MELODY_B` | `MELO` | `SCAN` |
| `SUB_A` / `SUB_B` | `SUB` | `SIZE` |
| `DETUNE_A` / `DETUNE_B` | `DTUN` | `ORG` |

**`DENSITY` bekommt keine zweite Zeile.** Die Beschriftung bleibt `DENS`, weil das Wort in beiden Engines stimmt — im Synth Groove-Dichte, im Sampler Korndichte. Das ist zugleich der Grund, warum die Kornüberlappung nicht „MORPH" heißt: dieser Name gehört bereits dem globalen A/B-Regler.

**Falls `TEXTS` eine Ausrichtungsspalte braucht**, um radiale Zweitzeilen sauber zu setzen: `PanelTxt` um eine `anchor`-Spalte erweitern, genau nach dem Vorbild, das `PanelCtl` dafür schon hat (`gen_panel.py:550-553`, `ANCHOR_ID` bei `:575`), und die Zeichenroutine in `Spotymod.cpp` entsprechend nachziehen. `test_header_carries_label_columns` pinnt `PanelCtl`, **nicht** `PanelTxt` — diese Erweiterung ist erlaubt.

- [ ] **Step 1: Den fehlschlagenden Test schreiben**

An `host/vcv/res/test_panel.py` anhängen:

```python
SAMPLER_CAPTIONS = [("MELO", "SCAN"), ("SUB", "SIZE"), ("DTUN", "ORG")]

def test_sampler_captions_exist():
    """Every remapped knob carries its sampler meaning on the plate."""
    txt = [t[-1] for t in g.TEXTS]
    for _main, sampler in SAMPLER_CAPTIONS:
        check(txt.count(sampler) == 2,
              f"sampler caption {sampler!r} appears {txt.count(sampler)}x, want 2 (A and B)")
    check("MRPH" not in txt,
          "DENS must keep its label -- MORPH is the global A/B control")

def test_sampler_captions_sit_outside_their_labels():
    """The sampler line goes further out than the main caption, never between
    the knob and the LED ring."""
    for suffix, cx in (('_A', g.RING_CX_A), ('_B', g.W - g.RING_CX_A)):
        for base, sampler in (('MELODY', 'SCAN'), ('SUB', 'SIZE'), ('DETUNE', 'ORG')):
            c = ctl(base + suffix)
            lx, ly, _a, _s, _col = g.label_of(c)
            main_d = math.hypot(lx - cx, ly - g.RING_CY)
            hits = [t for t in g.TEXTS if t[-1] == sampler
                    and math.hypot(t[0] - cx, t[1] - g.RING_CY) > g.KNOB_R]
            check(len(hits) >= 1, f"{base}{suffix}: no {sampler} caption near it")
            d = max(math.hypot(t[0] - cx, t[1] - g.RING_CY) for t in hits)
            check(d > main_d, f"{base}{suffix}: {sampler} sits inside {c.label}")

def test_sampler_captions_stay_on_the_plate():
    for t in g.TEXTS:
        check(1.0 <= t[0] <= g.W - 1.0 and 1.0 <= t[1] <= g.Hh - 1.0,
              f"panel text {t[-1]!r} off plate at ({t[0]:.2f}, {t[1]:.2f})")
```

- [ ] **Step 2: Test laufen lassen und Fehlschlag bestätigen**

Aus `host/vcv/`:

```bash
python res/test_panel.py
```
Erwartet: Fehlschläge in `test_sampler_captions_exist` — die Bildunterschriften gibt es noch nicht.

- [ ] **Step 3: Die Bildunterschriften erzeugen**

In `host/vcv/res/gen_panel.py`, an `TEXTS` anhängen. Die Positionen werden aus `orbit_label` abgeleitet; der Radius wächst um 2.2 mm. Der Ausdruck folgt dem Muster, mit dem `TEXTS` schon die Sektions-Bildunterschriften spiegelt (`:385-387`):

```python
# --- sampler meanings of the remapped knobs (spec 2026-07-21) ------------------
# ENG turns four knobs into the sampler's own controls, so both meanings belong
# on the plate. DENS is deliberately absent: the word already fits both engines
# (groove density / grain density), and MORPH is taken by the global A/B knob.
SAMPLER_LBL = [("MELODY", "SCAN"), ("SUB", "SIZE"), ("DETUNE", "ORG")]
```

Und im Anschluss an die bestehende `TEXTS`-Definition ein Block, der für jeden Part und jeden der drei Regler die Hauptbeschriftung ausliest, den Radius um 2.2 mm vergrößert und den Eintrag anhängt. Die vorhandene Orbit-Geometrie (`ORBIT_A`, `RING_CX_A`, `RING_CY`, die Winkel und das `mir`-Flag) ist die Quelle — **keine Zahlen abschreiben**, sondern dieselbe Rechnung wie `orbit_label` mit `r + 2.2` verwenden, Größe `1.5`, Farbe `GREEN` für A und `COPPER` für B.

Falls sich dabei zeigt, dass die Ausrichtung mitgeführt werden muss, `PanelTxt` um eine `anchor`-Spalte erweitern (siehe Kontext oben).

- [ ] **Step 4: Panel neu erzeugen und Test laufen lassen**

Aus `host/vcv/`:

```bash
python res/gen_panel.py
python res/test_panel.py
```
Erwartet: Exit-Code 0, alle Prüfungen grün — insbesondere die bestehenden `test_enum_order`, `test_no_overlap` und `test_every_label_is_reachable`.

- [ ] **Step 5: Plugin bauen**

Aus `host/vcv/`, in einer MSYS2-Shell:

```bash
make CC=gcc CXX=g++ SHELL=/usr/bin/bash \
  TMP="$LOCALAPPDATA/Temp" TEMP="$LOCALAPPDATA/Temp" -j4
```
Erwartet: `plugin.dll` entsteht ohne Fehler.

- [ ] **Step 6: Commit**

```bash
git add host/vcv/res/gen_panel.py host/vcv/res/test_panel.py \
        host/vcv/res/Spotymod.svg host/vcv/src/generated_panel.hpp \
        host/vcv/src/Spotymod.cpp
git commit -m "feat(vcv): the panel carries the sampler meanings of the remapped knobs"
```

---

### Task 9: Dokumentation

**Files:**
- Modify: `host/vcv/README.md` — Abschnitt „Sampler" bei `:38-91`

- [ ] **Step 1: Die Bedienfläche beschreiben**

In `host/vcv/README.md`, im Abschnitt „Sampler", nach dem REC-Absatz einen neuen Unterabschnitt einfügen. Er muss folgendes tragen, in Prosa und ohne die Spec zu wiederholen:

- Die Tabelle der vier umgedeuteten Regler mit ihren Sampler-Bedeutungen und Bereichen: SCAN (Tonkopfvorschub, Mitte eingefroren, bis 8× Realzeit in beide Richtungen), DENS (Kornüberlappung 1…8), SUB als GENE SIZE (1 ms … 42 s), DTUN als ORGANIZE (Leseposition).
- Dass NEW und TRIG im Sampler „neues Gen jetzt" auslösen — Tonkopf zurück auf ORGANIZE, sofortiger Grain — und **warum**: ohne diese Geste antwortet das Deck am langen Ende von GENE SIZE bis zu zehn Sekunden lang auf keinen Regler.
- Dass der Tonkopf als heller Punkt auf dem LED-Ring läuft.
- Dass die Tonhöhe im Sampler stillsteht und ausschließlich über TUNE angeglichen wird, damit Sampler-Material und Synth in derselben Tonart sitzen; dass das rhythmische Triggern über STEP davon unberührt bleibt.
- Unter „Known limitations": dass die Reglerstellung engineunabhängig gilt, sich ein ENG-Wechsel deshalb **nicht vorbereiten lässt**, und dass es keine Parameter-CV-Eingänge gibt.

- [ ] **Step 2: Commit**

```bash
git add host/vcv/README.md
git commit -m "docs(vcv): the sampler control surface"
```

---

## Abschlussprüfung vor dem Merge

Nach Task 9, vor der Übergabe an `superpowers:finishing-a-development-branch`:

```bash
# aus dem Repo-Wurzelverzeichnis
cmake --build build && ctest --test-dir build --output-on-failure
./build/render.exe host/render/scenarios/ctrl_identity.json /tmp/final.wav /tmp/final.csv
sha256sum /tmp/final.wav                      # muss ctrl_identity.sha256 treffen
./build/render.exe host/render/scenarios/ctrl_identity.json /tmp/final2.wav /tmp/final2.csv
cmp /tmp/final.wav /tmp/final2.wav            # Determinismus
cd host/vcv && python res/test_panel.py
```

Danach bleibt die Abnahme durch Gehör offen — sie gehört dem Autor, nicht der Prüfkette:

- Ist der Totbereich um SCANs Mitte breit genug?
- Sitzt der Realzeit-Rastpunkt bei drei Vierteln dort, wo die Hand ihn sucht, und ist das obere Viertel mit Faktor 8 zu nervös?
- Verläuft DENS über seinen Bereich gleichmäßig?
- Pumpt die Fenstersumme bei niedriger Überlappung und mittlerer Kornlänge (100–500 ms) im Korntakt — die vermutete Zone zwischen den beiden gewollten Polen?
- Stört es, dass DENS im Sampler-STEP-Modus zugleich das Groove-Gate ausdünnt und die Überlappung setzt?


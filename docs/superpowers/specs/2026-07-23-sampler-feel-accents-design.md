# Sampler: FEEL — Akzente aus dem Material, DENS zurück an die Phrase

**Datum:** 2026-07-23
**Status:** Entwurf, vom Autor abgenommen
**Voraussetzung:** Slice-Groove (Spec 2026-07-22) ist auf `main` (Merge `ce24923`).

## Ziel

DENS steuert auf einem Sampler-Deck nur noch die Phrase. COLOR wird in STEP zu **FEEL**:
der Tiefe, mit der jedes Grain die Anschlagstärke seiner eigenen Transiente erbt. Rolls
entfallen ersatzlos. Kein neues Bedienelement, und unterm Strich weniger Code als vorher.

## Warum

**Ein Regler, zwei Jobs.** DENS ging bisher an zwei Ziele gleichzeitig
(`host/vcv/src/Spotymod.cpp:351` und `:459`): an die Lane-Dichte, also wie viele Gates
feuern, und an `_overlap`. In STEP las `_fire_slice` daraus sowohl die
Roll-Wahrscheinlichkeit als auch die Unterteilung
(`engine/sampler/sampler_engine.cpp:856-857`). Ein dichtes Feld erzwang damit
automatisch 32tel — man drehte die Phrase auf und bekam ein Maschinengewehr dazu.

**Rolls machen den Loop berechenbar.** Ein tempo-gerasteter Wirbel auf jedem tiefen
Offbeat ist beim zweiten Durchlauf vorhersehbar und beim dritten lästig. In der Praxis
will man ihn fast nie.

**COLOR ist in STEP fast wirkungslos.** `_next_ratio` (`sampler_engine.cpp:360`) zieht
pro Fire genau einen Akkordton im Round-Robin. In FLOW verteilt sich das über viele
gleichzeitige Grains und ergibt eine echte Akkordwolke; in STEP, wo seit dem
Slice-Groove ein Fire genau ein Grain spawnt, bleibt davon eine leise
Arpeggio-Andeutung übrig. Der Regler steht auf einem Sampler-Deck praktisch leer.

**Das Material trägt seine Dynamik längst mit sich.** `SliceMap::strength()` speichert
die Anschlagstärke jeder Transiente, ist getestet — und wird von niemandem gelesen. Die
Selbstprüfung der Slice-Groove-Spec hatte das ausdrücklich für ein Akzent-Follow-up
geparkt. Bis dahin wird die Dynamik des Originalmaterials erfasst und weggeworfen.

**Der Polyphonie-Deckel hing am falschen Regler.** `_spawn_slice` deckelt bei
`ceil(_overlap) + kSpawnHeadroom`, also bei DENS-Minimum auf 3 gleichzeitige Grains. Mit
langem LEN (bis 16× Slice) überlebt ein Grain die nächsten Fires, die dann **stumm**
verworfen werden. Man dreht DENS herunter, um weniger Rolls zu bekommen, und verliert
dabei komponierte Noten — am Panel unsichtbar.

## Verhalten

### DENS

FLOW bleibt unverändert: `_overlap` bestimmt dort über `_spawn_every`
(`sampler_engine.cpp:391`) die Wolkendichte, und das ist die legitime Aufgabe des
Reglers.

In STEP liest der Sampler DENS überhaupt nicht mehr. DENS wirkt dort ausschließlich auf
die Phrase — wie tief in die Groove-Rangfolge hinein Noten feuern (`lane.h:22`).

### COLOR = FEEL (nur in STEP)

```
a = COLOR, roher Reglerwert (KEIN MOTION-Swing, siehe unten)
s = strength(k) / 255           Anschlagstärke der eigenen Transiente
gain = lerp(1, lerp(kAccentFloor, 1, s), a)
```

Bei `a = 0` ist `gain` für jedes Grain exakt 1: der Loop läuft flach und mechanisch. Das
ist kein Nebeneffekt, sondern der gewollte Referenzpunkt — man muss den Loop ohne
Akzente hören können.

Bei `a = 1` folgt der Pegel dem Material zwischen `kAccentFloor` und 1. Harte Anschläge
kommen laut, weiche leise; der Loop atmet wie die Aufnahme, ohne dass eine einzige
Zusatznote entsteht.

`kAccentFloor` startet bei `0.35` und ist ausdrücklich ear-tunable: er entscheidet, wie
leise die schwächste Transiente werden darf. Zu tief, und weiche Schläge verschwinden aus
dem Loop; zu hoch, und der Regler tut nichts Hörbares. Der Wert gehört in die Hörprobe,
nicht in eine Rechnung.

**Kein Zufall.** Akzente würfeln nicht, sie folgen dem Material. Das ist der Grund,
warum sie nicht ermüden wie ein Roll: dieselbe Stelle klingt in jedem Durchlauf gleich,
aber verschiedene Stellen klingen verschieden.

**Was `strength` wirklich misst.** Der Detektor speichert nicht den Pegel eines
Anschlags, sondern das fast/slow-Envelope-Verhältnis (`slice_map.cpp:46-53`) — also wie
*plötzlich* ein Onset relativ zu dem ist, was davor lief. Zwei Konsequenzen für die
Hörprobe: ein leiser Schlag nach einer Pause bekommt strength 255 (slow env ≈ 0, "leading
silence maps to full strength"), der erste Hit nach jeder Lücke akzentiert also immer
maximal, egal wie leise er im Material ist. Und die gemessene Verteilung (255/0/1/6) ist
quasi binär — bei FEEL = 1 kann das eher nach "alles geduckt außer den härtesten Hits"
klingen als nach gradueller Dynamik. Beides ist kein Defekt dieser Spec, aber die
Hörprobe muss wissen, dass sie Onset-Schärfe hört und nicht Anschlagstärke — sonst
testet sie etwas anderes, als der Reglername verspricht.

**FEEL liest den rohen Reglerwert, nicht `_color_eff`.** COLOR bekommt in Part einen
MOTION-Swing (`kColorMod`, `part.cpp:214-217`). Für den Akkord ist das gewollt, für die
Akzente nicht: eine atmende Akzent-Tiefe wäre eine versteckte Kopplung genau der Art,
die diese Spec abschafft.

**Grid-Fallback.** Ohne genügend Transienten (`< kMinSlices`) gibt es keine Marker und
damit keine `strength`. Dort bleibt `gain` bei 1, unabhängig von COLOR. Das ist kein
Defekt, sondern die einzig ehrliche Antwort: transientenloses Material hat keine
Anschlagdynamik, die man betonen könnte. Die Spec sagt es ausdrücklich, damit der Regler
auf Flächenmaterial nicht als kaputt gelesen wird.

### Akkord in STEP

`_chord_n` wirkt in STEP als 1: `_next_ratio` nimmt dort immer den gelatchten
Einzelton-Pfad. COLOR bedeutet in STEP damit ausschließlich FEEL und nicht nebenher noch
Akkordgröße — sonst hätten wir das DENS-Problem nur umgezogen.

Nebeneffekt: die im Code dokumentierte Spec-Abweichung bei `_next_ratio`
(`sampler_engine.cpp:347-359` — Chords werden entgegen der Zusage *nicht* am Gate
eingefroren) ist damit eingelöst, weil der Chord-Pfad in STEP nicht mehr erreichbar ist.

### Rolls

Ersatzlos gestrichen. Kein Retrigger unter gehaltener Note, weder auf DENS noch auf
einem anderen Regler.

### Grain-Deckel in STEP

Fest `kStepGrainCeil = 10`, unabhängig von jedem Regler. Der Wert entspricht dem
bisherigen DENS-Maximum (`kOverlapMax + kSpawnHeadroom`), liegt also im bereits auf
Hardware gemessenen CPU-Bereich. Er greift nur noch als Notbremse; leises DENS
verschluckt keine Noten mehr.

## Architektur

### 1. Pegel pro Grain (`engine/sampler/grain.h`)

`Grain::spawn()` bekommt einen `gain`-Parameter, der in `_level()` einmultipliziert wird.
Das ist der gemeinsame Enabler und die einzige Änderung an der Grain-Schicht: ein Float
pro Grain, eine Multiplikation pro Sample. `release()` und `trim_total()` frieren
weiterhin `_level()` ein und erben den Faktor damit automatisch — die Fades bleiben
stetig.

### 2. FEEL-Seitenkanal (Part → Sampler)

`SamplerEngine::set_feel(float)`, gepusht in `Part::_control_tick` neben
`set_overlap`/`set_step_clock` und wie diese auf `_engine_id == ENGINE_SAMPLER` gegated.
Gleiches Idiom, keine neue Mechanik.

`_spawn_slice(k, pan)` kennt den Pool-Index `k` und liest `_slices.strength(k)` selbst.
Im Grid-Modus gibt es kein `k` mit Marker — dort wird `gain = 1` übergeben.

FLOW (`_spawn_one`) übergibt immer `gain = 1`: dort bedeutet COLOR weiterhin Akkord, und
Akzente gibt es nicht.

### 3. Determinismus

Der Draw-Contract in STEP schrumpft auf **zwei Würfe pro Fire: walk, pan**. Der
Retrigger-Pfad und sein Zwei-Wurf-Contract entfallen vollständig. Beide Würfe bleiben
unbedingt und vor jedem Abbruch — ein Fire, das am Deckel gedroppt wird, verbraucht
weiterhin beide.

Akzente ziehen nicht. Der Pegel ist eine reine Funktion aus Reglerwert und Marker.

**Der STEP-Golden-Vector wird dadurch zwangsläufig neu aufgenommen.** Er verliert die
Roll-Zeilen (44 → 12) und die Wurf-Sequenz verschiebt sich um den entfallenen dritten
Wurf. Das ist eine bewusste Neuaufnahme mit Datum und Begründung im Test, kein stilles
Regenerieren. Der FLOW-Golden-Vector bleibt unberührt und muss bit-identisch bleiben.

## Was entfernt wird

- `_retrig_period`, `_retrig_ctr` und der Retrigger-Block in `process()`
- `_walk_ref` samt seiner Reset-Stellen in `punch()` und im Phrase-Wrap — das Feld
  existiert ausschließlich für die Roll-Index-Rechnung (`sampler_engine.cpp:939`)
- der Roll-Cleanup in `set_flow()` und der vorgezogene Disarm in `set_gate()`
- das Lesen von `_overlap` in `_fire_slice` (Wahrscheinlichkeit und Unterteilung)
- die Roll-Tests, der FLOW-Toggle-Test und der DENS-Grenzfall-Test

Damit werden drei Important-Befunde des Schluss-Reviews vom 2026-07-23 gegenstandslos:
der über einen FLOW-Wechsel stehengebliebene Roll (F1), die `_walk_ref`-Asymmetrie beim
Phrase-Wrap (F2) und das Armieren auf einem gedroppten Fire (F3). Sie bewachten alle
dasselbe Feature.

## Bedienfläche

`SAMPLER_LBL` in `host/vcv/res/gen_panel.py` bekommt `("COLOR", "FEEL")`, in derselben
Inline-Form wie SCAN, LEN und ORG.

**Korrektur (2026-07-23, Review):** hier stand, die Beschriftung sei eine Unschärfe, weil
COLOR in FLOW weiterhin die Akkordwolke sei und die Alternative wäre, „die Akkordwolke in
FLOW zu opfern". Das war falsch — es gibt auf einem Sampler-Deck keine Akkordwolke zu
opfern. `Part::_flatten_for_sampler` (`engine/parts/part.h`) klappt den Akkord für
`ENGINE_SAMPLER` **ohne Modus-Prüfung** auf den getriggerten Ton zusammen, auf dem
`set_chord`- wie auf dem Fire-Pfad. Ein Sampler-Deck hatte also auch in FLOW nie einen
Akkord.

Damit ist die Beschriftung FEEL auf einem Sampler-Deck eindeutig richtig. Was statt der
behaupteten Unschärfe tatsächlich gilt: **COLOR tut auf einem Sampler-Deck in FLOW gar
nichts** — der Akkord ist stromaufwärts plattgeklappt, `_feel` wird stromabwärts ignoriert,
weil Akzente STEP-only sind. Das ist **keine Regression dieser Spec**: COLOR war dort schon
vorher wirkungslos, die Spec macht es nur sichtbar.

**Offene Frage an den Instrumentenbauer** (hier bewusst nicht beantwortet): soll ein
totgelegter Regler in FLOW so bleiben, oder soll COLOR dort etwas anderes bekommen? Das ist
eine Gestaltungsentscheidung, keine Reparatur — an FLOW wird in dieser Spec nichts geändert.

## CPU und Speicher (Seed)

Kein zusätzlicher Speicher: der Pegel ist ein Float pro Grain im bestehenden Pool
(16 × 4 Byte = 64 Byte), `strength` liegt bereits in der `SliceMap`.

CPU sinkt netto. Dazu kommt eine Multiplikation pro Grain und Sample; weg fallen der
Retrigger-Zähler im Sample-Pfad und sämtliche Roll-Spawns. Der feste Deckel 10 liegt
nicht über dem bisherigen Maximum, die gemessene Spitzenlast steigt also nicht.

## Tests

- **Akzent-Tiefe:** bei COLOR 0 haben alle Grains Pegel 1; bei COLOR 1 folgt der Pegel
  der `strength` der getroffenen Slice, laute Transiente lauter als leise
- **Grid-Fallback:** auf transientenlosem Material bleibt der Pegel bei 1, für jede
  COLOR-Stellung
- **DENS ist entkoppelt:** eine DENS-Fahrt über den ganzen Weg ändert in STEP weder
  Spawn-Zeitpunkte noch Pegel; die Zahl der Fires ändert sie sehr wohl
- **Deckel:** bei DENS-Minimum und langem LEN werden keine Fires mehr verworfen, die
  vorher verworfen wurden
- **Akkord gepinnt:** in STEP liefert jedes Fire dieselbe Ratio wie der Trigger, für jede
  COLOR-Stellung
- **Kein Retrigger mehr:** unter gehaltener Note spawnt nichts nach, bei keiner
  Reglerstellung
- **FLOW unberührt:** der FLOW-Golden-Vector bleibt bit-identisch
- **STEP-Golden-Vector:** neu aufgenommen, mit Datum, Begründung und einer
  Neuaufnahme-Anleitung im Test
- **Szenario:** `sampler_slice_drums.json` tauscht die DENS-Rampe gegen eine COLOR-Rampe,
  damit die Hörprobe die Akzente zeigt statt der gestrichenen Rolls

## Nicht in diesem Entwurf

- **Micro-Timing / Swing.** Gehört in den Lane-/Step-Clock-Layer, wirkt dort auf beide
  Engines und braucht seinen eigenen Regler-Platz. Eigene Spec.
- **Echter Flam.** Ein Vorschlag *vor* dem Schlag verlangt, den Hauptschlag
  zurückzuhalten — Timing im Sampler nachgebaut statt im Clock. Verworfen aus demselben
  Grund wie Swing.
- **Ghost Notes, Auslassen von Fires.** Beides fügt Ereignisse hinzu oder entfernt sie;
  diese Spec ändert nur, wie laut vorhandene Ereignisse sind.
- **DENS „deckelt" vs. „setzt" die Subdivision.** Die Abweichung zwischen Spec 2026-07-22
  und Implementierung wird mit den Rolls gegenstandslos.
- **Spreizung der `strength`-Werte.** Gemessen wurden 255/0/1/6 über ein Klick-Signal —
  die Dynamik des Detektors ist konstruktionsbedingt schmal. Ob `kAccentFloor` reicht
  oder der Detektor selbst gespreizt gehört, entscheidet die Hörprobe, nicht diese Spec.
  Falls sie durchfällt, sind die Kandidaten benannt, in aufsteigender Eingriffstiefe:
  **(a)** Normalisierung gegen das Map-Maximum (`s = strength(k) / max_strength`) — eine
  Zeile im FEEL-Pfad, spreizt die vorhandenen Werte über den vollen Bereich, ändert den
  Detektor nicht; **(b)** der Detektor misst statt des Envelope-Verhältnisses die
  Peak-Amplitude im Slice-Fenster — das wäre dann tatsächlich Anschlagstärke im Sinne
  des Reglernamens und behebt auch den Stille-davor-Maximalakzent. Beides bleibt
  deterministisch; beides ist eine eigene kleine Änderung, kein Umbau dieser Spec.

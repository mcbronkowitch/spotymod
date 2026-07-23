# Sampler: COLOR streut die Wolke — Tonhöhenstreuung in FLOW

**Datum:** 2026-07-23
**Status:** Entwurf, vom Autor abgenommen
**Voraussetzung:** FEEL-Akzente (Spec 2026-07-23 sampler-feel-accents) sind auf `main`, veröffentlicht als v2.9.0.

## Ziel

COLOR bekommt auf einem Sampler-Deck in FLOW eine Aufgabe: die Streuung der
Wolke. Bei 0 laufen alle Körner unisono wie heute, aufgedreht spreizen sie sich
in der Tonhöhe. Kein neues Bedienelement, kein neuer Rng-Zug — und der Code, der
das tut, existiert bereits vollständig.

## Warum

**Die Streuung ist implementiert und an nichts angeschlossen.** `_spawn_one`
enthält beide Mechanismen, ausgeführt bei jedem FLOW-Spawn:

```cpp
// SUB: a share of grains an octave down. Drawn 4th.
if (_rng.next_unipolar() < _sub_n * kSubMaxShare) ratio *= 0.5f;

// DTUN: per-grain detune spread, +-kDetuneCeilCt at full. Drawn 5th.
const float cents = _rng.next_bipolar() * _detune_n * kDetuneCeilCt;
if (cents != 0.f) ratio *= detune_factor(cents);
```

`set_sub` und `set_detune` werden von niemandem aufgerufen — nicht in
`part.cpp`, nicht in `instrument.h`, nicht im VCV-Host. Beide Felder stehen
dauerhaft auf 0, seit die SUB- und DETUNE-Regler auf einem Sampler-Deck zu LEN
und ORG umgewidmet wurden (Spec 2026-07-21). Der Header sagt es selbst: *"these
stay at their silent 0.f defaults."*

Das ist dieselbe Form wie `SliceMap::strength()` vor der Akzent-Spec:
implementiert, getestet, von niemandem gelesen. Die Konstanten stehen
(`kDetuneCeilCt = 35`, wie beim Synth), die Züge liegen im Vertrag, die Tests für
die Mechanik existieren (`test_sampler_engine.cpp:634-670`). Es fehlt eine
Routing-Zeile.

**Eine Wolke ohne Streuung schillert nicht.** Granulares Material, in dem jedes
Korn auf derselben Ratio läuft, klingt wie verschmiertes Band. Das Schillern —
das leichte Auseinanderlaufen benachbarter Körner — ist das, was Granular von
Zeitdehnung unterscheidet, und der Sampler hat es momentan gar nicht.

**COLOR ist in FLOW leer.** Auf einem Sampler-Deck kollabiert
`Part::_flatten_for_sampler` (`part.h:183`) den Akkord modus-unabhängig auf einen
Ton. COLOR tat dort also noch nie etwas; die Akzent-Spec hat das festgestellt und
als offene Frage stehen lassen. Dies ist die Antwort.

## Verhalten

### COLOR in FLOW: Streuung

Zwei Zonen über den Reglerweg, nicht beide Achsen parallel:

```
a = COLOR nach MOTION-Swing (_color_eff)

untere Hälfte:  detune = clamp(a / kDispersionKnee, 0, 1)
obere Hälfte:   sub    = kSubSpreadMax * clamp((a - kDispersionKnee)
                                               / (1 - kDispersionKnee), 0, 1)
```

`kDispersionKnee = 0.5` — das Knie sitzt auf der Reglermitte, also auf einer
findbaren Position, wie SCANs 1.0x und SIZEs Slice-Unity. Ear-tunable: wenn sich
die Breite zu früh erschöpft anfühlt, wandert es nach oben.

Unten **Breite**: DTUN läuft bis ±35 Cent auf. Die Wolke wird breiter und
lebendiger, bleibt aber eine Farbe — es entsteht kein zweiter Ton, nur ein
Schweben.

Oben **Tiefe**: ab der Mitte mischt sich ein Oktavanteil dazu. Die Wolke bekommt
Gewicht unter dem Grundmaterial.

Bei `a = 0` ist beides exakt 0. Das ist keine Näherung, sondern strukturell und
**schon vorhanden**: `next_unipolar() < 0` ist nie wahr, und `cents` wird exakt
0, was der bestehende `if (cents != 0.f)` überspringt. FLOW bei COLOR 0 ist
bit-identisch mit heute, ohne dass eine neue Sperre nötig wäre.

**Warum der Oktavanteil bei 0.5 endet und nicht bei 1.** `kSubMaxShare = 1`
bedeutet: *jedes* Korn eine Oktave tiefer. Das ist keine Streuung mehr, das ist
eine Transposition — die Wolke klingt dann geschlossen eine Oktave tiefer und
spreizt gar nichts. Die größte Streuung liegt bei einem Anteil von **0.5**: dort
ist die Varianz einer Bernoulli-Verteilung maximal, die Hälfte der Körner unten,
die Hälfte oben. `kSubSpreadMax = 0.5` ist deshalb keine vorsichtige Drosselung,
sondern das Maximum dessen, was das Wort „Streuung" überhaupt meint.

### COLOR in STEP: unverändert

FEEL bleibt, wie es ist — Akzenttiefe aus der Anschlagschärfe, gelesen vom
**rohen** Knopfwert. Die Streuung wirkt ausschließlich in FLOW.

### Die Asymmetrie beim MOTION-Swing, und warum sie richtig ist

STEP liest `_color`, FLOW liest `_color_eff` (Knopf plus MOTION-Swing,
`kColorMod = 0.2`). Das sieht auf den ersten Blick inkonsequent aus und ist es
nicht:

- In **STEP** ist COLOR eine Eigenschaft **einzelner, komponierter Ereignisse**.
  Eine atmende Akzenttiefe wäre eine versteckte Kopplung auf diskreten Noten —
  genau das, was die Akzent-Spec abgeschafft hat.
- In **FLOW** ist COLOR eine Eigenschaft **der Wolke**, und MOTION besitzt dort
  bereits jede andere Streuachse: Position (`kScatterPosFrac`), Pan,
  Spawn-Timing (`kScatterTimeFrac`). Die Tonhöhe davon auszunehmen hieße, eine
  Achse still stehen zu lassen, während die anderen drei atmen. Die Wolke soll
  als Ganzes atmen.

Die Regel dahinter ist nicht „roh oder geschwungen", sondern: **diskrete
Ereignisse bekommen keinen versteckten Swing, kontinuierliche Texturen schon.**

## Bedienfläche

**Die Zweitbeschriftung entfällt ersatzlos.** `("COLOR", "FEEL")` wird aus
`SAMPLER_LBL` (`host/vcv/res/gen_panel.py`) wieder entfernt, und `"COLOR"`
verlässt `SAMPLER_RADIAL` — beides genau die Änderung, die v2.9.0 eingeführt hat,
rückgängig gemacht.

COLOR steht dann für sich. Das Wort bedeutet in beiden Modi dasselbe — *wie viel
Variation* — und verspricht nichts, was in einem der beiden Modi falsch wäre: in
STEP Variation der Pegel, in FLOW Variation der Tonhöhen. Das ist der einzige
Zustand dieser Beschriftung, der in beiden Modi wahr ist, und er kommt zusätzlich
der Hardware-Vorgabe entgegen, Bedienfläche eher zu reduzieren.

**Was das kostet, offen gesagt:** das Panel kündigt die Akzente nicht mehr an.
Wer FEEL sucht, findet es nicht beschriftet. Das ist der bewusst gezahlte Preis
dafür, dass keine Zweitbeschriftung mehr in einem Modus lügt. Und es ist eine
Rücknahme eine Version nach der Einführung — v2.9.0 hat FEEL gebracht, v2.10.0
nimmt das Wort wieder weg. Der Regler behält beide Funktionen; nur die Aufschrift
geht.

## Architektur

### 1. Ein Push, eine Kurve

`SamplerEngine::set_dispersion(float n)` nimmt den Reglerwert und setzt
`_detune_n` und `_sub_n` über die Zonenkurve. Die Kurve gehört in die Engine, wo
die Konstanten liegen, nicht in Part — Part pusht wie bei `set_overlap`,
`set_step_clock` und `set_feel` einen Reglerwert und keine Politik.

`Part::_control_tick` pusht `set_dispersion(_color_eff)` im bestehenden
`_engine_id == ENGINE_SAMPLER`-Block, neben `set_feel(_color)`. Gleiches Idiom,
keine neue Mechanik, zwei benachbarte Zeilen mit *unterschiedlichen* Argumenten —
was den Unterschied genau dort sichtbar macht, wo er entsteht.

`set_sub` und `set_detune` bleiben als Primitive erhalten: die vorhandenen
Engine-Tests (`test_sampler_engine.cpp:634-670`) treiben die Mechanik über sie
und sollen das weiter tun. Dass es damit zwei Wege auf dieselben Felder gibt, ist
eine bekannte Falle (last writer wins). Sie ist hier ungefährlich, weil nur
`set_dispersion` aus Part heraus gerufen wird und die Primitive keinen anderen
Aufrufer haben — aber der Header muss es sagen, sonst ruft es eines Tages jemand
gleichzeitig.

### 2. Determinismus

**Der Draw-Contract ändert sich nicht.** Beide Züge — SUB an vierter, DTUN an
fünfter Stelle — passieren heute schon unbedingt bei jedem FLOW-Spawn,
unabhängig vom Wert. Es kommt kein Zug hinzu, keiner entfällt, keiner wandert.

**Der FLOW-Golden-Vector bleibt gültig und bit-identisch.** Er treibt die Engine
direkt, ohne Part, also wird `set_dispersion` dort nie gerufen und `_sub_n` /
`_detune_n` bleiben 0 — derselbe Zustand, gegen den die Tabelle aufgenommen
wurde. Der STEP-Golden-Vector ist von FLOW-Code ohnehin nicht berührt.

Beide Tabellen dürfen sich **nicht** bewegen. Tun sie es, ist etwas anderes
passiert als beabsichtigt.

### 3. Der Header sagt heute die Unwahrheit

`sampler_engine.h` dokumentiert bei `sub()` / `detune()`: *"SUB and DTUN no
longer reach the sampler (spec 2026-07-21 morphagene-controls); these stay at
their silent 0.f defaults. Exposed so tests can pin the disconnection down."*

Der erste Halbsatz bleibt wahr — die **Regler** SUB und DETUNE erreichen den
Sampler weiterhin nicht, sie heißen dort LEN und ORG. Der zweite wird falsch: die
Felder sind ab dieser Änderung über COLOR erreichbar. Der Kommentar muss beides
auseinanderhalten, sonst liest ihn jemand als Zusicherung und baut darauf.

## CPU und Speicher (Seed)

Nichts Neues. Beide Züge und beide Multiplikationen laufen bereits bei jedem
FLOW-Spawn; sie hatten bisher nur immer den Wert 0. `detune_factor` ist eine
Polynomnäherung ohne `pow` und läuft zur Spawn-Rate, nicht pro Sample. Kein
zusätzlicher Speicher, kein Heap.

Der einzige messbare Effekt: bei `_tape` hängt die Grain-Länge an `ratio`
(`lenf = _grain_len / ratio`), ein oktavtiefes Korn ist also doppelt so lang.
Bei Tape-Modus plus hohem COLOR steigt damit die mittlere Grain-Last. Der
Pool-Deckel (`kGrains`) und die Durchsatzgrenze in `_spawn_one` fangen das ab,
wie sie es für SUB schon immer taten — aber es gehört in die Hörprobe und in eine
Dichte-Telemetrie, nicht in eine Annahme.

## Tests

- **Streuung greift:** bei COLOR 0 haben alle FLOW-Körner dieselbe Ratio; bei
  COLOR 0.5 streuen sie, ohne dass eine Oktave auftaucht; bei COLOR 1 tritt ein
  Oktavanteil auf.
- **Die Knie sitzen:** DTUN erreicht sein Maximum bei `kDispersionKnee`, SUB
  bleibt bis dahin exakt 0 und beginnt erst darüber.
- **Der Oktavanteil deckelt bei `kSubSpreadMax`:** bei COLOR 1 ist ungefähr die
  Hälfte der Körner unten, nicht alle — statistisch über genügend Spawns gepinnt.
- **COLOR 0 ist strukturell still:** kein Spawn weicht in der Ratio ab, für
  beliebig viele Körner.
- **STEP ist unberührt:** eine COLOR-Fahrt ändert in STEP keine Ratio; die
  Akzente bleiben, was sie sind.
- **Part reicht den geschwungenen Wert durch:** bei aktiver MOTION-Lane bewegt
  sich der an die Engine gepushte Streuwert, während der FEEL-Wert am rohen Knopf
  steht — dieselbe Beobachtungsnaht wie im Akzent-Test.
- **Beide Golden Vectors bit-identisch.**
- **Szenario:** `sampler_solo.json` bekommt eine COLOR-Rampe über den ganzen Weg,
  damit beide Zonen nacheinander hörbar werden — erst die Breite, dann der
  Oktavanteil. Es ist das schlankste der vorhandenen FLOW-Szenarien und damit das
  mit dem geringsten Nebengeräusch; `sampler_storm.json` und
  `sampler_texture_deck.json` bringen zu viel Eigenbewegung mit, um eine einzelne
  Achse zu zeigen.

## Nicht in diesem Entwurf

- **STEP-Verhalten jeder Art.** FEEL bleibt unangetastet.
- **Reverse-Wahrscheinlichkeit pro Grain.** `reverse` ist bereits ein
  Per-Grain-Parameter in `Grain::spawn` und heute nur global gesetzt; eine
  Mischung aus vor- und rückwärts laufenden Körnern wäre fast geschenkt und sehr
  hörbar. Eigener, kleiner Entwurf — hier wäre es eine zweite Bedeutung auf
  demselben Regler.
- **Attack-Bindung der Wolke** (Grains auf Transienten starten lassen). Wurde
  erwogen und verworfen: es ist eine Groove-Idee, und in FLOW gibt es keinen
  Groove. Bleibt als Notiz für den Fall, dass der Sampler je einen dritten Modus
  bekommt.
- **Swing / Micro-Timing.** Weiterhin vertagt, gehört in den Lane-/Step-Clock-
  Layer und wirkt dort auf beide Engines.
- **`kDetuneCeilCt` neu vermessen.** 35 Cent stammen vom Synth. Ob eine Wolke
  mehr verträgt, entscheidet die Hörprobe; die Konstante ist ear-tunable und
  bleibt, wo sie ist.

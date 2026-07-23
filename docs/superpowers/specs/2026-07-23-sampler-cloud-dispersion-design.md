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

Oben **Spaltung**: ab der Mitte wandert ein wachsender Anteil der Körner eine
Oktave nach unten, bis bei COLOR 1 die Hälfte unten und die Hälfte oben liegt.
Die Wolke teilt sich dann hörbar in zwei Lagen im Oktavabstand.

Bei `a = 0` ist beides exakt 0. Das ist keine Näherung, sondern strukturell und
**schon vorhanden**: `next_unipolar()` liefert `[0,1)`, also ist `x < 0` nie
wahr, und `cents` wird exakt 0, was der bestehende `if (cents != 0.f)`
überspringt. FLOW bei COLOR 0 ist bit-identisch mit heute, ohne dass eine neue
Sperre nötig wäre.

Diese Bit-Identität hängt allerdings an einer zweiten, weniger sichtbaren
Zusage: `a` ist `_color_eff`, und das ist bei COLOR 0 nur deshalb exakt 0, weil
`kColorGate` (`part.cpp:213-217`, `part.h:289`) den MOTION-Swing unterhalb von
0.01 Reglerweg ausblendet. Ohne dieses Gate würde eine aktive MOTION-Lane
`_color_eff` bei COLOR 0 von der Null wegschieben und die Zusage fiele. Das Gate
ist da — aber es ist ab jetzt für FLOW mittragend, nicht nur für den Akkord.

**Warum der Oktavanteil bei 0.5 endet und nicht bei 1.** `kSubMaxShare = 1`
bedeutet: *jedes* Korn eine Oktave tiefer. Das ist keine Streuung mehr, sondern
eine Transposition — die Wolke klingt geschlossen eine Oktave tiefer und spreizt
gar nichts. Bei einem Anteil von **0.5** ist die Streuung maximal: die Varianz
des Oktav-Indikators (und damit die von `log₂ ratio`) hat dort ihr Maximum.
`kSubSpreadMax = 0.5` ist deshalb keine vorsichtige Drosselung, sondern das
Maximum dessen, was das Wort „Streuung" hergibt.

Was das musikalisch heißt, und zwar ausdrücklich: **das obere Reglerende ist
keine Grundierung, sondern eine Spaltung.** Ein 50/50-Verhältnis hört sich nicht
als eine Wolke mit Gewicht darunter an, sondern als zwei gleichwertige Lagen, die
das Ohr trennt — näher an Organum als an einem Subbass. Wer stattdessen eine
Grundierung will, braucht einen Anteil um 0.3, und dann ist die Varianz nicht
mehr die Begründung. Beides ist verteidigbar; diese Spec entscheidet sich für die
Spaltung, weil FLOW das Ambient-Werkzeug ist und zwei Oktavlagen dort ein
brauchbarer Klang sind. Der Wert ist ear-tunable, die *Geschichte* dahinter darf
aber nicht wechseln, ohne dass die Konstante mitwechselt.

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

COLOR steht dann für sich. Der tragende Grund ist **nicht**, dass „COLOR" in
beiden Modi dasselbe bedeutet — Pegelvariation und Tonhöhenvariation sind nicht
dieselbe Achse, und wer COLOR in STEP aufdreht und Verstimmung erwartet, wird so
oder so überrascht. Der tragende Grund ist die Hardware: das Panel soll auf das
reale Gerät reduzierbar bleiben, und eine Zweitbeschriftung, die nur in einem von
zwei Modi stimmt, ist dort schlechter als gar keine.

Die Alternative, die naheliegt und die deshalb ausdrücklich verworfen wird: die
Beschriftung im VCV-Panel modus-abhängig umschalten. Das geht auf Hardware nicht,
und ein Modul, das mehr verrät als das Instrument, das es prototypisiert, prüft
die falsche Bedienfläche.

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

`set_dispersion` klemmt sein Argument zuerst auf `[0,1]`. `_color_eff` ist zwar
schon geklemmt (`part.cpp:217`) und `set_sub`/`set_detune` klemmen intern, aber
die Zonenkurve rechnet *davor* — ein Wert außerhalb würde die Knie verschieben,
bevor irgendeine Klemme greift.

**`set_sub` und `set_detune` müssen als Primitive erhalten bleiben.** Das ist
keine Höflichkeit gegenüber Bestandscode, sondern eine harte Anforderung: der
FLOW-Golden-Vector ruft beide selbst auf (siehe Determinismus unten). Ein
späterer Aufräumversuch, der sie zugunsten von `set_dispersion` entfernt, bricht
einen Vertrag. Dass es damit zwei Wege auf dieselben Felder gibt, ist die bekannte
Falle (last writer wins); sie ist ungefährlich, weil aus Part heraus nur
`set_dispersion` gerufen wird — aber der Header muss es sagen.

### 2. Determinismus

**Der Draw-Contract ändert sich nicht.** Beide Züge — SUB an vierter, DTUN an
fünfter Stelle — passieren heute schon unbedingt bei jedem FLOW-Spawn,
unabhängig vom Wert. Es kommt kein Zug hinzu, keiner entfällt, keiner wandert.

**Der FLOW-Golden-Vector bleibt gültig — aber nicht aus dem naheliegenden
Grund.** Er läuft *nicht* mit genullten Streufeldern: er setzt selbst
`set_sub(0.4f)` und `set_detune(0.5f)` (`test_sampler_engine.cpp:872-873`),
ausdrücklich damit beide Züge „a mark on the ratio" hinterlassen und die
Zugreihenfolge in einer einzigen Zahl beobachtbar wird. Gültig bleibt die Tabelle
allein deshalb, weil der Test die Engine direkt treibt und `set_dispersion` dort
nie gerufen wird — die Felder behalten exakt die Werte, die der Test ihnen gibt.

Der Unterschied ist nicht akademisch: wer die Tabelle „verifizieren" will, indem
er prüft, dass `sub()` und `detune()` dort 0 sind, findet 0.4 und 0.5, hält das
für die Abweichung und nimmt einen harten Vertrag neu auf. Genau so gehen Golden
Vectors verloren.

Der STEP-Golden-Vector ist von FLOW-Code nicht berührt: `_spawn_slice` liest
weder `_sub_n` noch `_detune_n`, und `_next_ratio` kehrt in STEP vor dem
FLOW-Zweig zurück.

Beide Tabellen dürfen sich **nicht** bewegen. Tun sie es, ist etwas anderes
passiert als beabsichtigt.

### 3. Was STEP doch berührt — eine Naht, die benannt gehört

`set_dispersion` wird wie `set_feel` in beiden Modi gepusht; der
`ENGINE_SAMPLER`-Block ist nicht modus-gegated. Das ist richtig so — die Felder
sind in STEP schlicht wirkungslos, und ein zusätzliches Gate wäre Zustand ohne
Sicherheitsgewinn.

Eine Kopplung besteht trotzdem, und „STEP ist unberührt" ist ohne sie eine Spur
zu stark formuliert: `set_flow` (`sampler_engine.cpp:127-129`) ist eine nackte
Zuweisung und gibt **keine Grains frei**. Laufende FLOW-Körner überleben den
Moduswechsel und belegen Slots, bis die erste fallende Gate-Flanke sie freigibt.
STEPs Deckel ist `kStepGrainCeil = 10` von `kGrains = 16`, also können Reste dort
Noten verschlucken. Das ist **vorbestehend** — jedes lange Tape-Korn tut es schon
heute — und der Oktavanteil verdoppelt die Länge solcher Reste und verbreitert
das Fenster. Begrenzt (eine Gate-Flanke räumt auf), klein, und ausdrücklich
**nicht** Gegenstand dieser Spec. Aber es steht hier, damit niemand die
Isolationszusage für stärker hält, als der Code sie hergibt.

### 3. Der Header sagt heute die Unwahrheit

`sampler_engine.h` dokumentiert bei `sub()` / `detune()`: *"SUB and DTUN no
longer reach the sampler (spec 2026-07-21 morphagene-controls); these stay at
their silent 0.f defaults. Exposed so tests can pin the disconnection down."*

Der erste Halbsatz bleibt wahr — die **Regler** SUB und DETUNE erreichen den
Sampler weiterhin nicht, sie heißen dort LEN und ORG. Der zweite wird falsch: die
Felder sind ab dieser Änderung über COLOR erreichbar. Der Kommentar muss beides
auseinanderhalten, sonst liest ihn jemand als Zusicherung und baut darauf.

Dasselbe gilt für `tests/test_sampler_part.cpp:441` („SUB and DTUN no longer
reach the sampler"). Der Test läuft weiter grün — er ruft nie `process()`, also
feuert kein Control-Tick und die Felder bleiben 0 — aber Titel und Kommentar
werden irreführend, sobald COLOR sie schreiben kann. Er gehört auf das umbenannt,
was er tatsächlich pinnt: dass die SUB/DTUN-**Regler** am Synth landen und nicht
am Sampler. Mit dem Zusatz, dass COLOR ab jetzt der einzige Schreiber dieser
Felder auf einem Sampler-Deck ist.

### 4. Eine Regel, die es wert ist, im Code zu stehen

Die Asymmetrie roh/geschwungen ist kein Sonderfall, sondern ein Grundsatz:
**diskrete Ereignisse bekommen keinen versteckten Swing, kontinuierliche Texturen
schon.** Der Satz gehört als Kommentar an den Push in `Part::_control_tick`, wo
die beiden Zeilen nebeneinander stehen und der Unterschied sonst wie ein Versehen
aussieht.

## CPU und Speicher (Seed)

Nichts Neues, und das ist keine Hoffnung, sondern zweifach im Code begrenzt.

Beide Züge und beide Multiplikationen laufen bereits bei jedem FLOW-Spawn; sie
hatten bisher nur immer den Wert 0. `detune_factor` ist eine Polynomnäherung ohne
`pow` und läuft zur Spawn-Rate, nicht pro Sample. Kein zusätzlicher Speicher,
kein Heap.

Die naheliegende Sorge — im Tape-Modus hängt die Grain-Länge an der Ratio
(`lenf = _grain_len / ratio`), ein oktavtiefes Korn ist also doppelt so lang, und
längere Körner heißen mehr gleichzeitig lebende Körner heißen mehr verstreute
SDRAM-Zugriffe pro Sample — **trägt nicht**:

1. **Der Deckel begrenzt die Anzahl, nicht die Länge.** `_spawn_one` verwirft
   einen Spawn bei `live >= min(ceil(_overlap) + kSpawnHeadroom, kGrains)`
   (`sampler_engine.cpp:503-505`), Maximum **10**. Die Zahl der pro Sample
   gelesenen Körner ist damit von der Grain-Länge unabhängig. Längere Körner
   heben den *Mittelwert* gegen eine Decke, die die Firmware ohnehin aushalten
   muss — die *Spitze* rühren sie nicht an.
2. **Die Länge selbst ist geklemmt.** `len_ceil = _spawn_every * ceiling`
   (`:655`) ergibt bei DENS max `1.25 × _grain_len`. Ein oktavtiefes Tape-Korn
   will `2 ×` und bekommt 1.25. Genau dort, wo die Last zählt, wird es nicht
   einmal doppelt so lang. Der Test „F-10: the tape ceiling binds at one octave
   down" pinnt das bereits.

Dazu kommt, dass die Richtung falsch herum gedacht war: der gemessene Worst Case
auf Zielhardware (`bench/workloads_sampler.cpp`, DENS max + SIZE 0.05 + MOTION 1
+ laufender SCAN) ist das *kurze* Korn mit hoher Spawn-Rate — tausende frischer
Zufallsadressen pro Sekunde. Ein langes, oktavtiefes Korn ist das Gegenteil:
weniger frische Starts, längere zusammenhängende Leseläufe, Schrittweite 0.5
statt 1.0. Streuung verbessert die Lokalität.

**Der reale Preis ist kein CPU-Preis, sondern ein musikalischer.** Längere Körner
füllen den Pool früher, `dropped_spawns` steigt, und die Wolke wird **dünner**,
je weiter COLOR aufgeht. Ein Regler, der „mehr Variation" verspricht und dabei
Dichte wegnimmt, ist die Regression, auf die zu achten ist — nicht die Rechenlast.
Deshalb steht in den Tests eine Messung dafür, und keine Absichtserklärung.

## Tests

Die Zonenkurve ist deterministische Arithmetik und wird auch so geprüft — nicht
über Stichproben. Statistische Anteilstests sind das Langsamste und Wackeligste
in dieser Suite, und sie werden hier nur dort eingesetzt, wo nichts anderes geht.

- **Die Kurve, direkt:** `set_dispersion(x)` → `sub()` / `detune()` an vier
  Punkten geprüft — 0, knapp unter `kDispersionKnee`, genau auf dem Knie, und 1.
  Das pinnt „die Knie sitzen" und „der Anteil deckelt bei `kSubSpreadMax`" exakt,
  in Mikrosekunden, ohne Sampling-Rauschen.
- **Klemmung:** Argumente außerhalb `[0,1]` verschieben die Knie nicht.
- **COLOR 0 ist strukturell still:** über viele FLOW-Spawns weicht keine einzige
  Ratio ab.
- **Ein einziger End-to-End-Test:** bei COLOR 1 tritt mindestens eine halbierte
  Ratio auf. Mehr statistische Prüfung als das braucht es nicht — die Mechanik
  darunter ist bereits durch `test_sampler_engine.cpp:634-670` abgedeckt.
- **STEP ist unberührt:** eine COLOR-Fahrt ändert in STEP keine Ratio; die
  Akzente bleiben, was sie sind.
- **Part reicht den geschwungenen Wert durch:** bei aktiver MOTION-Lane bewegt
  sich der an die Engine gepushte Streuwert, während der FEEL-Wert am rohen Knopf
  steht — dieselbe Beobachtungsnaht wie im Akzent-Test.
- **Beide Golden Vectors bit-identisch.**
- **Dichte-Messung vor dem Merge, keine Vertagung.** Eine COLOR-Rampe im
  Tape-Modus bei DENS max, gerendert, und `dropped_spawns()` / `active_grains()`
  vorher gegen nachher verglichen. Die Telemetrie existiert bereits
  (`sampler_engine.h`); das kostet einen Render. Steigt die Drop-Rate spürbar,
  ist die Wolke am oberen Reglerende dünner statt reicher — dann gehört
  `kSubSpreadMax` nach unten oder die obere Zone in Frage gestellt, **bevor**
  das hier gemergt wird.
- **Szenario:** `sampler_solo.json` bekommt eine COLOR-Rampe, die **vor t = 32 s
  endet** — dort schaltet das Szenario auf STEP (`sampler_solo.json:26`, bei 40 s
  Gesamtlänge), und eine Rampe über den ganzen Weg würde in den letzten acht
  Sekunden FEEL-Akzente zeigen statt Streuung. Das Fenster 10–30 s ist FLOW und
  reicht für beide Zonen nacheinander.

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

# Sampler: Morphagene-nahe Bedienfläche

**Datum:** 2026-07-21
**Status:** Entwurf, vom Autor abgenommen
**Voraussetzung:** M5b (Sampler auf dem VCV-Panel, Branch `sampler-vcv`) muss gemergt sein.

## Ziel

Der Grain-Sampler bekommt vier Regler, die seiner Natur entsprechen — Tonkopfvorschub,
Überlappung, Kornlänge, Leseposition — statt der aus dem Synth geerbten Melodie- und
Phrasenmaschinerie, die im Sampler-Modus wirkungslos ist. Die Tonhöhe wird stabil, damit
Sampler-Material und gespielter Synth in derselben Tonart zusammenklingen.

## Warum

Drei unabhängige Beobachtungen zeigen auf dieselbe Lücke.

**Die Bedienfläche ist zur Hälfte tot.** `PRINCIPLE`, `NEWPHRASE`, `MELODY` und `DENSITY`
wirken nur bei aktivem STEP auf einer melodischen Lane (`mod/lane.cpp:163-166,227,248,260`).
Im FLOW-Sampler — der stehenden Wolke, dem Normalzustand — tun sie nichts. Das widerspricht
dem erklärten Ziel „no dead knobs" (`engine/parts/part.h:79-80`).

**Die Engine kann mehr, als das Panel erreicht.** Die SIZE-Kurve spannt 1 ms bis 42 s
(`sampler_config.h:40-57`), die SOURCE-Lane die volle Materiallänge. Beide hängen an
`_base[]`-Werten, die auf ihrem Boot-Wert 0.5 festsitzen: `set_target_base` existiert
(`part.h:46`, `instrument.h:51`) und wird vom Render-Host benutzt (`host/render/scenario.cpp:118`),
aber aus `host/vcv/` ruft es niemand auf. Die Kornlänge lässt sich von Hand nicht einstellen.

**ATK und DEC sind messbar wirkungslos.** Bei `kOverlap = 8` ist die Überlappung-Addition nahe
an einem Constant-OverLap-Add-System: der Crest-Faktor der Fenstersumme bewegt sich über den
ganzen Reglerweg nur zwischen 1.000 und 1.062 und ist bei ATK = DEC = 1.0 exakt konstant.
Am freistehenden Korn sind es 23 %. Die Fensterform kürzt sich bei hoher Überlappung heraus.
Ein Überlappungsregler macht ATK/DEC hörbar, ohne dass an ihnen selbst etwas geändert wird.

## Bedienfläche

`ENG` schaltet die Engine und damit die Bedeutung von vier Reglern um. Die Parameter-IDs
bleiben unverändert — für die Hardware ist das eine Zusammenlegung, kein Zuwachs.

| Parameter | Synth | Sampler | Bereich im Sampler |
|---|---|---|---|
| `MELODY` (bipolar) | Phrasenprinzip RENEW/LOOP/GROW | **SCAN** | Tonkopfvorschub, siehe Kurve unten |
| `DENSITY` | Groove-Gate-Dichte | **MORPH** | Überlappung 1.0 … 8.0, stufenlos |
| `SUB` | Oktavanteil der Körner | **GENE SIZE** | 1 ms … 42 s (vorhandene SIZE-Kurve) |
| `DETUNE` | Verstimmung ±35 ct | **ORGANIZE** | Leseposition, volle Materiallänge |

Unverändert in beiden Modi: `RATE`, `SHAPE`, `SMOOTH`, `RANGE`, `MOD`, `TUNE`, `ATTACK`,
`DECAY`, `RES`, `FILT`, `FLUX`, `GRIT`, `COMP`, `STEPS`, `COLOR`, `REC` sowie die
FLUX-Anhänge.

`NEWPHRASE` und `TRIGGER` bekommen im Sampler dieselbe neue Bedeutung — **„neues Gen jetzt"**,
siehe Engine-Änderung 4. `PRINCIPLE` bleibt im Sampler ohne Funktion.

`STEP` bleibt der Schalter zwischen stehender Wolke (FLOW) und groove-getriggerten Bursts;
er ruft bereits `set_flow(!on)` auf (`part.cpp:85-89`). Es wird kein neues Bedienelement
hinzugefügt.

Slide (Streuung, MOTION-Lane), Reverse, Tape/Digital und Overdub-Feedback bleiben im
Kontextmenü.

### Beschriftung

`gen_panel.py` bekommt für die vier umgedeuteten Regler eine zweite Beschriftungszeile mit
der Sampler-Bedeutung: `MELO` / `SCAN`, `SUB` / `LEN`, `DTUN` / `ORG`. Beide
Bedeutungen stehen gleichrangig auf der Platte, weil beide gleichrangig gelten.

**`DENSITY` bekommt keine zweite Zeile** und behält `DENS` — das Wort stimmt in beiden
Engines (Groove-Dichte im Synth, Korndichte im Sampler). Es heißt insbesondere nicht
`MRPH`: dieser Name gehört dem globalen A/B-Überblendregler.

Die Kornlänge heißt auf der Platte **`LEN`, nicht `SIZE`** — `SIZE` steht bereits als
Hallgröße in der ROOM-Sektion, und derselbe String an zwei Stellen ist genau die
Verwechslung, die bei MORPH vermieden wurde. `LEN` ist zugleich kürzer und entspannt
damit die engste Stelle des Layouts, den Spalt unter der VOICE-Box.

`NEW` und `TRIG` behalten ihre Beschriftung unverändert — „neu" und „auslösen" treffen die
Sampler-Bedeutung genauso wie die Synth-Bedeutung, eine zweite Zeile wäre nur Lärm.

### Verhalten beim Engine-Wechsel

Die Reglerstellung gilt, unabhängig von der Engine. Kein getrenntes Gedächtnis, kein
einmaliges Setzen beim Umschalten, kein Soft-Takeover.

Die Folge wird ausdrücklich in Kauf genommen: steht `SUB` für den Synth auf null und `ENG`
kippt auf Sampler, dann steht GENE SIZE auf null, also 1 ms statt der 200 ms des heutigen
Vorgabezustands — der erste Umschaltvorgang kann deutlich anders klingen, bis die vier Regler
einmal angefasst werden.

Begründung: es ist das einzige Verhalten, das in VCV und auf der Hardware identisch ist.
Getrenntes Gedächtnis bräuchte auf der Hardware Soft-Takeover, das es dort nicht gibt;
einmaliges Setzen ließe den Hardware-Regler etwas anderes anzeigen, als klingt. Ein Regler,
der lügt, ist schlimmer als einer, der einmal angefasst werden will.

**Das Init-Patch bleibt synth-getrimmt.** Eine Reglerstellung kann nicht beiden Engines einen
guten Startwert geben: GENE SIZE bräuchte 0.5 für die heutigen 200 ms, aber derselbe Regler
ist SUB, und SUB auf 0.5 legt beim Synth die Hälfte der Körner eine Oktave tiefer; ORGANIZE
bräuchte 0.5, das ist DETUNE auf ±17.5 ct. `init.vcvm` wird deshalb **nicht** angepasst
(vgl. `defaultFor()` / `configControls()`).

Der Sampler startet folglich nicht in dem Zustand, der in M5b abgehört wurde, sondern dort,
wo die vier Regler gerade stehen. Das ist die direkte und beabsichtigte Folge dieser
Entscheidung, keine Nachlässigkeit.

**Der eigentliche Preis ist nicht der erste Klang, sondern die fehlende Vorbereitung.** Der
hässliche Moment direkt nach dem Kippen ist formbar — man kann das Deck vorher über LEVEL
oder CHOKE wegziehen. Was nicht geht: den Wechsel vorbereiten. Die vier Regler sind bis zur
letzten Sekunde die Synth-Regler, und SUB schon einmal auf 0.5 vorzulegen verstimmt hörbar
den laufenden Synth. Jeder ENG-Wechsel erzwingt deshalb die Reihenfolge „erst falsch, dann
hindrehen". Für ein Set mit inszenierten Übergängen ist das tragbar; für nahtlose Wechsel
ist es das nicht. Diese Einschränkung wird bewusst in Kauf genommen und steht hier, damit sie
niemanden auf der Bühne überrascht.

## Tonhöhe

`_active[LANE_PITCH]` wird im Sampler-Modus auf `false` gesetzt. Damit liefert `target_raw`
konstant `_base[LANE_PITCH]` (`part.cpp:44-57`) — die Tonhöhe driftet nicht mehr.

Tonhöhe kommt danach ausschließlich aus `TUNE`, dem bipolaren ±18-Halbton-Transpose, der
**vor** dem Quantizer aufsummiert wird (`part.cpp:59-64`), sodass beide Parts auf einem
gemeinsamen Tonleiterraster landen. Das ist der dokumentierte Entwurfszweck dieser Zeile und
genau das gesuchte Werkzeug: ein dorisches Sample in Deck B lässt sich stabil an einen in
A-Dorisch gespielten Synth in Deck A angleichen.

Zwei Dinge überleben diese Abschaltung und sollen es:

- **Rhythmisches Triggern.** `lane_fired(LANE_PITCH)` (`part.cpp:183`) hängt nicht an
  `_active`. Die Lane feuert weiter auf Step-Grenzen, `_gate_ctr` wird gesetzt, `set_gate`
  pulst — der STEP-Modus triggert Bursts wie bisher, nur auf konstanter Tonhöhe.
- **COLOR.** Der Akkord wird weiter gebaut (`part.cpp:221-226`), jetzt über festem Grundton:
  tonleitereigene, gestapelte Schichten desselben Materials statt einer Melodie.

## Engine-Änderungen

### 1. Laufender Tonkopf (neu)

`SamplerEngine` bekommt `set_scan(float bipolar)` und einen Phasenakkumulator, der im
Kontrolltakt (`kCtrlInterval = 96`, ~2 ms bei 48 kHz) vorrückt. Seine Position wird auf das
SOURCE-Ziel addiert.

Damit summieren sich drei Beiträge zur Leseposition: **ORGANIZE** setzt den Grundwert
(`_base[LANE_SOURCE]`), die **Lane** moduliert darum herum, **SCAN** lässt das Ganze driften.
Die Lane-Semantik in `Part` bleibt unangetastet; der Akkumulator lebt vollständig in
`SamplerEngine`.

Eigenschaften:

- Der Akkumulator faltet an der Materiallänge (`_size`), mit derselben O(1)-Faltung wie
  `read_linear` (`sample_buffer.cpp:220`) — kein Subtraktionsschleife, kein `fmodf` auf dem
  Sample-Pfad.
- Er läuft unabhängig vom Spawnen. Auch im STEP-Modus, wo nur auf Gates Körner entstehen,
  rückt der Kopf weiter vor — es ist ein Band, kein Sequencer.
- Er läuft unabhängig vom Aufnehmen. Schreibkopf und Lesekopf sind getrennt.
- Bei `_size == 0` steht er still (nichts zu lesen).
- Der Akkumulator wird in `clear()` und `load_sample()` auf 0 zurückgesetzt.

Kurve, nach dem Vorbild der vorhandenen SIZE-Kurve mit Knicken:

- Reglerbereich −1 … +1, Vorzeichen ist die Richtung.
- `|n| < 0.02`: echter Totbereich, Vorschub exakt 0. Ein eingefrorener Kopf muss auch bei
  Reglerrauschen eingefroren bleiben.
- `0.02 ≤ |n| ≤ 0.75`: exponentiell von 0.001× bis 1.0× Realzeit.
- `|n| > 0.75`: linear von 1.0× bis 8.0× Realzeit.

Das obere Viertel des Reglerwegs trägt damit den Faktor 8 statt 4 — es ist das steilste Stück
der Kurve und der erste Kandidat, falls sich der Regler oben als zu nervös erweist. Die
Gegenmaßnahme wäre dann nicht, den Bereich zurückzunehmen, sondern das obere Segment
ebenfalls exponentiell zu führen. Das steht hier, damit beim Abhören klar ist, welche
Schraube gemeint ist.

Realzeit (1.0×) liegt damit auf einer festen, wiederfindbaren Reglerstellung statt irgendwo
im Verlauf. Nahe der Mitte kriecht der Kopf langsam genug, um ein 42-s-Band über Minuten zu
durchwandern.

### 2. Überlappung wird einstellbar

`kOverlap = 8` (`sampler_engine.h:91`) wird von einer Compile-Zeit-Konstanten zu einem Setter
`set_morph(float n)` mit einem Laufzeitwert. Der Wertebereich ist 1.0 … 8.0, stufenlos.

Das Rechenwerk ist bereits parametrisiert: `spawn_interval(grain_len, overlap)`
(`sampler_engine.cpp:64`) nimmt die Überlappung als Argument, und der Test-Seam
`test_spawn_interval(float, int)` (`sampler_engine.h:23-30`) wurde ausdrücklich angelegt, um
„Überlappungen abzudecken, die die Engine derzeit nicht nutzt". Die Änderung schließt eine
vorbereitete Naht an.

Die Obergrenze bleibt bei 8 und steigt nicht auf `kGrains = 16`: der Pool-Deckel
`len_ceil = _spawn_every × kGrains` (`sampler_engine.cpp:394`) würde oberhalb von 8 anfangen,
die Kornlänge stillschweigend zu beschneiden. Nach unten wird der Deckel lockerer — MORPH zu
senken ist unter allen Umständen sicher.

Die Pegelkompensation `1/sqrt(n)` über die aktiven Körner bleibt unverändert und trägt die
Lautstärkeänderung, die aus einer veränderten Überlappung folgt.

`spawn_interval` bekommt eine `float`-Überladung; die bestehende `int`-Signatur des
Test-Seams bleibt erhalten, damit vorhandene Tests unverändert gelten.

**MORPH bekommt eine Bewegungsquelle.** GENE SIZE und ORGANIZE leben, weil die SIZE- und
SOURCE-Lanes um ihre Grundwerte modulieren; MORPH hinge sonst an keiner Lane und an keinem
Eingang — reiner Handregler, und das als einziges Dichtemittel des Decks. Die MOTION-Lane
wird deshalb zusätzlich auf MORPH gelegt, nach demselben Muster, mit dem sie heute schon
COLOR mitbewegt (`part.cpp:129-134`). Der Reglerwert bleibt der Grundwert, die Lane moduliert
darum herum: die Wolke atmet in der Dichte, statt starr zu stehen.

### 3. Lane-Grundwerte erreichen den VCV-Host

`host/vcv/src/Spotymod.cpp` ruft `Instrument::set_target_base(part, slot, value)` für
`LANE_SIZE` (aus GENE SIZE) und `LANE_SOURCE` (aus ORGANIZE) auf, im selben Kontrollpfad wie
die übrigen Parameter. Zusätzlich wird `_active[LANE_PITCH]` beim Umschalten auf Sampler auf
`false` und beim Umschalten auf Synth zurück auf `true` gesetzt.

Diese Aufrufe geschehen nur im Sampler-Modus. Im Synth-Modus bleibt jeder Pfad exakt so, wie
er ist.

### 4. „Neues Gen jetzt" — die Rückkehr-Geste

`SamplerEngine` bekommt `punch()`: Scan-Akkumulator auf null (der Kopf springt zurück auf
ORGANIZE) und `_spawn_ctr = 0.f` (sofortiger Spawn). Im Sampler-Modus lösen `NEWPHRASE` und
`TRIGGER` diese Geste aus.

Ohne sie hat der Entwurf ein Loch an genau dem Pol, für den er gebaut wird. Position, Ratio
und Länge werden beim Spawn gelatcht (`sampler_engine.cpp:409`), die nächste Gelegenheit
kommt erst nach `_spawn_every`. Bei MORPH 1.0 und GENE SIZE 10 s antworten ORGANIZE, TUNE und
SCAN also bis zu zehn Sekunden lang gar nicht; am oberen Anschlag ist das Deck minutenlang
taub. Das lange, ruhig laufende Korn wäre ohne Rückkehr-Geste kein spielbarer Zustand,
sondern ein Instrument, das nicht antwortet.

`punch()` löst zugleich zwei bestehende Probleme mit:

- **Der Tonkopf lässt sich wiederfinden.** Nach längerem SCAN-Lauf ist die Leseposition
  ORGANIZE plus akkumulierte Phase — der Regler zeigt dann nicht mehr, wo gelesen wird.
  `punch()` stellt den bekannten Zusammenhang wieder her.
- **`TRIGGER` wird im FLOW-Sampler überhaupt erst wirksam.** Heute latcht `trigger_manual`
  (`part.cpp:91-97`) nur `_burst_ratio`, und `_next_ratio` liest den Latch ausschließlich mit
  `!_flow` (`sampler_engine.cpp:226`) — in der stehenden Wolke greift der Taster ins Leere.
  Das ist ein Bestandsfehler aus M5b, den dieser Entwurf mit erledigt.

Die Geste ist als Taster rhythmisch spielbar und braucht keine Splice-Verwaltung.

### 5. Der Kopf wird sichtbar

Der LED-Ring (`SpkyRing` in `host/vcv/src/Spotymod.cpp`) zeichnet heute je einen bewegten
Punkt pro Modulations-Lane. Im Sampler-Modus kommt ein Punkt für die Leseposition dazu,
gespeist aus `last_spawn_pos()` (`sampler_engine.h:161`, existiert bereits).

Ohne Anzeige ist ein driftender Tonkopf blind zu spielen: man weiß nicht, wo im Band man
liest, und findet keine Stelle gezielt wieder. Der Ring ist vorhanden, der Abgriff ist
vorhanden, und auf der Hardware ist dieselbe Anzeige mit demselben Ring machbar.

## Persistenz

Es entstehen keine neuen VCV-Parameter. Alle vier umgedeuteten Regler existieren bereits,
behalten ihre Parameter-ID und werden von Rack wie bisher gespeichert. `PART_STRIDE` bleibt
23, `NUM_PARAMS` bleibt 78.

Ein Patch aus M5b lädt unverändert. Was sich ändert, ist die Wirkung der vier Regler im
Sampler-Modus — und da die Sampler-Bedienfläche mit M5b entsteht und noch nicht veröffentlicht
ist, gibt es keinen Bestand an Patches, der davon getroffen würde.

`test_panel.py` bekommt die neuen Beschriftungen; `PARAM_ORDER` und `LIGHT_ORDER` bleiben
unverändert, weil keine Parameter hinzukommen.

## Prüfung

**Synth-Neutralität.** Die acht angehefteten Szenarien müssen byte-identisch bleiben. Das ist
das schärfste Gatter dieses Entwurfs: keine Änderung darf den Synth-Pfad berühren. `set_morph`
und `set_scan` existieren nur auf `SamplerEngine`; die Lane-Abschaltung und die
`set_target_base`-Aufrufe geschehen ausschließlich im Sampler-Modus.

**Determinismus.** Doppelter Render bleibt byte-identisch.

**Unit-Tests** (doctest, `tests/`):

- `spawn_interval` bei Überlappung 1, 2, 4, 8 — inklusive des `kSpawnMinSamples`-Bodens bei
  kurzem Korn und hoher Überlappung.
- Der Pool-Deckel `len_ceil` wird bei sinkender Überlappung lockerer, nie enger.
- Der Scan-Akkumulator: Richtung, Faltung an der Materialgrenze, Stillstand im Totbereich,
  Stillstand bei `_size == 0`, Rücksetzung durch `clear()`, `load_sample()` und `punch()`.
- Die Scan-Kurve trifft bei `|n| = 0.75` exakt 1.0× und ist an beiden Knicken stetig.
- `_active[LANE_PITCH] = false` lässt `target_raw(LANE_PITCH)` konstant, während
  `lane_fired(LANE_PITCH)` weiter feuert.
- `punch()` erzwingt einen Spawn innerhalb weniger Samples, auch bei MORPH 1.0 und langem
  GENE SIZE, wo das nächste Korn sonst zehn Sekunden entfernt wäre. Das ist der Test, der
  den Nutzen der Geste festhält.
- `TRIGGER` erzeugt im FLOW-Modus einen Spawn. Dieser Test schlägt vor der Änderung fehl —
  er pinnt den Bestandsfehler aus M5b.
- Die MOTION-Lane bewegt die effektive Überlappung um den MORPH-Grundwert.

**Render-Szenarien.** Ein Szenario je neuem Regler unter `host/render/scenarios/`, das seinen
Bereich durchfährt. Diese dienen als Plausibilitätsprüfung und zum Abhören, nicht als
Byte-Gatter.

**Rückgewinnbarkeit des M5b-Klangs.** Bei SCAN in der Mitte, MORPH auf 8.0, GENE SIZE auf 0.5
(= 200 ms) und ORGANIZE auf 0.5 muss der Sampler klingen wie in M5b abgehört und freigegeben.
Das ist eine *Prüfstellung*, kein Startzustand — das Init-Patch landet woanders (siehe
„Verhalten beim Engine-Wechsel"). Der in M5b freigegebene Klang darf durch diesen Entwurf
nicht unerreichbar werden.

**Abhören.** Ob die vier Regler zusammen ein spielbares Instrument ergeben, wird gehört,
nicht gemessen. Insbesondere: ob der Totbereich um SCANs Mitte breit genug ist, ob der
Realzeit-Rastpunkt bei 0.75 sitzt, wo die Hand ihn sucht, und ob MORPH über seinen Bereich
gleichmäßig verläuft oder sich am unteren Ende drängt.

Gezielt abzuhören ist außerdem eine Vermutung aus dem Live-Review: bei MORPH nahe 1.0 und
mittlerem GENE SIZE (100–500 ms) könnte die Fenstersumme im Korntakt pumpen — unsynchron zum
Groove, also weder Wolke noch Band. Falls dieses Loch existiert, ist es genau die Zone
zwischen den beiden gewollten Polen und muss vor dem Merge benannt werden.

## Nicht in diesem Entwurf

- Kein Regler für Slide (MOTION-Lane). Bleibt bei seinem Grundwert und im Kontextmenü.
- Keine Regler für `LANE_LEVEL` und `LANE_MOTION`.
- **Kein Varispeed durch die Null** — Morphagenes Signaturgeste, die glatte, unquantisierte
  Fahrt durch Stillstand hindurch ins Rückwärtige mitsamt Tonhöhe. SCAN fährt die Position
  durch null, aber die Tonhöhe bleibt stehen; TUNE ist auf das Tonleiterraster quantisiert.
  Das ist die direkte Folge der Entscheidung, die Tonhöhe stabil zu halten, damit Sampler und
  Synth in derselben Tonart zusammenklingen. Bewusster Verzicht, kein Versehen.
- **Keine Parameter-CV-Eingänge.** Das Panel hat nur IN L/R, CLOCK und RESET
  (`gen_panel.py:269`); PIT und GAT sind Ausgänge. Externe Modulation der neuen Regler ist in
  VCV nur über Fremdmodule (etwa stoermelder µMap) möglich. Auf der Hardware übernimmt das
  die Gestensteuerung aus M6. Bewusste Lücke.
- Keine Änderung an ATK/DEC selbst. Sie werden durch MORPH hörbar, nicht durch eigene Arbeit.
- Kein Panel-Parameter für Reverse, Tape/Digital oder Feedback. Kontextmenü bleibt.
- Keine Änderung an der Hardware-Gestensteuerung (M6).
- Keine Version, kein Tag. `plugin.json` bleibt bei 2.7.0.

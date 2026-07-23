# Sampler: Slice-Groove — STEP spielt geschnittenes Material

**Datum:** 2026-07-22
**Status:** Entwurf, vom Autor abgenommen
**Voraussetzung:** Morphagene-Bedienfläche (Spec 2026-07-21) ist auf `main`.

## Ziel

Der Sampler groovt in STEP wie die Synth-Parts. Das Material wird an Transienten in
Slices geschnitten, und die Phrase-Engine spielt die Slices, wie sie beim Synth Noten
spielt: Wiederholung aus der Motiv-Struktur, Synkopen aus der Groove-Zelle, hörbare
komponierte Notenlängen, tempo-gerastete Rolls. MOTION wird zur Ordnungsachse zwischen
Loop-Treue (Beat-Repair) und Neuarrangement (Chop-Kit). Kein neues Bedienelement.

## Warum

**Der Sampler ist rhythmisch taub.** Die Phrase-Engine weiß über jeden Schlag: Slot-Index,
metrisches Gewicht (`pg_metric_weight`, `mod/phrase_gen.h:30`), Motiv-Zugehörigkeit
(A A B A, `pg_build_arrangement`), Rank in der Groove-Zelle, komponierte Notenlänge,
Pitch-Kontur. Davon erreicht den Sampler: ein Gate und eine Pitch, die
`_flatten_for_sampler` (`engine/parts/part.h:183`) zusätzlich auf eine Note kollabiert.

**Jeder Schlag granuliert dieselbe beliebige Stelle.** Die Leseposition in STEP ist
`SOURCE * span + _scan_pos + MOTION-Jitter` (`sampler_engine.cpp:541`) — meist mitten im
Sustain irgendeines Materials, ohne Attack. Der Spawn ist sample-tight am Gate, klingt
aber wie ein Fade-in: der Schlag kommt pünktlich und trifft nichts. Der Groove ist da,
er wird nur auf ungeschnittenes Material geworfen.

**Schnelle Grain-Folgen laufen frei.** Das Spawn-Intervall in STEP ist
`_grain_len / _overlap` plus Jitter — es kennt das Tempo nicht. Dichte Bursts rieseln
statt zu rollen.

## Verhalten

### Ein Fire = ein Slice-Grain

Jede gefeuerte Note startet genau ein Grain am Start eines Slices. ATK/DEC fenstern wie
bisher. Die Auswahl des Slices, seine Länge und ob die Note rollt, stehen unten.

### MOTION: die Ordnungsachse

SOURCE und SCAN behalten ihre Jobs: sie setzen die **Basis-Position** im Material
(das heutige `centre` ohne Jitter). Von dort:

- **Ein Slice-Cursor** rückt pro gefeuerter Note einen Slice weiter und springt am
  Phrasen-Wrap zur Basis zurück. Bei MOTION 0 spielt jede Phrase dieselbe Slice-Folge in
  Materialreihenfolge — Loop-Treue und Wiederholung in einem Mechanismus. SCAN schiebt
  die Basis weiter: das Pattern evolviert, bleibt aber tight.
- **MOTION mischt einen Walk dazu:** pro Fire ein Rng-Zug (immer gezogen — feste
  Zugzahl), kubisch geformt wie `pg_contour_walk` (Nachbarschritte häufig, Sprünge
  selten), skaliert mit MOTION × Poolgröße. MOTION 0 multipliziert mit null =
  strukturell stumm (das `kColorGate`-Idiom, `part.h:287`). Voll aufgedreht: freier
  Walk durch den ganzen Slice-Pool.

MOTION ist eine Modulations-Lane — die Phrase kann selbst zwischen ordentlich und
zerhackt atmen.

Der bisherige STEP-Wolkenmodus (frei spawnende Grains unterm Gate) **entfällt** — er ist
die Beschwerde, die dieser Entwurf behebt. FLOW bleibt vollständig unangetastet; dort
ist MOTION weiter der Streuer (Position, Pan, Spawn-Timing).

### SIZE und Notenlänge

- Fensterlänge = **Slice-Länge × SIZE-Kurve**, mit Unity auf findbarer Knopfposition:
  Mitte = genau der Slice, darunter nur der Attack-Zipfel (perkussiv), darüber liest das
  Grain übers Slice-Ende hinaus weiter ins Material. (Analog zum findbaren 1.0x von
  SCAN, `sampler_config.h:239`.)
- Das Gate trägt die komponierte Notenlänge schon heute (`note_sustain`,
  `mod/lane.h:50`). Fällt es, stoppen Retrigger sofort und das laufende Grain geht in
  seinen DEC-Release. Staccato/Sustain der Groove-Zelle werden im Sampler damit zum
  ersten Mal hörbar.
- Das 60-ms-Burst-Release (`kBurstReleaseS`) entfällt in STEP — Grains laufen nicht
  mehr frei, es gibt nichts mehr auslaufen zu lassen.

### Rolls

> **Überholt (2026-07-23):** Rolls wurden ersatzlos entfernt; siehe die Spec
> `2026-07-23-sampler-feel-accents-design.md`. Dieser Abschnitt bleibt als historischer
> Stand stehen.

- Pro Note ein Rng-Wurf, gewichtet mit dem metrischen Gewicht: Downbeats schlagen
  meist einmal, Off-Beats rollen gern.
- DENS deckelt die maximale Subdivision: unten 1 Hit/Note, oben bis Step/8. DENS bleibt
  damit „wie dicht", bekommt aber musikalische Raster statt freiem Geriesel.
- Retrigger-Intervall = `step_samples / subdiv`, **sample-exakt integer**.
- Ein Roll stottert denselben Slice; der MOTION-Walk wird pro Retrigger angewandt und
  ist bei MOTION 0 strukturell stumm — erst hohes MOTION lässt den Roll durchs Material
  wandern.

## Architektur

### 1. SliceMap (im Engine, neben `SampleBuffer`)

Festes Array: **max. 512 Slices × {Start-Frame `uint32`, Stärke `uint8`}**, nach Position
sortiert, ~4 KB SRAM. Kein Heap (Engine-Vertrag). Befüllung:

- **Beim Schreiben:** Envelope-Paar (schnell ~1 ms / langsam ~80 ms) auf dem Frame, der
  tatsächlich im Puffer landet (also inkl. Overdub-Mischung und Feedback). Onset, wenn
  schnell/langsam eine Schwelle mit Hysterese reißt; Refraktärzeit ~40 ms; der Marker
  sitzt ~2 ms **vor** dem Erkennungspunkt, damit der Attack nicht angeschnitten wird
  (geklemmt auf Regionsanfang). Stärke = Ratio am Auslösepunkt. Kosten: ~8 Ops/Sample,
  nur während Recording. Überschreibt der Write-Head beim Overdub eine Region, werden
  deren alte Marker verworfen und aus dem neuen Inhalt ersetzt.
- **Bei `load_sample`:** derselbe Detektor, einmal offline über den Puffer.

Die Schwellen- und Zeitkonstanten sind ear-tunable und landen in `sampler_config.h`
mit Messnotizen, wie dort üblich.

### 2. Takt-Seitenkanal (Part → Sampler)

Sampler-spezifisch, wie `set_sampler_overlap` heute (`part.h:47`) — **kein
`IPartEngine`-Umbau**:

- am Control-Tick: `set_step_clock(step_samples)` — Part rechnet die Step-Dauer aus
  Master-Rate, Division und `clock_scale()` (`mod/lane.h:55`);
- bei jedem Fire: `set_phrase_pos(slot, steps, metric_weight)`.

Die Lane bekommt kleine Accessors (`cur_step()`, `steps()`, Groove-Gewicht des
gefeuerten Slots). Alles deterministisch, kein Rng beteiligt.

### 3. Fallback ohne Transienten

Liefert das Material zu wenige Marker (Drones, Atmos; Schwelle z. B. < 4 Slices), wird
nicht gespeichert, sondern gerechnet: Slices = **Tempo-Raster** in Step-Dauer-Stücken ab
der Basis-Position. Bei MOTION 0 heißt das „spiel das Material der Reihe nach in
Step-Häppchen" — groovt ohne einen einzigen Anschlag im Material. Der Wechsel
Marker↔Raster ist ein Zustand der SliceMap, nicht der Aufrufer.

### 4. Determinismus

> **Überholt (2026-07-23):** die Roll-Züge unten gibt es nicht mehr — der STEP-Vertrag ist
> seit `2026-07-23-sampler-feel-accents-design.md` genau zwei Züge pro Fire (Walk, dann
> Pan). Dieser Abschnitt bleibt als historischer Stand stehen.

Die Rng-Zugfolge des STEP-Pfads ändert sich (Position-Zug entfällt, Walk- und Roll-Züge
kommen dazu) und wird neu dokumentiert und als **neuer Golden Vector** gepinnt — wie die
bestehende Zugfolge-Doku in `_spawn_one` (`sampler_engine.cpp:537`). Feste Zugzahl pro
Fire und pro Retrigger, unabhängig vom Ausgang (das `pg_gen_groove`-Muster: conditional
anwenden, unconditional ziehen). Der FLOW-Pfad bleibt Zug für Zug identisch.

## CPU/Speicher (Seed)

- **Audio-Pfad pro Sample: unverändert.** Gleiche Grain-Reads; Retrigger-Zähler ersetzt
  den Spawn-Zähler. Keine neuen SDRAM-Muster — Slices ändern nur, wo Reads starten,
  nicht wie viele.
- Detektor: ~8 Ops/Sample nur während Recording (im ohnehin laufenden `write()`-Pfad).
- Trigger-Pfad: Binärsuche über ≤ 512 Marker plus wenige Züge — Trigger-Rate, nicht
  Sample-Rate.
- **Nie** zur Spawn-Zeit im 32-MB-SDRAM-Puffer suchen; das wäre genau der verstreute
  Speicherverkehr, der den Seed drosselt.
- 4 KB SRAM für die SliceMap.

## Tests

- **Detektor:** synthetische Klicks → Markerposition ± Toleranz; Refraktärzeit; Pre-Roll
  vor dem Erkennungspunkt; Stärke-Ordnung; Overdub ersetzt Marker der überschriebenen
  Region.
- **Loop-Treue:** MOTION 0 → identische Slice-Folge pro Phrasenzyklus, aufsteigende
  Materialordnung; Wrap-Reset auf die Basis; SCAN verschiebt die Basis.
- **Rolls:** Retrigger-Intervalle exakt `step_samples / subdiv`; DENS-Deckel greift;
  Downbeat-Einzelhit-Bias statistisch gepinnt; Gate-Fall stoppt Retrigger sofort.
- **SIZE:** Unity-Position = exakt Slice-Länge; darunter Zipfel, darüber Überlauf.
- **Fallback:** transientenloses Material → Raster-Slices in Step-Dauer.
- **Determinismus:** neuer Golden Vector der STEP-Zugfolge; FLOW-Renders bit-identisch
  zu vorher.
- **Renders:** zwei Szenarien — Drumloop (Loop-Treue hörbar) und Field Recording
  (Raster-Fallback bzw. Chop-Kit).

## Nicht in diesem Entwurf

- FLOW-Änderungen jeder Art.
- Slice-Bearbeitung von Hand (verschieben, löschen) — die SliceMap ist maschinell.
- Pitch-gesteuerte Slice-Wahl (Kontur → Slice statt Transposition): reizvoll, aber erst
  hören, wie weit Cursor + Walk tragen.
- Velocity/Akzente aus dem metrischen Gewicht (Level pro Grain): eigener, kleiner
  Folgeentwurf, wenn der Groove steht.
- Hardware-Firmware-Anbindung; dieser Entwurf endet am Engine/VCV-Host.

# Step-Clock — STEPS von der Geschwindigkeit entkoppeln

**Datum:** 2026-07-17
**Status:** Design abgenommen, nicht implementiert

## Ziel

RATE ist heute eine **Pattern-Clock**: die Lane-Phase läuft 0…1 pro Zyklus
(`lane.cpp:212`), und STEPS teilt diesen Zyklus nur auf
(`step = phase * _steps`, `lane.cpp:240`). Wer von 8 auf 16 Steps stellt,
halbiert die Step-Dauer — das Pattern wird „doppelt so schnell" statt doppelt
so lang. Damit sind Polymeter zwischen den Decks unmöglich: Deck A mit
8 Steps und Deck B mit 14 Steps bei gleicher RATE laufen nicht gleich schnell
und verschieben sich nicht pro Step.

Dieses Spec macht RATE im STEP-Modus zur **Step-Clock mit 8-Step-Referenz**:

```
zyklus_hz = rate_hz × 8 / steps        (nur STEP-Modus)
```

- Die Step-Dauer ist unabhängig von der Step-Zahl: gleiche RATE ⇒ gleiches
  Step-Tempo auf beiden Decks, egal wie viele Steps.
- 8 vs. 14 Steps bei gleicher RATE ⇒ klassischer Polymeter, Verschiebung um
  6 Steps pro Runde.
- Bei 8 Steps (Panel-Default) ist der Faktor exakt 1.0 — **bit-identisch zu
  heute**. Init-Patch, eingespielte RATE-Positionen und das FLOW↔STEP-Gefühl
  am Default bleiben unverändert (dasselbe Neutralitäts-Prinzip wie
  TIDE-Mitte).
- FLOW ist unberührt: ohne Steps bleibt RATE die Zyklusrate.

Verworfene Alternative: RATE als *pure* Step-Clock (`zyklus = rate/steps`,
Sequencer-Konvention „1/16 = jeder Step ein 16tel"). Semantisch sauberer,
aber alle bestehenden Rate-Stellungen würden im STEP-Modus ~8× langsamer,
Init-Patch und FLOW↔STEP-Parität wären hin. Die 8er-Referenz liefert
denselben Polymeter bei null Bruch am Default.

## Verhalten

### Step-Dauer

Im SYNC-Modus benennt die RATE-Leiter (`divisions.h:12`) weiterhin die Länge
des 8-Step-Referenz-Patterns: „1 bar" ⇒ jeder Step ein Achtel des Takts,
unabhängig davon, ob das Pattern 2 oder 16 Steps hat. Ein 14-Step-Pattern
bei „1 bar" dauert 1¾ Takte und wandert gegen das Raster — das ist der
Sinn der Übung, kein Bug.

### Live-Drehen am STEPS-Knopf: nahtlos

`set_step()` (`lane.cpp:53`) reskaliert beim Wechsel der Step-Zahl die
Phase so, dass Step-Index und Position *innerhalb* des Steps erhalten
bleiben:

```
pos       = phase × alte_steps          // Step-Index + Bruchteil
neue_phase = fmod(pos, neue_steps) / neue_steps
_cur_step  = int(fmod(pos, neue_steps)) // kein Geister-Boundary
```

- 8→16 auf Step 4: bleibt auf Step 4, läuft ohne Sprung bis 16 weiter.
- 16→8 auf Step 10: läuft ab Step 2 weiter (Index mod 8); der
  Boundary-Rhythmus bleibt durchgehend, kein Doppel-Trigger, kein Glitch.
- Die Reskalierung greift nur bei aktivem STEP-Modus; die bestehende
  Regen-Logik (Phrase neu würfeln am Wrap, wenn sich die effektive Länge
  ändert, `lane.cpp:57-61`) bleibt wie sie ist.

### Alle 5 Lanes einheitlich

`SuperModulator::set_step` verteilt Steps schon heute an alle Lanes
(Drone-Regel, `super_modulator.cpp:55`). Die Step-Clock gilt entsprechend
einheitlich: Lane-Ratios (×2, ×½, ×¾, ×1½, `super_modulator.cpp:8`), TIDE
und COUPLE/DRIFT multiplizieren künftig die Step-Clock statt der
Pattern-Clock. Auch die Textur-Lanes behalten damit beim STEPS-Drehen ihre
gefühlte Geschwindigkeit.

### Bewusste Nebenwirkungen

- **Pro-Runde-Ereignisse werden seltener pro Zeit:** VARIATION/RENEW-Würfe,
  EVOLVE-Walks und Phrase-Regen hängen am Phase-Wrap (`lane.cpp:216-237`).
  Ein 16-Step-Pattern wrappt halb so oft wie heute — gleich oft *pro
  Phrase*, seltener *pro Sekunde*. Musikalisch gewollt: Variation atmet mit
  der Phrasenlänge.
- **STEP↔FLOW bei ≠8 Steps ändert die gefühlte Geschwindigkeit:** FLOW
  läuft auf der Zyklusrate, STEP auf der Step-Clock; nur bei 8 Steps sind
  beide identisch. Folgt direkt aus der Semantik.

## Implementierung

Eine Stelle: `ModLane` speichert die rohe Rate (`_rate_hz`) zusätzlich zum
Inkrement und rechnet `_phase_inc` neu, wenn sich Rate, Step-Modus oder
Step-Zahl ändern:

```
_phase_inc = (_rate_hz / _sr) × (_step_mode ? 8.f / _steps : 1.f)
```

Betroffen: `set_rate_hz()` (`lane.cpp:48`) und `set_step()`
(`lane.cpp:53`, inkl. Phase-Reskalierung). `SuperModulator`, `Part`,
`Instrument` und der VCV-Host bleiben unangetastet — kein neues Control,
kein Panel-Impact (Hardware-Constraint ✓). Der STEPS-Knopf bleibt 2…16
(`Spotymod.cpp:95`); `kSeqSlots = 32` und die zugehörige Clamp-Logik sind
unberührt.

## Tests

1. **Regression:** 8 Steps ⇒ bit-identische Ausgabe zu vorher (Faktor
   exakt 1.0).
2. **Konstante Step-Dauer:** gleiche Rate, 8 vs. 16 Steps ⇒ identische
   Boundary-Intervalle; Zykluslänge verdoppelt.
3. **Polymeter:** 8 vs. 14 Steps, gleiche Rate ⇒ Boundaries bleiben
   deckungsgleich, Verschiebung um 6 Steps pro 14er-Runde.
4. **Nahtloses Umstellen:** STEPS-Wechsel mitten im Pattern ⇒ kein
   Phase-Sprung, `_cur_step` konsistent, kein Doppel-Trigger auf dem
   Umschalt-Sample; 16→8 wrappt den Index korrekt.
5. **FLOW unberührt:** Step-Zahl ändert im FLOW-Modus weiterhin nichts an
   der Rate.

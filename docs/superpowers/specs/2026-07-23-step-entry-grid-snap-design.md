# STEP-Einstieg schnappt aufs Raster

**Datum:** 2026-07-23
**Status:** Entwurf zur Umsetzung

## Das Problem

Mitten in der Performance von der Wolke auf den Rhythmus umzuschalten trifft
den Takt nicht. Die Pitch-Lane des Decks läuft frei gegen den Takt — nicht
zufällig, sondern gewollt: `Center::_rebase_grid` (`center.cpp:195`) schiebt bei
einer STEPS-Drehung das Grid-*Ziel* auf die aktuelle Phase, damit der harte
Servo nicht am Tempo zerrt (Rack-Bericht 2026-07-17: STEPS-Drehungen
beschleunigten und bremsten hörbar). Der Preis steht im Kommentar auf
`center.cpp:194`: „Der Offset persistiert — die Schleife läuft frei gegen den
Takt — bis RST resynct."

Damit gibt es heute genau eine Geste, die das geraderückt: RST (Buchse oder
Menüpunkt „Resync loops to bar"). Die ist für diesen Moment zu grob. Sie ruft
`Instrument::reset_transport()` (`instrument.h:188`), und das nullt den
Downbeat **und** setzt die Phasen **beider** Decks auf 0 — es verschiebt also
das Raster gegen eine laufende externe Clock und reißt das andere Deck mit,
das gerade spielt.

## Das Verhalten

Beim Wechsel eines Decks von FLOW nach STEP schnappt genau dieses Deck auf die
Position, an der der Takt gerade steht. Steht der Transport auf Zählzeit 3,
setzt das Deck auf 3 ein — nicht auf 1 und nicht erst beim nächsten Taktanfang.
Es klingt ab dem ersten Schritt das, was geklungen hätte, wäre das Deck die
ganze Zeit im Takt mitgelaufen.

Nichts sonst bewegt sich: der Transport behält seinen Downbeat, die externe
Clock bleibt unangetastet, das andere Deck merkt nichts davon.

### GRID-Welt (SYNC an)

Das Ziel ist dieselbe Zahl, gegen die der Servo ohnehin rechnet
(`center.cpp:161-164`):

```
tgt = frac(transport.beats() * kDivisions[div].cpb * clock_scale)
```

Drei Zuweisungen für das schaltende Deck `i`:

1. `_grid_off[i] = 0.f` — der Free-Run-Offset entfällt, das Ziel ist wieder das
   reine Transport-Raster.
2. Pitch-Lane-Phase := `tgt`, über `ModLane::reset(float)` (`lane.h:76`).
3. Slice-Cursor := die zur neuen Phase gehörende Phrasenposition, also
   `pitch_cur_step()` **nach** Schritt 2 gelesen (siehe unten). Das gilt nur
   für ein Deck mit geladener Sampler-Engine; auf einem Synth-Deck entfällt
   Schritt 3 ersatzlos, die Schritte 1 und 2 gelten dort unverändert.

Weil die Phase *auf* das Ziel gesetzt wird statt umgekehrt, ist der Servofehler
ab dem ersten Sample 0. Kein Zerren, kein Tempo-Wobble — die Umkehrung von
`_rebase_grid`, mit derselben Begründung.

### Freie Welt (SYNC aus)

Ohne Transport gibt es kein Raster, an dem sich „gerade bei 3" festmachen
ließe; die Decks hängen dort nur über die Kuramoto-Kopplung aneinander. Dann
ist das andere Deck die Referenz: Pitch-Phase := `pitch_phase()` des anderen
Decks (roh, dieselbe Größe, aus der `center.cpp:100` `_phase_err` bildet).
`_grid_off` spielt in diesem Zweig keine Rolle und wird nicht angefasst.

### Der Slice-Cursor

`_cursor` zählt Slices seit dem Phrasen-Wrap und geht im Grid-Modus direkt in
den Slice-Index ein (`k = (_cursor + wo) % pool`, `sampler_engine.cpp:808`).
Beim Schnappen auf Zählzeit 3 soll Slice 3 klingen, nicht Slice 1 — sonst läge
das Material dauerhaft gegen die Phrase versetzt, bis der nächste Wrap es
geraderückt.

Der Sampler bekommt dafür eine Methode, die Cursor und Wrap-Erkennung
gemeinsam setzt:

```cpp
void SamplerEngine::snap_phrase_cursor(int slot);
```

Sie setzt `_cursor = slot`, `_walk = 0.f` und **`_last_slot = slot`**. Das
letzte ist keine Kosmetik: ohne es sieht `_fire_slice` beim nächsten Feuern
einen rückwärts gesprungenen Slot, erkennt das als Phrasen-Wrap und nullt den
Cursor sofort wieder (`sampler_engine.cpp:786`) — die Ausrichtung wäre beim
ersten Schritt wieder weg. `_walk` wird mitgenullt, weil die beiden im
Wrap-Pfad ebenfalls immer zusammen gesetzt werden.

Die Methode zieht **keinen** Rng-Zug. Der Draw-Contract bleibt unverändert.

## Architektur

Part meldet den Wunsch, Center führt ihn aus.

`Part::set_step` (`part.cpp:86`) wird bei jedem Control-Tick mit dem
Schalterzustand aufgerufen, nicht nur beim Wechsel — der Vergleich gegen
`_step_on` liefert die steigende Flanke. Part setzt daraufhin ein Einmal-Flag
und weiß sonst nichts vom Transport.

`Center::update(SuperModulator& a, SuperModulator& b, Part& pa, Part& pb)`
(`center.cpp:72`) bekommt beide Modulatoren und beide Parts bereits übergeben,
läuft im Control-Tick und ist der Ort, an dem die gesamte Rasterpolitik liegt
(`_rebase_grid`, `_grid_servo`, `_grid_off`). Es konsumiert das Flag —
unmittelbar neben den `_rebase_grid`-Aufrufen, die dieselbe Größe pflegen — und
löscht es.

Damit bleibt die Regel gewahrt, nach der Part Werte schiebt und keine Politik
hält (dieselbe Begründung wie beim COLOR-Push in spec 2026-07-23
sampler-cloud-dispersion). Die Verzögerung beträgt höchstens einen Control-Tick
(96 Samples, ~2 ms).

## Nicht-Ziele

- **Nur die PITCH-Lane springt.** Die vier Textur-Lanes laufen weiter. Ein
  Sprung dort wäre ein hörbarer Ruck in Filter und Pan, der dem Rhythmus nicht
  hilft. (RST setzt alle Lanes — das ist eine andere Geste mit einem anderen
  Zweck und bleibt, wie sie ist.)
- **Nur die Richtung FLOW→STEP.** Der Rückweg in die Wolke bleibt unverändert;
  die Wolke hat kein Raster, auf das sie schnappen könnte.
- **Kein neues Bedienelement, keine Menüoption.** Die Geste ist der
  Moduswechsel selbst — das Panel bleibt auf die reale Hardware reduzierbar.
- **RST bleibt unangetastet.** Downbeat-Null für alles bleibt genau das, was es
  heute ist.
- **Kein Engine-Verhalten außerhalb des Umschaltmoments.** Läuft ein Deck
  bereits in STEP, ändert sich nichts — insbesondere bleibt der Free-Run nach
  einer STEPS-Drehung erhalten.

## Vertrag und Risiken

- **Beide Golden Vectors müssen bit-identisch bleiben.** Sie treiben die Engine
  direkt, ohne Center; der Snap läuft dort nie. Bewegt sich einer: STOP und
  melden, nicht neu aufnehmen.
- **Der Draw-Contract ändert sich nicht.** `snap_phrase_cursor` zieht nicht.
- **Kein Heap, kein libDaisy in `engine/`.** Das Flag ist ein `bool` im Part.
- **Kein `std::pow`/`exp2f` pro Sample.** Der Snap läuft im Control-Tick und
  feuert einmal pro Moduswechsel.

## Prüfbarkeit

Die Mechanik ist deterministisch und gehört in Tests, nicht in einen Render:

1. **GRID:** Transport auf eine bekannte Position bringen, Deck in FLOW frei
   laufen lassen (`_grid_off` ungleich 0 erzwingen, wie es eine STEPS-Drehung
   tut), auf STEP schalten, einen Control-Tick rechnen. Danach muss die
   Pitch-Phase gleich `tgt` sein und `_grid_off` für dieses Deck 0.
2. **Kein Servo-Zerren:** über mehrere Ticks nach dem Snap darf die
   Rate-Skalierung des Decks nicht vom Servo weggezogen werden — der Fehler
   startet bei 0 und bleibt dort. Das ist die eigentliche Zusage gegenüber
   `reset_transport`, und der Punkt, an dem ein naives „nur `_grid_off` nullen"
   auffliegt.
3. **Fremde Zustände unberührt:** Downbeat des Transports und Phase des anderen
   Decks vor und nach dem Snap identisch.
4. **Freie Welt:** SYNC aus, beide Decks mit auseinanderlaufenden Phasen, Deck B
   auf STEP — danach steht B auf As Phase.
5. **Cursor:** nach einem Snap auf Slot n feuert der nächste STEP-Fire den
   Slice, der zu n gehört (MOTION 0, damit `_walk` strukturell 0 ist), und der
   Cursor wird nicht durch die Wrap-Erkennung sofort wieder genullt.
6. **Nur eine Flanke:** ein zweiter Control-Tick mit unverändertem STEP-Schalter
   löst keinen zweiten Snap aus.

## Offen, nach dem Hören zu entscheiden

- Ob der Snap auch dann richtig liegt, wenn die Phrase länger als ein Takt ist
  (STEPS gegen Division): dann ist „die Position, an der der Takt steht"
  eindeutig, aber sie liegt mitten in der Phrase. Erwartet ist genau das; ob es
  sich im Rack so anfühlt, sagt erst das Ohr.

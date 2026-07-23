# Sampler: drei Dinge, die im Spiel stören

**Datum:** 2026-07-23
**Status:** Entwurf zur Umsetzung

Drei unabhängige Befunde aus dem Spielen, in einem Durchgang umzusetzen. Sie
teilen sich kein Bauteil und können in beliebiger Reihenfolge gebaut werden:

1. **Der STEP-Einstieg trifft den Takt nicht** — der Wechsel von der Wolke auf
   den Rhythmus landet irgendwo.
2. **Die SCAN-Kurve verschenkt den halben Regler** — untenrum kriecht sie,
   obenrum springt sie.
3. **MOD wirft die Leseposition zu früh durch das Material** — schon eine
   Prise MOD reicht.

Punkt 1 ist eine Architekturänderung, 2 und 3 sind Kurven- und Tiefen-Tuning.

---

# 1. STEP-Einstieg schnappt aufs Raster

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

---

# 2. SCAN: linear unterhalb des Knies, Maximum 4x

## Das Problem

Die untere Zone ist exponentiell: `kScanMinRate * (1/kScanMinRate)^t` über den
Weg `kScanDead .. kScanKnee`, also 0.001x bis 1.0x über 0.02–0.75
(`sampler_engine.cpp:47-58`). Damit liegt bei halb aufgedrehtem Regler eine
Rate von **0.09x** an — der Kopf kriecht, und der gesamte Weg zwischen Mitte
und Dreiviertel tut fast nichts. Direkt darüber trägt das letzte Viertel den
Faktor 8 und ist die steilste Strecke der Kurve. Der Regler hat damit zwei
unbrauchbare Zonen und kaum eine spielbare dazwischen.

## Die Änderung

Drei Eingriffe, sonst nichts:

1. **Untere Zone linear** statt exponentiell: von `kScanMinRate` an der
   Totzonenkante geradlinig auf 1.0x am Knie. Bei halb aufgedreht dann ~0.66x
   statt 0.09x.
2. **`kScanMaxRate` von 8 auf 4.** Das nimmt dem obersten Viertel den
   Sprungcharakter am wirksamsten; die ganz schnellen Sweeps entfallen dafür.
3. **Alles andere bleibt:** `kScanDead` = 0.02 (die Halteschwelle exakt in der
   Mitte, damit ein eingefrorener Kopf unter Reglerrauschen eingefroren
   bleibt), `kScanKnee` = 0.75 (Echtzeit bleibt auf einer findbaren Position),
   das obere Segment bleibt linear, das Vorzeichen bleibt die Richtung.

`kScanMinRate` behält seine Bedeutung als Wert an der Totzonenkante — die
Gerade startet dort, nicht bei 0. Bei 0 zu starten würde die Kante der Totzone
verwischen: direkt daneben stünde der Kopf praktisch ebenfalls still.

Nebeneffekt: aus `scan_rate()` entfällt ein `std::pow`. Die Funktion lief schon
immer nur auf Control-Rate (`sampler_engine.h:163`), das ist also kein
Laufzeitargument, sondern eine Vereinfachung.

## Was mitwandern muss

Der Kommentarblock in `sampler_config.h:232-243` beschreibt die alte Kurve und
schreibt ausdrücklich fest: *„if it plays too nervously, the fix is an
exponential top segment, not a smaller range"*. Genau diese frühere Entscheidung
wird hier umgestoßen. Der Block muss die neue Form UND die neue Begründung
tragen, sonst widerspricht die Datei sich selbst — mit dem Hinweis, dass das
kleinere Maximum eine bewusste Rücknahme ist und keine übersehene Zeile.

## Vertrag und Risiken

- **Beide Golden Vectors bleiben bit-identisch.** Zu verifizieren, nicht
  anzunehmen: nach heutigem Stand ruft keiner der beiden `set_scan`, `_scan_rate`
  bleibt dort 0. Bewegt sich einer: STOP und melden.
- Der vorhandene Kurventest (`test_sampler_engine.cpp:1703-1733`) prüft gegen
  die Konstanten **beim Namen** — Totzone, 1.0x am Knie, Maximum bei ±1,
  `kScanMinRate` an der Totzonenkante, Stetigkeit über das Knie, Monotonie über
  den ganzen Weg. Alle diese Zusicherungen überleben die Änderung; keine pinnt
  die exponentielle Form. Der Test darf deshalb NICHT gelockert werden.
- **Zu ergänzen:** ein Fall, der die Linearität unterhalb des Knies festhält —
  ohne ihn ist die eigentliche Änderung durch nichts abgesichert, und die
  nächste Kurvenidee kippt sie unbemerkt zurück. Die Mittelwert-Eigenschaft
  reicht: der Wert in der Mitte zweier Reglerpositionen ist der Mittelwert ihrer
  Raten.

---

# 3. MOD auf die Scan-Position: quadratisch

## Das Problem

Die Leseposition entsteht als `clampf(_targets[LANE_SOURCE], 0, 1) * span +
_scan_pos + jitter` (`sampler_engine.cpp:548`) — die SOURCE-Lane greift also
über die **gesamte** Aufnahme. Ihr Modulationsanteil ist
`lane_output * MOD * _tdepth[LANE_SOURCE]` (`part.cpp:48`), mit
`_tdepth[LANE_SOURCE]` = 0.55 als Default (`part.h:273`).

Bei MOD 0.3 wandert die Position damit um ±16.5% des Materials: auf einer
10-Sekunden-Aufnahme ±1.65 s. Dass eine Prise MOD die Position durchs Material
wirft, ist also kein Fehler im Detail, sondern die lineare Kennlinie selbst.

## Die Änderung

Auf einem Sampler-Deck geht die Master-MOD **quadratisch** in die SOURCE-Lane:
statt `MOD` wirkt `MOD²`. Bei MOD 0.3 bleiben ±0.5 s statt ±1.65 s, bei MOD 0.5
±0.7 s. Bei voll aufgerissener MOD ändert sich exakt nichts (1² = 1) — das
Ausbrechen bleibt erhalten und rückt nur ans obere Reglerende, wo es hingehört.

Die Stelle ist `Part::target_raw` (`part.cpp:47`), wo bereits die Ausnahme für
die PITCH-Lane steht (*„the PITCH lane is the anchor"*) — also die etablierte
Stelle für die Frage, welche Tiefe für diese Lane gilt.

**Nur SOURCE, nur Sampler-Decks.** Auf einem Synth-Deck bedeutet SOURCE etwas
anderes und bleibt unangetastet; die übrigen Lanes ebenso. `_tdepth` bleibt
unverändert — es ist die vom Nutzer gesetzte Ziel-Tiefe, nicht der Ort für eine
Kennlinie.

Der Exponent gehört als benannte Konstante nach `sampler_config.h`, mit
Kommentar, dass er ear-tunable ist. Zu prüfen bei der Umsetzung: `part.cpp`
inkludiert nur `parts/part.h` — ob `sampler_cfg` dort überhaupt sichtbar ist,
entscheidet, wo die Konstante stehen kann. Sie in Part zu duplizieren wäre
falsch; dann gehört sie an eine Stelle, die beide sehen.

## Vertrag und Risiken

- **Beide Golden Vectors bleiben bit-identisch.** Sie treiben die Engine direkt
  ohne Part; `target_raw` läuft dort nie. Bewegt sich einer: STOP und melden.
- Der Draw-Contract ändert sich nicht — es wird nichts gezogen.
- **Zu prüfen:** ob ein bestehender Test die lineare MOD-Kennlinie auf der
  SOURCE-Lane festhält. Findet sich einer, ist sein Umbau eine gemeldete
  Entscheidung, keine stille Anpassung.
- Zu ergänzen: bei MOD 0.5 beträgt die SOURCE-Auslenkung ein Viertel statt der
  Hälfte; bei MOD 1 ist sie exakt unverändert (die Zusage, dass oben nichts
  verloren geht); ein Synth-Deck bleibt bei allen MOD-Werten unverändert.

## Offen, nach dem Hören zu entscheiden

Ob quadratisch weit genug biegt. Kubisch war die Alternative (bei MOD 0.3 nur
±0.15 s); die Entscheidung fiel bewusst auf die sanftere Kurve, weil sie das
untere Drittel nicht ganz stillstellt. Fühlt es sich im Rack immer noch zu
nervös an, ist der Exponent die Stellschraube — nicht `_tdepth`.

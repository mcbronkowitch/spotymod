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
Es setzt ab dem ersten *klingenden* Schritt dort ein, wo es gestanden hätte,
wäre das Deck die ganze Zeit im Takt mitgelaufen. „Ab dem ersten Schritt"
heißt nicht „sofort hörbar": ist der getroffene Slot bei niedriger DENSE
gerastet oder maskiert, schweigt das Deck bis zum nächsten gegateten Schritt
(`_on_boundary` liest `_effective_gate`, `lane.cpp:288-294`). Das ist richtig
so — der Snap setzt die Position, nicht die Gate-Politik.

Nichts sonst bewegt sich: der Transport behält seinen Downbeat, die externe
Clock bleibt unangetastet, das andere Deck merkt nichts davon.

### GRID-Welt (SYNC an)

Das Ziel ist dieselbe Zahl, gegen die der Grid-Servo rechnet
(`Center::_grid_servo`, `center.cpp:206-218`):

```
tgt = frac(transport.beats() * kDivisions[div].cpb * clock_scale + _grid_off[i])
```

Drei Zuweisungen für das schaltende Deck `i`, **in dieser Reihenfolge**:

1. `_grid_off[i] = 0.f` — der Free-Run-Offset entfällt, das Ziel ist wieder das
   reine Transport-Raster. Muss zuerst passieren: `tgt` wird danach mit dem
   genullten Offset berechnet, sonst landet der Snap um genau den alten Offset
   daneben.
2. Pitch-Lane-Phase := `tgt`.
3. Slice-Cursor := der zu `tgt` gehörende Slot (siehe unten). Nur auf einem Deck
   mit Sampler-Engine; auf einem Synth-Deck entfällt Schritt 3 ersatzlos, die
   Schritte 1 und 2 gelten dort unverändert. Schaltet das Deck gerade die Engine
   um (`_switching`), zählt die **Ziel**-Engine (`_pending_engine`) — sonst
   verlöre ein Wechsel „auf Sampler und auf STEP zugleich" die Ausrichtung.

Weil die Phase *auf* das Ziel gesetzt wird statt umgekehrt, ist der Servofehler
ab dem ersten Sample 0. Kein Zerren, kein Tempo-Wobble — die Umkehrung von
`_rebase_grid`, mit derselben Begründung.

**Reihenfolge im Tick:** der Moduswechsel ändert `clock_scale` (1 in FLOW,
8/S in STEP, `lane.h:55`) und löst damit im selben `Center::update` einen
`_rebase_grid`-Aufruf aus (`center.cpp:74-75`, `:196-198`). Der Snap muss
**nach** den beiden `_rebase_grid`-Aufrufen laufen, sonst überschreibt der
Rebase den genullten Offset gleich wieder. Das ist eine Zusicherung, die der
Plan festnageln muss, keine glückliche Fügung.

### Freie Welt (SYNC aus)

Ohne Transport gibt es kein Raster, an dem sich „gerade bei 3" festmachen
ließe; die Decks hängen dort nur über die Kuramoto-Kopplung aneinander. Dann
ist das andere Deck die Referenz: Pitch-Phase := `pitch_phase()` des anderen
Decks (roh, dieselbe Größe, aus der `center.cpp:100` `_phase_err` bildet, und
dieselbe, aus der `lane.cpp:288` die Schrittgrenzen ableitet — roh auf roh
nullt Phasenfehler und Schrittraster zugleich).

Drei Randfälle, die entschieden sind:

- **Beide Decks schalten im selben Tick.** Deck A ist die Phasenreferenz des
  Paars (so nennt es `center.cpp:155-157` selbst): A bleibt stehen, B schnappt
  auf A. Ohne diese Regel schnappten beide auf die jeweils andere Vorher-Phase
  und tauschten sie nur.
- **COUPLE = 0.** Der Snap feuert trotzdem. Was der Moduswechsel zusagt, darf
  nicht an der Stellung eines anderen Reglers hängen; A bleibt auch ungekoppelt
  eine definierte Referenz.
- **`_grid_off[i]` wird auch hier genullt**, obwohl es in der freien Welt
  niemand liest. `_grid_servo` rechnet `target = beats*cpb + off`, ein
  genullter Offset ist deshalb die fehlerfreie Wahl (ein später
  eingeschaltetes SYNC beginnt bei Fehler 0); ein rebase-artiger (nicht
  genullter) Offset wäre die Variante, an der ein später eingeschaltetes SYNC
  zöge. Genullt wird trotzdem, weil das zu `reset_transport()`s "auf den Takt
  ausrichten"-Konvention passt (`center.h:33`). Damit lautet die Regel
  schlicht: immer nullen, dann je nach Welt die Phase setzen.

### Nebenwirkungen von `ModLane::reset`

`reset` setzt nicht nur die Phase: es nullt `_note_age` und `_note_hold` und
setzt beide Slews (`lane.cpp:133-140`). Auf einem melodischen Synth-Deck fällt
`gate_state()` damit bis zum nächsten Fire auf false — eine gehaltene Note
bricht im Umschaltmoment ab. Das ist hingenommen: der Moduswechsel ist ohnehin
ein Schnitt, und ein eigener Phasen-Setter ohne diese Nebenwirkungen wäre ein
zweiter Weg in denselben Zustand.

### Der pitch-only Hook

`SuperModulator::reset_phases()` (`super_modulator.h:71-77`) taugt nicht: es
setzt **alle** Lanes und ist damit die RST-Geste. Gebraucht wird ein neuer
Einstieg, der nur die PITCH-Lane setzt:

```cpp
void SuperModulator::snap_pitch_phase(float ph);
```

Er setzt die Pitch-Lane über `ModLane::reset(ph)` **und nullt den
Onset-Gap-Ring** (`_since_onset`, `_onsets`, `_gap[]`, `_rhythm`) — genau die
Kopplung, auf der der Kommentar an `reset_phases` besteht. Ohne das misst der
erste Onset nach dem Sprung einen Abstand, den es nie gab, und dieser
Rhythmus-Blick steuert über `Instrument` die FX-Abgriffe des **anderen** Decks
(`instrument.cpp:81-86`) — ein Deck, das von diesem Snap nichts merken soll.
Die vier Textur-Lanes bleiben unberührt.

### Der Slice-Cursor

`_cursor` zählt Slices seit dem Phrasen-Wrap und geht im Grid-Modus direkt in
den Slice-Index ein (`k = (_cursor + wo) % pool`, `sampler_engine.cpp:808`).
Beim Schnappen auf Zählzeit 3 soll Slice 3 klingen, nicht Slice 1 — sonst läge
das Material dauerhaft gegen die Phrase versetzt, bis der nächste Wrap es
geraderückt.

**Der Slot lässt sich nicht zurücklesen.** Der naheliegende Weg — Phase setzen,
dann `pitch_cur_step()` fragen — ist versperrt: `ModLane::reset` setzt
`_cur_step` ausdrücklich auf **−1** (`lane.cpp:133-140`), und der Wert entsteht
erst wieder im nächsten `process()`/`tick()` (`lane.cpp:288-294`), also nach
`Center::update`. Zurückgelesen käme −1 heraus, `_cursor` und `_last_slot`
stünden auf −1, und der erste Fire spielte `k = (-1 + wo) % pool`, also den
*letzten* Slice statt Slot n.

Der Slot wird deshalb aus `tgt` selbst berechnet, mit derselben Formel, die die
Lane benutzt (`lane.cpp:288-290`):

```
slot = min(int(tgt * steps), steps - 1)
```

Damit die beiden nicht auseinanderlaufen können, gehört diese Zeile an **eine**
Stelle — ein kleiner Helfer an der Lane, den sowohl `process()` als auch der
Snap ruft. Zwei Kopien derselben Rundungsregel sind genau die Art Duplikat, die
später still divergiert.

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

1. **GRID:** Transport auf eine bekannte Position bringen, Deck mit einem
   Offset ungleich 0 in FLOW laufen lassen, auf STEP schalten, einen
   Control-Tick rechnen. Danach muss die Pitch-Phase gleich `tgt` sein und
   `_grid_off` für dieses Deck 0. **Achtung beim Aufbau:** eine STEPS-Drehung
   *in FLOW* erzeugt den Offset nicht — `clock_scale()` ist in FLOW immer 1
   (`lane.h:55`), und `_rebase_grid` feuert nur bei dessen Änderung
   (`center.cpp:196-197`). Der Offset entsteht durch den Moduswechsel selbst
   oder durch Drehungen im STEP-Modus.
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
7. **Kein Snap beim Laden.** Hosts pushen `set_step` in jedem Tick, und
   `_step_on` bootet auf false (`part.cpp:29`) — ein Patch, der mit STEP an
   geladen wird, erzeugt beim ersten Push eine steigende Flanke. Dort gab es
   keine Geste und keine Wolke, aus der man käme: die erste Beobachtung des
   Schalters nach `init()` setzt nur den Zustand, ohne zu schnappen.

**Zum Testaufbau:** das Rig in `test_center.cpp:12-19` reicht `SuperModulator`s
herein, die **nicht** `pa.mod()` / `pb.mod()` sind — anders als im echten
`Instrument`. Der Snap muss deshalb auf den übergebenen Referenzen `a` / `b`
arbeiten, sonst lassen sich die Fälle 1–4 gegen dieses Rig gar nicht schreiben.

**Erwartete Kollateraltreffer:** mehrere bestehende Tests schalten mitten im
Lauf von FLOW auf STEP, etwa `test_sampler_part.cpp:181` (SYNC aus, also der
Freie-Welt-Zweig) und die Annahme „STEP-Einstieg: Schritt −1 → 0" bei
`test_instrument.cpp:283-286`. Ihre Zusicherungen sind weit genug gefasst, dass
sie voraussichtlich halten — aber wenn dort etwas rot wird, ist das ein Befund
und keine Störung, und gehört gemeldet statt umgebogen.

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

Und noch vier Stellen außerhalb der Engine, die von der alten Kurve erzählen:

- `host/vcv/README.md:66` und `:78-81` dokumentieren die exponentielle Form,
  die 8x und die Init-Patch-Notiz „−8x realtime". Mit dem neuen Maximum wird
  daraus −4x, und der dort genannte Reglerwert −0.728 ergibt künftig ~−0.97x
  statt −0.81x — die beschriebene Wirkung ändert ihren Charakter, nicht nur
  ihre Zahl.
- `host/vcv/src/Spotymod.cpp:461-464` begründet ein Sampler-Gate (K-03) mit dem
  `std::pow` im exponentiellen Zweig. Genau dieses `pow` entfällt hier. Das Gate
  darf bleiben, seine Begründung muss neu geschrieben werden.
- `bench/workloads_sampler.cpp:178-226` samt Eintrag in `bench/run.py:276` misst
  eigens dieses `std::pow`. Der Workload verliert seine Prämisse — ihn stehen zu
  lassen, als messe er noch etwas, wäre die schlechtere Variante. Ob er
  umgewidmet oder entfernt wird, ist eine gemeldete Entscheidung.
- Der Testfallname `test_sampler_engine.cpp:1699` sagt „tops at 8x", der
  Kommentar bei `:1915-1917` spricht vom „sub-knee exponential segment". Beide
  Zusicherungen überleben (sie prüfen gegen die Konstanten), die Wörter nicht.

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
`lane_output * MOD * _tdepth[LANE_SOURCE]` (`part.cpp:48`), und
`_tdepth[LANE_SOURCE]` ist **1.0** (`part.h:273` in Verbindung mit
`LANE_SOURCE = 0`, `lane_id.h:8`): die SOURCE-Lane läuft ungedämpft. Die
gestaffelten 0.55 und 0.7 gehören zu FILTER und MOTION, nicht hierher.

Bei MOD 0.3 wandert die Position damit um ±30% des Materials: auf einer
10-Sekunden-Aufnahme ±3 s. Dass eine Prise MOD die Position durchs Material
wirft, ist also kein Fehler im Detail, sondern die ungedämpfte lineare
Kennlinie selbst.

## Die Änderung

Auf einem Sampler-Deck geht die Master-MOD **quadratisch** in die SOURCE-Lane:
statt `MOD` wirkt `MOD²`. Bei MOD 0.3 bleiben ±0.9 s statt ±3 s, bei MOD 0.5
±1.25 s statt ±5 s. Bei voll aufgerissener MOD ändert sich exakt nichts
(1² = 1) — das Ausbrechen bleibt erhalten und rückt nur ans obere Reglerende,
wo es hingehört.

**Das gilt in beiden Modi.** `_targets[LANE_SOURCE]` speist über `_base_pos()`
(`sampler_engine.h:306-314`) auch die Slice-Basisposition in STEP, nicht nur
die Wolke in FLOW. Die Kennlinie dort auszunehmen hieße, denselben Regler je
nach Modus verschieden tief wirken zu lassen — genau die versteckte Kopplung,
die die FEEL-Spec abgeschafft hat. Soll STEP anders, ist das eine eigene
Entscheidung mit eigener Begründung.

Die Stelle ist `Part::target_raw` (`part.cpp:47`), wo bereits die Ausnahme für
die PITCH-Lane steht (*„the PITCH lane is the anchor"*) — also die etablierte
Stelle für die Frage, welche Tiefe für diese Lane gilt.

**Nur SOURCE, nur Sampler-Decks.** Auf einem Synth-Deck bedeutet SOURCE etwas
anderes und bleibt unangetastet; die übrigen Lanes ebenso. `_tdepth` bleibt
unverändert — es ist die vom Nutzer gesetzte Ziel-Tiefe, nicht der Ort für eine
Kennlinie.

Der Exponent gehört als benannte Konstante nach `sampler_config.h`, mit
Kommentar, dass er ear-tunable ist. Die Sichtbarkeit ist geprüft: `part.h:10`
inkludiert `sampler/sampler_engine.h`, das wiederum `sampler_config.h` zieht —
`sampler_cfg` ist in `part.cpp` sichtbar, die Konstante braucht kein zweites
Zuhause.

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

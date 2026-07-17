# FILT — bipolarer Cutoff-Trim pro Part

**Datum:** 2026-07-17
**Status:** Design abgenommen (v2 — HP-Hälfte gestrichen, rechte Hälfte öffnet,
linker Anschlag endet in Stille), nicht implementiert

## Ziel

Der klassische Cutoff-Regler fehlt am Instrument. Die Cutoff-Frequenz kommt
heute **ausschließlich** aus der Modulator-Lane von Pad 2 (SIZE/FILTER,
`LANE_SIZE`), exponentiell 60 Hz…14 kHz (`synth_engine.cpp:16-19, 107`) — sie
bewegt sich nur, wenn der Super-Modulator sie bewegt. Der RES-Knopf steuert
allein die Resonanz des Voice-SVF (`voice.cpp:62`) und ist von der Cutoff
vollständig entkoppelt.

`FILT` ergänzt den fehlenden Handgriff: ein bipolarer Cutoff-Trim pro Part,
der mit der FILTER-Lane **summiert**. Links zudrehen (mit Resonanz), rechts
öffnen — und der linke Anschlag darf als Sound-Design-Geste **komplett in
Stille enden**. Kein neuer Filter: der vorhandene Voice-SVF macht die gesamte
Arbeit, die Stille-Blende reitet auf dem vorhandenen Level-Smoother.

Drei verankerte Punkte, **unabhängig davon, wo die Lane gerade steht**:

| Stellung | Garantie |
|---|---|
| ganz links (−1) | Part ist stumm |
| Mitte (0) | bit-identisch zu heute |
| ganz rechts (+1) | Filter voll offen (14 kHz) |

## Bedienung

- **Panel:** kleiner bipolarer Knopf `FILT` in der Voice-Row, links neben RES.
- **Wertebereich:** −1…+1, Default 0 (Mitte = Funktion aus; Doppelklick fährt
  auf die Mitte zurück).
- **Richtung:**
  - links von Mitte → **Cutoff fährt zu**, RES singt am wandernden Cutoff mit;
    unter der 60-Hz-Schiene blendet der Part in Stille aus
  - Mitte → **bit-identisch zu heute**
  - rechts von Mitte → **Cutoff öffnet** über die Lane-Stellung hinaus

Der Name ist `FILT`, nicht `TONE`: „TONE" ist auf dem Panel bereits durch
`REV_TONE` im Center-Strip belegt (`gen_panel.py:138`). `FILT` benutzt
dasselbe Wort wie die FILTER-Lane, die denselben Filter bewegt.

## Kennlinie

Sei `t` der Reglerwert −1…+1. Gerechnet wird **nicht in Hertz, sondern im
normalisierten Lane-Raum**, in dem `filter_hz()` ohnehin exponentiell ist:

```
offset = t < 0 ? kFiltLeftScale * t : t      // kFiltLeftScale = 1.25
n_raw  = lane_n + offset                     // darf < 0 und > 1 laufen
cutoff = filter_hz(n_raw)                    // clampt intern auf 0..1
gain   = clamp(1 + n_raw / kFiltFadeRange, 0, 1)   // kFiltFadeRange = 0.25
```

Das ist das klassische „Cutoff-Knopf + Modulationstiefe summieren sich"-
Verhalten, plus eine Blende unterhalb der Schiene:

- **Beißt ab der ersten Bewegung**, unabhängig davon, wo die FILTER-Lane
  gerade steht. Keine wandernde Totzone (genau daran ist die Deckel-Variante
  gescheitert, siehe unten).
- **Links unter der 60-Hz-Schiene** (`n_raw < 0`) fährt die Cutoff nicht
  weiter — stattdessen blendet `gain` den Part linear aus, vollständig stumm
  bei `n_raw ≤ −0.25` (≈ 2 „virtuelle" Oktaven unter der Schiene). Mit hoher
  RES klingt das als Resonanz-Ring, der in Stille wegkippt — die gewünschte
  Sound-Design-Geste.
- **Invariante:** `kFiltLeftScale ≥ 1 + kFiltFadeRange` garantiert Stille am
  linken Anschlag selbst im Worst Case `lane_n = 1` (dort ist
  `n_raw = 1 − 1.25 = −0.25`, die Blende also gerade vollständig zu). Wer die
  Konstanten später verstellt, muss die Invariante mitnehmen.
- **Rechts** genügt `offset = t`: bei `t = +1` ist `n_raw ≥ 1` für jede
  Lane-Stellung → 14 kHz, voll offen. Der Knick in der Steigung am
  Nulldurchgang (1.25 vs. 1.0) ist unhörbar.
- Steht die Lane bereits tief, erreicht der Regler die Stille früher und die
  letzten Grad tun nichts mehr. Das ist die übliche Schienen-Logik, nur dass
  die Schiene jetzt „stumm" heißt statt „60 Hz".

## Engine-Architektur (portabel, kein Host-Wissen)

Kein neuer Filter, keine neue Voice-API — die Änderung lebt vollständig in
`SynthEngine`.

### Push-Kette

Exakt analog zu RES (`instrument.h:76` → `part.h:68` → `synth_engine.cpp:153`):

- `Instrument::set_voice_filt(int p, float t)` (−1…+1, geclampt)
- `Part::set_voice_filt(float t)` → `_synth.set_filt(t)`
- `SynthEngine::set_filt(float t)` → Member `_filt_amt` (Boot-Default `0.f`)

### SynthEngine

Neue Konstanten und Member:

```cpp
static constexpr float kFiltLeftScale = 1.25f;   // linke Hälfte übersteuert …
static constexpr float kFiltFadeRange = 0.25f;   // … um die Blendzone (Invariante!)
float _filt_amt  = 0.f;   // Reglerwert -1..+1
float _filt_gain = 1.f;   // Stille-Blende, control-rate berechnet
```

In `SynthEngine::_update_control()` an der vorhandenen Cutoff-Stelle
(`synth_engine.cpp:107`):

```cpp
const float off    = _filt_amt < 0.f ? kFiltLeftScale * _filt_amt : _filt_amt;
const float n_raw  = _targets[LANE_SIZE] + off;
const float cutoff = filter_hz(n_raw);            // clampt intern auf 0..1
_filt_gain = clampf(1.f + n_raw / kFiltFadeRange, 0.f, 1.f);
```

In `SynthEngine::process()` reitet die Blende auf dem vorhandenen
Level-Smoother (`synth_engine.cpp:138`):

```cpp
const float gain = _level.process(_targets[LANE_LEVEL] * _filt_gain) * kVoiceGain;
```

- **Klickfrei gratis:** `_filt_gain` ändert sich nur alle 96 Samples, aber der
  10-ms-`OnePole` (`_level`, `synth_engine.cpp:34`) glättet jede Stufe —
  derselbe Weg, den das LEVEL-Target heute schon nimmt.
- **Bit-Identität bei `t = 0` fällt gratis ab:** `off = 0` → `n_raw = lane_n`,
  also dieselbe Cutoff wie heute; `_filt_gain = 1.f`, und `x * 1.0f == x` ist
  in IEEE-754 exakt. Kein Sonderfall nötig.
- **Null zusätzliche Per-Sample-Kosten:** eine Multiplikation pro Part und
  Sample, die ohnehin in der Gain-Zeile steht. Auf der Daisy irrelevant.

### Voice

Unangetastet. Kein neuer Member, kein neuer Signalpfad.

### Verhalten unter der Blende

Der Part wird **ausgeblendet, nicht angehalten**: Envelopes, Gates, die
PITCH/GATE-CV-Ausgänge und die CHOKE-Fenster (die auf `voice_env` schauen)
laufen unter der Stille weiter. Gewollt — der Sequencer treibt das Rack
weiter, und beim Aufdrehen ist der Part sofort wieder da, mitten in seiner
Phrase. Nebenwirkung: ein stumm gefilterter Part **choked weiterhin** den
anderen. Im Play-Test gegenprüfen, ob das musikalisch überrascht (siehe
Offene Risiken).

## Host: VCV

### Panel

Die Voice-Row wächst von 5 auf 6 Knöpfe, Pitch bleibt 13 mm, weiter mittig auf
der Ring-Achse (x = 42):

```
heute:   16    29    42    55    68            (5 @ 13 mm)
         ATK   DEC   RES   SUB   DTUN

neu:    9.5  22.5  35.5  48.5  61.5  74.5      (6 @ 13 mm)
        ATK   DEC  FILT   RES   SUB   DTUN
FX-Row:      22.5  35.5  48.5  61.5            <- liegt jetzt exakt drauf
```

Nebeneffekt, der das Panel besser macht: Heute stehen Voice- und FX-Row um
einen halben Pitch versetzt. Mit sechs Slots landet die FX-Row exakt auf den
Slots 2–5 (`FILT_A` bei 35.5 sitzt direkt über `GRIT_A` bei 35.5) — das Raster
wird kohärenter als vorher.

Geometrie geprüft:

- Linker Knopf endet bei x = 6.5 (Kragen 5.5); Accent-Bar liegt bei x 0…1.4,
  Schrauben bei y = 3 / 125.5 (`gen_panel.py:257-258`) — kein Konflikt mit
  y = 76.8.
- Rechter Knopf endet bei x = 77.5; der Center-Streifen beginnt bei x ≈ 85.7
  (`gen_panel.py:197`).
- Part B spiegelt automatisch (`W - x`); `FILT_B` liegt bei x = 177.86.
- Die Accent-Farbe (Solder-Grün / Kupfer) folgt automatisch aus der
  x-Position (`gen_panel.py:195-199`).

### Param-IDs — Patch-Kompatibilität

`FILT_A`/`FILT_B` werden **ans Ende von `PARAMS` angehängt**, nicht ins
`part_controls()`-Template. Grund: `PART_STRIDE = len(PART_A)`
(`gen_panel.py:112`), und `PARAMS = PART_A + PART_B + SHARED`
(`gen_panel.py:148`) — ein Knopf mehr im Template erhöht den Stride und
verschiebt damit **alle** Part-B- und SHARED-Param-IDs. Jedes existierende
`.vcv`-Patch und der `init.vcvm`-Snapshot würden brechen. Dieselbe
Ausweichbewegung hat CHOKE schon gemacht (`gen_panel.py:142-145`).

Listen-Reihenfolge und Panel-Position sind ohnehin entkoppelt — die Koordinate
setzt FILT trotzdem optisch in die Voice-Row:

```python
VOICE_X = [9.5, 22.5, 35.5, 48.5, 61.5, 74.5]   # Slot 2 = FILT (ans Ende von PARAMS)

# in part_controls(): Slot 2 auslassen, Enum-Reihenfolge unverändert
for i,(enum,lbl) in zip([0,1,3,4,5],
                        [("ATTACK","ATK"),("DECAY","DEC"),("RES","RES"),
                         ("SUB","SUB"),("DETUNE","DTUN")]):
    out.append(Ctl(enum, SMKNOB, VOICE_X[i], 76.8, lbl))

PARAMS = PART_A + PART_B + SHARED + [
    Ctl("FILT_A", SMKNOB, VOICE_X[2],     76.8, "FILT"),
    Ctl("FILT_B", SMKNOB, W - VOICE_X[2], 76.8, "FILT"),
]
```

### C++

- `configControls()`: Sonderfall vor dem generischen `configParam(0, 1, …)`
  (`Spotymod.cpp:71-72`), analog zum CHOKE-Zweig (`:65-70`):
  ```cpp
  else if (c.id == FILT_A || c.id == FILT_B)
      configParam(c.id, -1.f, 1.f, 0.f, lbl);
  ```
  Widget bleibt Trimpot (`WK_SMKNOB`). Präzedenzen: CHOKE für angehängte IDs
  mit Sonderfall in `configControls()`; MELODY (`KNOBC`) für einen
  kontinuierlichen Bipolar-Regler — nur eben als großer Knopf.
- `defaultFor()` wird **nicht** angefasst. FILT ist wie CHOKE in
  `configControls()` special-cased und erreicht `defaultFor()` nie. Wichtig:
  `defaultFor()` rechnet mit `id / PART_STRIDE` (`Spotymod.cpp:131-132`) und
  würde für die angehängten IDs Unsinn liefern.
- `pushParams()`: eine Zeile, neben `inst.set_voice_resonance(p, pp(RES_A, p))`
  (`Spotymod.cpp:177`). **Kein `pp()`** — FILT liegt außerhalb des Part-Stride:
  ```cpp
  inst.set_voice_filt(p, params[p ? FILT_B : FILT_A].getValue());
  ```
- Init-Patch: Default 0 → bestehende Patches klingen unverändert.
- `python3 res/gen_panel.py` aus `host/vcv/` neu laufen lassen (SVG +
  `generated_panel.hpp` sind beide committed).

## Host: Desktop-Render (Szenarien)

`host/render/scenario.cpp:112-114` mappt Actions auf die Instrument-API
(`set_voice_attack`, `set_voice_resonance`, …). FILT braucht dort eine Zeile:

```cpp
else if (a == "set_voice_filt") inst.set_voice_filt(e.part, e.value);
```

Ohne sie kann kein Szenario den neuen Regler rendern — und über diese
Szenarien laufen die Abhör-Tests.

## Hardware-Reduzierbarkeit

**Dies ist ein Zugeständnis, kein Gewinn.** Die Projekt-Konstante lautet
„lieber mergen als hinzufügen"; FILT fügt ein Poti pro Seite hinzu (2 gesamt,
Voice-Row 5 → 6).

Die Merge-Variante wurde bewertet und verworfen (siehe unten): Cutoff in RES zu
falten koppelt Resonanz und Cutoff so, dass „volle Resonanz bei offenem Filter"
unerreichbar wird — ein realer Klangverlust für einen Panel-Gewinn von einem
Poti. Auf der realen Hardware bleibt die Voice-Row mit sechs Potis auf dem
13-mm-Raster abbildbar.

## Verworfene Alternativen

Bewusst durchgespielt und verworfen — nicht erneut aufrollen ohne neuen Grund:

- **Rechte Hälfte als Highpass (v1 dieses Specs).** Eigene `daisysp::Svf`-
  Stufe pro Voice (20 Hz…2 kHz), Crossfade am Nulldurchgang gegen den
  Bypass-Thump. Verworfen (2026-07-17): 8 zusätzliche SVFs laufen permanent —
  auf der Daisy ein realer Budget-Posten für ein Nice-to-have; dazu
  Crossfade-Logik, die nur langsame Reglerfahrten schützt (Patch-Load springt
  über die Zone), und eine Voice-Änderung. Der Cutoff-Trim deckt den
  eigentlichen Bedarf („der fehlende Cutoff-Knopf") ohne all das.
- **Betrag von RES = Resonanz, äußere Viertel machen Cutoff.** Spart das Poti
  und der Init-Default bliebe klanggleich. Verworfen: „volle Resonanz bei
  offenem Filter" wird unerreichbar, Resonanz und Cutoff sind zwangsgekoppelt.
- **RES wird ganz zum Filter-Regler, Resonanz fest verdrahtet.** Verworfen:
  Resonanz-Kontrolle geht verloren.
- **Deckel: `cutoff = min(lane_hz, lid_hz)`, lid 14 kHz → 60 Hz.** Verworfen:
  Der Deckel beißt erst, wenn er die Lane erreicht — steht die Lane bei 2 kHz,
  passiert von `t = 0` bis ≈ −0.35 nichts; steht sie bei 500 Hz, bis ≈ −0.6.
  Eine Totzone, deren Größe an einem Regler an ganz anderer Stelle hängt.
  Fühlt sich wie ein kaputter Regler an.
- **Oktaven-Shift: `cutoff = lane_hz * 2^(-oct)`.** Verworfen: läuft unten in
  den 20-Hz-Clamp des Voice-SVF und bleibt dort hörbar stehen. Der
  normalisierte Offset mit Stille-Blende ist das sauberere Ende.
- **Cutoff unter 60 Hz weiterfahren statt Blende (Floor von `filter_hz`
  absenken).** Verworfen: Der SVF-Clamp liegt bei 20 Hz (`voice.cpp:61`); ein
  12-dB-Lowpass bei 20 Hz lässt einen 40-Hz-Sub nur ~12 dB leiser — „stumm"
  ist so nicht erreichbar, und die Resonanz bei 20 Hz wummert. Die Blende auf
  dem Level-Smoother erreicht echte Stille, klickfrei.
- **Eigene Stereo-Stufe auf dem Part-Bus (DJ-Filter), Voice-SVF unangetastet.**
  Verworfen: keine Resonanz beim Zudrehen, und der Eingriff läge an einem
  anderen Punkt der Kette (hinter GRIT).
- **CV-Eingang für FILT.** YAGNI: Panel ist voll, und die FILTER-Lane moduliert
  den Cutoff bereits.
- **Snap / Rasterung auf die Mitte.** Nicht nötig: Default 0 heißt, Doppelklick
  fährt auf die Mitte zurück.
- **Custom `ParamQuantity` fürs Tooltip** („zu 63 %" / „off" / „offen +0.4").
  Nice-to-have, jederzeit nachrüstbar; v1 zeigt rohe −1…+1-Werte wie MELODY.

## Tests

Zwei Nahtstellen, nach vorhandenen Mustern:

**Instrument-Ebene** — Regression nach dem Muster von `test_choke.cpp:101`:

- **Neutralität:** `set_voice_filt(p, 0)` → Output bit-identisch zu einer
  unberührten Instrument-Instanz. Deckt Cutoff-Pfad (`n_raw = lane_n`) und
  Blende (`_filt_gain = 1`) gleichzeitig ab.
- **Symmetrie:** `FILT_A` und `FILT_B` wirken je nur auf ihren Part.

**SynthEngine-Ebene** — Kennlinien-Tests brauchen gepinnte Lane-Werte; auf
Instrument-Ebene wandern die Lanes generativ. `test_synth_engine.cpp` treibt
die Engine direkt über `set_targets()` (dortiger `feed()`-Helper):

- **Stille am linken Anschlag:** `set_filt(−1)` → Output-RMS unter −80 dBFS
  (der 10-ms-Smoother konvergiert exponentiell gegen 0, exakte Null gibt es
  nicht), **für mehrere FILTER-Targets** (0, 0.5, 1.0) — pinnt die
  `kFiltLeftScale`-Invariante.
- **Voll offen am rechten Anschlag:** `set_filt(+1)` bei FILTER-Target 0.2 →
  Output bit-identisch zu FILTER-Target 1.0 mit `set_filt(0)` (beide landen
  auf `filter_hz(1)`; alle anderen Targets gleich halten).
- **Keine Totzone:** kleines negatives `t` (z. B. −0.1) bei mittlerem
  FILTER-Target → Output ändert sich messbar gegenüber `t = 0`.
- **Stetigkeit:** Sweep `t` von −1 nach +1 in Control-Block-Schritten →
  kein Sample-Sprung über einem Schwellwert; deckt insbesondere den Eintritt
  in die Blendzone ab (der OnePole muss jede Stufe glätten).

**Host-Ebene:**

- VCV-Host baut, `gen_panel.py` läuft ohne Diff-Überraschung, und die
  Voice-Row zeigt im Panel-Preview sechs Knöpfe `ATK DEC FILT RES SUB DTUN`.
- Ein Render-Szenario mit `set_voice_filt` läuft durch (Action-Map-Zeile).

## Offene Risiken

- **`kFiltFadeRange = 0.25` / `kFiltLeftScale = 1.25`** sind geschätzt, nicht
  gehört. Im Play-Test prüfen: Kippt die Blende zu abrupt in die Stille, oder
  fühlt sich das letzte Reglerfünftel tot an? Beim Nachjustieren die
  Invariante `kFiltLeftScale ≥ 1 + kFiltFadeRange` mitnehmen.
- **Stumm ≠ still:** Ein weggeblendeter Part feuert weiter Gates, CV-Ausgänge
  und CHOKE-Fenster. Gewollt (Sequencer läuft unter der Stille weiter), aber
  im Play-Test gegenprüfen, ob ein unhörbarer Part, der den anderen choked,
  musikalisch überrascht.
- **Wechselwirkung mit dem Limiter:** `engine/fx/limiter.h:21` notiert, dass
  „RES + COMP resonante Peaks über das alte feste Knie drückten und es
  krachte". FILT links fährt die Resonanz in genau diese Gegend. Im Play-Test
  mit hohem RES + COMP gegenprüfen.

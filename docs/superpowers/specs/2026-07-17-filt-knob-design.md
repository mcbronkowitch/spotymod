# FILT — bipolarer Cutoff/Highpass-Regler pro Part

**Datum:** 2026-07-17
**Status:** Design abgenommen, nicht implementiert

## Ziel

Der klassische Cutoff-Regler fehlt am Instrument. Die Cutoff-Frequenz kommt
heute **ausschließlich** aus der Modulator-Lane von Pad 2 (SIZE/FILTER,
`LANE_SIZE`), exponentiell 60 Hz…14 kHz (`synth_engine.cpp:16-19, 107`) — sie
bewegt sich nur, wenn der Super-Modulator sie bewegt. Der RES-Knopf steuert
allein die Resonanz des Voice-SVF (`voice.cpp:62`) und ist von der Cutoff
vollständig entkoppelt.

`FILT` ergänzt den fehlenden Handgriff: ein bipolarer Regler pro Part, der
links die Cutoff zudreht (mit Resonanz) und rechts einen Highpass hochzieht.
Beide Hälften greifen im selben Voice-Filterbereich an, damit sich der Regler
als *ein* Gerät anfühlt und nicht in zwei zerfällt.

## Bedienung

- **Panel:** kleiner bipolarer Knopf `FILT` in der Voice-Row, links neben RES.
- **Wertebereich:** −1…+1, Default 0 (Mitte = Funktion aus).
- **Richtung:**
  - links von Mitte → **Cutoff fährt zu**, RES singt am neuen Cutoff mit
  - Mitte → **bit-identisch zu heute**
  - rechts von Mitte → **Highpass zieht hoch**, der Part wird ausgehöhlt

Der Name ist `FILT`, nicht `TONE`: „TONE" ist auf dem Panel bereits durch
`REV_TONE` im Center-Strip belegt (`gen_panel.py:138`). `FILT` benutzt
dasselbe Wort wie die FILTER-Lane, die denselben Filter bewegt.

## Kennlinie

Sei `t` der Reglerwert −1…+1.

### Linke Hälfte — Cutoff-Offset im Lane-Raum (`t < 0`)

Gerechnet wird **nicht in Hertz, sondern im normalisierten Lane-Raum**, in dem
`filter_hz()` ohnehin exponentiell ist:

```
n_eff  = clamp(lane_n + min(t, 0), 0, 1)
cutoff = filter_hz(n_eff)                 // 60 Hz .. 14 kHz
```

Das ist das klassische „Cutoff-Knopf + Modulationstiefe summieren sich, die
Schienen begrenzen"-Verhalten. Kein neuer Filter — der vorhandene SVF macht die
Arbeit, also wirkt die RES-Resonanz am geschlossenen Cutoff mit.

Eigenschaften:

- **Beißt ab der ersten Bewegung**, unabhängig davon, wo die FILTER-Lane
  gerade steht. Keine wandernde Totzone.
- **Ganz links immer 60 Hz**, nie stumm — die Untergrenze ist die Lane-eigene
  Untergrenze, kein Clamp ins Nichts.
- Steht die Lane bereits tief, läuft der Regler unten früher in die Schiene und
  die letzten Grad tun nichts. Das ist Synth-üblich, vorhersehbar und
  akzeptiert.

### Rechte Hälfte — Highpass (`t > 0`)

Eigene Filterstufe, weil der `.High()`-Ausgang des vorhandenen SVF an der
Lane-Cutoff hängt und alles wegschneiden würde, sobald die Lane tief steht.

```
hp_hz = 20 * 100^t                        // 20 Hz .. 2 kHz, exponentiell
```

| `t` | `hp_hz` | Wirkung |
|---|---|---|
| 0.0 | 20 Hz | neutral (Stufe umgangen) |
| 0.25 | 63 Hz | Sub geht |
| 0.5 | 200 Hz | Bass weg |
| 0.75 | 632 Hz | hohl |
| 1.0 | 2000 Hz | Geisterbild |

Obergrenze 2 kHz: Der ganze Regelweg trägt musikalisch. Bei 5 kHz wäre das
letzte Drittel bei einem Bass-Part de facto verschenkt.

## Engine-Architektur (portabel, kein Host-Wissen)

### Instrument / Part / SynthEngine

Push-Kette exakt analog zu RES (`instrument.h:76` → `part.h:68` →
`synth_engine.cpp:153`):

- `Instrument::set_voice_filt(int p, float t)` (−1…+1, geclampt)
- `Part::set_voice_filt(float t)` → `_synth.set_filt(t)`
- `SynthEngine`: neuer Member `_filt_tone` (Boot-Default `0.f`)

In `SynthEngine::_update_control()` (control-rate, alle 96 Samples):

1. Cutoff-Offset an der vorhandenen Stelle (`synth_engine.cpp:107`):
   ```cpp
   const float n_eff  = clampf(_targets[LANE_SIZE] + fminf(_filt_tone, 0.f), 0.f, 1.f);
   const float cutoff = filter_hz(n_eff);
   ```
2. HP-Menge an die Voices durchreichen, neben `vc.set_resonance(_resonance)`
   (`synth_engine.cpp:118`):
   ```cpp
   vc.set_hp_amount(_filt_tone);
   ```

**Bit-Identität der linken Hälfte fällt gratis ab:** bei `t = 0` ist `n_eff`
identisch zu dem, was `filter_hz()` intern ohnehin klemmt. Kein Sonderfall
nötig.

### Voice

- Neuer Member `daisysp::Svf _hp` neben `_filt` (`voice.h:50`), im Voice-Init
  mit der Samplerate initialisiert, **feste Resonanz 0** (`SetRes(0.f)` →
  maximal gedämpft, kein Peak).
- Neuer Member `float _hp_mix = 0.f`.
- `Voice::set_hp_amount(float t)` — control-rate, setzt beides:
  ```cpp
  const float a = t > 0.f ? t : 0.f;
  _hp.SetFreq(clampf(20.f * powf(100.f, a), 20.f, 0.3f * _sr));  // gleicher Clamp wie set_cutoff_hz
  _hp_mix = clampf(a / 0.02f, 0.f, 1.f);
  ```
  Die Frequenz wird **auch bei `t <= 0`** gesetzt (dann 20 Hz), damit der
  mitlaufende Filter-State zur Reglerstellung passt und der Wiedereintritt
  nicht ploppt.
- Audio-Pfad (`voice.cpp:99-100`), Ersatz für `s = _filt.Low() * _env.process();`:
  ```cpp
  _filt.Process(s);
  const float lo = _filt.Low();
  _hp.Process(lo);                       // immer laufen lassen: State bleibt warm
  s = (_hp_mix > 0.f) ? lerpf(lo, _hp.High(), _hp_mix) : lo;
  s *= _env.process();
  ```

**`_hp` läuft immer mit**, auch bei `t <= 0` — sonst friert der Filter-State
ein und ploppt beim Wiedereintritt. Die Ausgabe wird nur verworfen; der
Signalweg bleibt bei `t <= 0` bitgenau `_filt.Low()`.

### Click-Vermeidung am Nulldurchgang

Ein harter Bypass-Umschalter bei `t = 0` springt von `lo` auf `_hp.High()` —
ein 12-dB-HP bei 20 Hz ist auf einem Sub-lastigen Part **nicht** transparent
(Content bei 40 Hz liegt nur eine Oktave über der Ecke). Der Sprung wäre ein
hörbarer Thump beim Drehen über die Mitte.

Lösung: kurze Überblendung statt hartem Schalter.

```
hp_mix = clamp(t / 0.02, 0, 1)            // t <= 0 -> 0 ; t >= 0.02 -> 1
```

Das hält `t = 0` exakt bit-identisch **und** macht den Eintritt stetig. Die
Überblendzone ist 1 % des Regelwegs — als Totzone nicht wahrnehmbar.

## VCV-Host

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

- Linker Knopf endet bei x = 6.5; Accent-Bar liegt bei x 0…1.4, Schrauben bei
  y = 3 / 125.5 (`gen_panel.py:257-258`) — kein Konflikt mit y = 76.8.
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
  Widget bleibt Trimpot (`WK_SMKNOB`) — bipolar auf einem Trimpot hat mit CHOKE
  Präzedenz.
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
  den 20-Hz-Clamp und der Part verstummt. Der normalisierte Offset landet
  stattdessen auf der Lane-eigenen 60-Hz-Schiene.
- **Eigene Stereo-Stufe auf dem Part-Bus (DJ-Filter), Voice-SVF unangetastet.**
  Verworfen: keine Resonanz beim Zudrehen, und die beiden Reglerhälften
  griffen an verschiedenen Punkten der Kette an (eine vor GRIT, eine dahinter).
- **6 dB/Okt One-Pole-HP.** Verworfen: gegen die 12-dB-LP-Seite mit Resonanz
  fühlt sich die rechte Hälfte kraftlos an.
- **HP-Obergrenze 800 Hz / 5 kHz.** Verworfen: 800 Hz lässt keine Reserve für
  Arrangement-Gesten; bei 5 kHz ist das letzte Reglerdrittel auf einem
  Bass-Part unhörbar.
- **CV-Eingang für FILT.** YAGNI: Panel ist voll, und die FILTER-Lane moduliert
  den Cutoff bereits.
- **Snap / Rasterung auf die Mitte.** Nicht nötig: Default 0 heißt, Doppelklick
  fährt auf die Mitte zurück.

## Tests

Neue Datei `tests/test_filt.cpp`, Muster nach `tests/test_choke.cpp:101`.

- **Neutralität:** `FILT = 0` → Output bit-identisch zu einer unberührten
  Instrument-Instanz (Regressionsrender). Deckt beide Hälften ab: Cutoff-Offset
  ist 0, `hp_mix` ist 0.
- **Linke Schiene:** `FILT = −1` → Cutoff = 60 Hz, **unabhängig vom Wert der
  FILTER-Lane** (über mehrere Lane-Stellungen prüfen — genau das war der
  Ausschlussgrund für die Deckel-Variante).
- **Kein Verstummen:** `FILT = −1` → Output-RMS > 0 bei tiefster Lane-Stellung.
- **Rechte Schiene:** `FILT = +1` → HP-Ecke bei 2 kHz (Dämpfung eines
  40-Hz-Sinus messen).
- **Stetigkeit:** Sweep `t` von −1 nach +1 in Control-Block-Schritten → kein
  Sample-Sprung über einem Schwellwert, insbesondere am Nulldurchgang
  (pinnt die `hp_mix`-Überblendung).
- **Symmetrie:** `FILT_A` und `FILT_B` wirken je nur auf ihren Part.
- VCV-Host baut, `gen_panel.py` läuft ohne Diff-Überraschung, und die
  Voice-Row zeigt im Panel-Preview sechs Knöpfe `ATK DEC FILT RES SUB DTUN`.

## Offene Risiken

- **Überblendbreite `0.02`** ist geschätzt, nicht gemessen. Im Play-Test
  prüfen: Klickt der Nulldurchgang noch? Fühlt sich die Zone tot an? Beides
  wäre ein Grund, den Wert zu drehen.
- **`Svf::SetFreq`-Grenzen:** DaisySP klemmt intern; 2 kHz ist bei allen
  üblichen Samplerates unkritisch, aber `Voice::set_cutoff_hz` clampt heute auf
  `[20 Hz, 0.3·sr]` (`voice.cpp:61`) — die HP-Stufe sollte denselben Clamp
  benutzen statt einen eigenen zu erfinden.
- **Wechselwirkung mit dem Limiter:** `engine/fx/limiter.h:21` notiert, dass
  „RES + COMP resonante Peaks über das alte feste Knie drückten und es
  krachte". FILT links fährt die Resonanz in genau diese Gegend. Im Play-Test
  mit hohem RES + COMP gegenprüfen.

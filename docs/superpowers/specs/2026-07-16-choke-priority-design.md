# CHOKE — Ereignis-Vorrang zwischen Deck A und B

**Datum:** 2026-07-16
**Status:** Design abgenommen, bereit für Implementierungsplan

## Ziel

Ein performabler Regler, der die beiden Decks ereignisweise entflechtet:
bei Bedarf spielt nur A **oder** B, nie beide gleichzeitig — stufenlos von
"beide frei" bis "die weichende Seite schweigt, solange die andere klingt".
Anders als MORPH (Lautstärke-Balance) arbeitet CHOKE auf Ereignis-Ebene:
Trigger der weichenden Seite werden verworfen, nicht leiser gemacht.

## Bedienung

- **Panel:** kleiner bipolarer Knopf `CHOKE` im freien Center-Slot
  `(CX, y=51)` zwischen SCALE und DRIFT, direkt unter TEMPO.
- **Wertebereich:** −1…+1, Default 0 (Mitte = Funktion aus).
- **Richtung** (konsistent mit MORPH, wo kleiner Wert = A):
  - links von Mitte → **A hat Vorrang, B weicht**
  - rechts von Mitte → **B hat Vorrang, A weicht**

## Kennlinie

Sei `a = |CHOKE|` die Auslenkung 0…1 (Beispieltext: A hat Vorrang).

### Zone 1 — Wahrscheinlichkeit (0 < a ≤ 0.25)

Jeder Anschlag der Vorrang-Seite "beansprucht" den Raum mit
Wahrscheinlichkeit `p = a / 0.25` (0→100%, Zufallsentscheid **pro Step**,
Rng mit festem Seed). Ein Claim sperrt neue Trigger der weichenden Seite,
solange das **Gate** der Vorrang-Seite high ist (Retrigger-Puls bzw.
gehaltene Step-Note).

### Zone 2 — Fensterlänge (0.25 < a ≤ 1)

Alle Anschläge beanspruchen (p = 1). Das Sperrfenster wächst über das Gate
hinaus in den Ausklang: mit `w = (a − 0.25) / 0.75` sperrt zusätzlich die
Voice-Envelope der Vorrang-Seite, solange `env > (1 − w)`. Bei
Vollausschlag (`w = 1`) sperrt der komplette Decay — die weichende Seite
spielt nie, solange die Vorrang-Seite noch hörbar ist.

### Verhalten der weichenden Seite

- Verworfene Trigger werden **übersprungen, nicht verzögert**: Lanes,
  Phrase und Pitch-CV laufen normal weiter; nur Engine-Trigger und
  Gate-Puls entfallen. Damit folgt auch das GATE-Ausgangsjack.
- Bereits klingende Noten laufen ihr Decay zu Ende — **kein Audio-Choke**,
  keine Klicks.
- Bei CHOKE = 0 ist das Verhalten bit-identisch zu heute.

## Engine-Architektur (portabel, kein Host-Wissen)

### Instrument

- Neue API: `Instrument::set_choke(float c)` (−1…+1, geclampt).
- Die Claim-Logik lebt **sample-genau in `Instrument::process`** — Center
  wäre mit seinem 96-Sample-Control-Tick (~2 ms) zu grob für den
  ~5-ms-Gate-Puls. Pro Sample, nur Bool-Checks:
  1. Feuert die Vorrang-Seite (`lane_fired(LANE_PITCH)`), würfelt eine
     kleine `Rng` (Member des Instruments, deterministischer Seed) gegen
     `p` → Claim gesetzt/nicht gesetzt.
  2. Claim aktiv solange: Gate der Vorrang-Seite high, **oder** (Zone 2)
     `max(voice_env)` der Vorrang-Seite `> (1 − w)`.
  3. Weichende Seite: `Part::set_inhibit(claim_aktiv)`.

### Part

- Neu: `Part::set_inhibit(bool)`. An der Stelle in `part.cpp`, wo
  Lane-Fire → Engine-Trigger + `_gate_ctr` gesetzt wird, überspringt
  Inhibit beides. Alles andere (Lane-Advance, Pitch, FX) bleibt unberührt.
- Envelope-Abfrage über vorhandenes `active_voices()` / `voice_env()`.

## VCV-Host

- `res/gen_panel.py`: neuer SHARED-Eintrag
  `Ctl("CHOKE", SMKNOB, CX, 51.0, "CHOKE")` → Panel-SVG +
  `generated_panel.hpp` neu generieren.
- `Spotymod.cpp / configControls()`: Sonderfall für `CHOKE` —
  `configParam(CHOKE, -1.f, 1.f, 0.f, "Choke")` (Trimpot bleibt).
- `pushParams()`: eine Zeile `inst.set_choke(params[CHOKE].getValue())`.
- Init-Patch: Default 0 → bestehende Patches klingen unverändert.

## Hardware-Reduzierbarkeit

Ein Poti ersetzt die ursprünglich angedachten zwei Pro-Seite-Schalter —
im Sinne der Konstante "lieber mergen als hinzufügen". Auf der realen
Hardware als ein Center-Poti abbildbar.

## Tests

- **Neutralität:** CHOKE = 0 → Output bit-identisch zum heutigen Engine-
  Verhalten (Regressionsrender).
- **Vollausschlag:** CHOKE = −1 → Part B feuert nie (kein Gate, kein
  Trigger), solange Part A Gate high hat oder eine Voice hörbar ausklingt.
- **Zone 1 statistisch:** fester Seed, a = 0.125 → ~50% der A-Steps
  claimen; verworfene B-Trigger nur innerhalb A-Gate-Fenstern.
- **Symmetrie:** CHOKE = +1 spiegelbildlich (A weicht B).
- VCV-Host baut und der Knopf erscheint im Panel-Preview an (CX, 51).

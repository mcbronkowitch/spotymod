# MOD & TIDE — Modulation greifbar machen

**Datum:** 2026-07-17
**Status:** Design abgenommen, nicht implementiert

## Ziel

Die Grundidee des Instruments ist „Modulation first" — aber am Panel ist die
Modulation heute nur indirekt greifbar. Es gibt keinen Regler, der sagt „wie
stark bewegt sich der Klang", ohne gleichzeitig die Melodie zu verändern, und
keinen, der sagt „wie schnell wälzt sich die Textur", ohne die Clock zu
verstellen:

- **DEPTH** (`part.h:28`, `part.cpp:31-38`) skaliert die Modulation *aller*
  Lanes — inklusive der PITCH-Lane. Wer die Textur zurücknimmt, staucht auch
  den Melodie-Ambitus.
- **RANGE** (`super_modulator.cpp:38`) skaliert die Lane-Amplitude ebenfalls
  für alle Lanes. Auf den Textur-Lanes multiplizieren sich RANGE und DEPTH —
  zwei Regler für denselben Job, keiner davon exklusiv.
- **RATE** ist die Master-Clock: schneller drehen beschleunigt Rhythmus,
  Melodie *und* Textur gemeinsam.

Dabei ist die Trennung „Melodie ist der Anker, die Textur bewegt sich darum"
im Kern längst angelegt: `SuperModulator` führt `pitch_scale` und `mod_scale`
getrennt (`super_modulator.h:42-47`, `_apply_rate()` in
`super_modulator.cpp:28-34`), und SPOT überspringt die PITCH-Lane bewusst
(`super_modulator.cpp:48-58`). Dieses Spec zieht die Trennung bis ans Panel:

| Regler | vorher | nachher |
|---|---|---|
| **MOD** (ex-DEPTH, pro Deck) | Tiefe aller Lanes inkl. Pitch | Tiefe **nur der 4 Textur-Lanes** + FX-Targets |
| **RANGE** (pro Deck) | Amplitude aller Lanes | **nur Melodie-Ambitus** (PITCH-Lane) |
| **TIDE** (global, Center, **neu**) | — | Rate **nur der Textur-Lanes** beider Decks, ¼×–4× |

Invariante über alles: **Melodie (Tonfolge + Ambitus) und Rhythmus (Gates)
sind von MOD und TIDE vollständig unberührt.** Gates feuern aus der
PITCH-Lane; weder ihre Rate noch ihre Density wird angefasst.

## Bedienung

- **MOD** (pro Deck, Orbit-Position 6, wo heute DEPTH sitzt): 0…1, die
  Marbles-Spread-Geste. Auf 0 steht der Klang still auf seinen Basiswerten,
  die Melodie spielt unverändert weiter; aufgedreht atmet und wandert alles.
- **RANGE** (pro Deck, unverändert an Position 4): 0…1, wie weit die Melodie
  sich ausbreitet. Der Name passt jetzt besser als vorher.
- **TIDE** (Center, kleiner Knob rechts neben MORPH bei `(R, 22.0)`): 0…1,
  Mitte = neutral (heutiges Verhalten, bit-identisch). Links wälzt sich die
  Textur bis ¼× — träge, dronig; rechts bis 4× — flirrend. Die Melodie-Clock
  und die festen Lane-Verhältnisse untereinander (×2, ×½, ×¾, ×1½,
  `super_modulator.cpp:8`) bleiben stehen.
  - **SYNC an:** TIDE rastet auf eine symmetrische 9-Stufen-Leiter
    musikalischer Verhältnisse: ¼, ⅓, ½, ⅔, **1**, 1½, 2, 3, 4. Neun Stufen,
    nicht zehn, damit die Knopf-Mitte exakt auf ×1 liegt (die im Brainstorm
    genannte ¾-Stufe ist gestrichen — sie hätte kein reziprokes Gegenstück
    und würde die Mitte von der Neutralstellung wegschieben).
  - **SYNC aus:** kontinuierlich, `mult = 2^(4·(norm−0.5))`.
- Deck-Geschichte danach in einem Satz: **RATE = Tempo, RANGE =
  Melodie-Weite, MOD = Textur-Intensität** — und TIDE im Center ist das
  Wetter-Tempo des ganzen Instruments.

### Labels: der MOD-Namenskonflikt

Das Panel trägt bereits ein „MOD"-Label: `REV_MOD` im ROOM-Block
(`gen_panel.py:147`), der Tail-LFO der Reverb („Wobble", siehe
Reverb-Mod-Split). Zwei gleichnamige Knöpfe auf einem Panel sind genau die
Verwirrung, die dieses Spec abbauen soll. Darum wird das *Label* von
`REV_MOD` zu **WOBL** umbenannt — das Wort, mit dem der Dev-Diary den Regler
ohnehin beschreibt. Enum und Param-ID bleiben unverändert (Labels sind rein
kosmetisch, kein Patch-Impact).

## Engine-Architektur (portabel, kein Host-Wissen)

### MOD: PITCH-Lane aus der Depth-Klammer nehmen

`Part::target_raw()` (`part.cpp:31-38`) ist die einzige Stelle:

```cpp
// vorher
float mod = _active[slot] ? _mod.lane_output(slot) * _depth * _tdepth[slot] : 0.f;
// nachher: die PITCH-Lane hängt nicht am Master-MOD
const float d = (slot == LANE_PITCH) ? 1.f : _depth;
float mod = _active[slot] ? _mod.lane_output(slot) * d * _tdepth[slot] : 0.f;
```

- `fx_target_value()` (`part.cpp:49-52`) bleibt an `_depth`: die FX-Targets
  (GRIT_INT, FLUX_TIME, FX_MIX, REV_SEND, FLUX_FB) zählen zur Textur und
  atmen mit MOD.
- Das per-Slot-`_tdepth` (Target-Row, `part.h:33`) bleibt unangetastet — die
  Feinbalance pro Target existiert weiter, MOD ist der Master davor.
- API unverändert: `Instrument::set_depth()` / `Part::set_depth()` behalten
  ihre Namen (die Engine-API heißt nicht um, nur das Panel-Label).

### RANGE: nur noch die PITCH-Lane

`SuperModulator::set_range()` (`super_modulator.cpp:38`):

```cpp
// vorher
void SuperModulator::set_range(float r) { for (auto& l : _lanes) l.set_range(r); }
// nachher
void SuperModulator::set_range(float r) { _lanes[LANE_PITCH].set_range(r); }
```

Die Textur-Lanes bleiben auf ihrem Init-Default `_range = 1.f` (`lane.h:70`)
— ihre Amplitude regelt allein MOD am Combine-Punkt. Mathematisch geht
nichts verloren: beides sind lineare Faktoren auf demselben Signal, nur die
Klammer wandert.

### TIDE: eigener Faktor in `_apply_rate()`

Neuer Zustand + Setter im `SuperModulator`:

```cpp
void set_tide(float norm);        // 0..1 vom Panel; Default-Zustand 0.5
float _tide_norm = 0.5f;
float _tide_mult = 1.f;           // aus norm + _synced abgeleitet
```

Die Leiter lebt neben der RATE-Leiter in `mod/divisions.h` (dasselbe Muster:
Engine und VCV-Tooltip lesen dieselbe Tabelle):

```cpp
inline constexpr float kTideRatios[] =
    { 0.25f, 1.f/3.f, 0.5f, 2.f/3.f, 1.f, 1.5f, 2.f, 3.f, 4.f };
inline constexpr const char* kTideNames[] =
    { "x1/4", "x1/3", "x1/2", "x2/3", "x1", "x3/2", "x2", "x3", "x4" };
inline constexpr int kTideCount = 9;
inline int tide_index(float norm) {
    return static_cast<int>(clampf(norm, 0.f, 1.f) * (kTideCount - 1) + 0.5f);
}
inline float tide_free(float norm) {
    return std::pow(2.f, 4.f * (clampf(norm, 0.f, 1.f) - 0.5f));   // 1/4 .. 4
}
```

`set_tide()` und `set_synced()` berechnen `_tide_mult` neu
(synced → `kTideRatios[tide_index(norm)]`, frei → `tide_free(norm)`) und
rufen `_apply_rate()`. Dort die einzige Anwendungsstelle
(`super_modulator.cpp:28-34`):

```cpp
void SuperModulator::_apply_rate() {
    _master_hz = _base_hz * _pitch_scale;                  // unverändert: Melodie-Clock
    for (int i = 0; i < LANE_COUNT; ++i) {
        const float s = (i == LANE_PITCH) ? _pitch_scale
                                          : _mod_scale * _tide_mult;
        _lanes[i].set_rate_hz(_base_hz * s * kLaneRatio[i]);
    }
}
```

- **COUPLE/DRIFT unverändert obendrauf:** `Center::update()` schreibt
  `mod_scale` weiter über `set_rate_scale()` (`center.cpp:110-113, 171-172`);
  TIDE multipliziert sich dahinter. Bei vollem COUPLE in SYNC (Lockstep,
  `texture = 0`, `center.cpp:109`) halten die Lanes ihre exakten Verhältnisse
  ×TIDE — der Lockstep läuft dann einfach langsamer oder schneller. Gewollt:
  TIDE ist die Hand des Spielers, kein Wetter, und wird nie unterdrückt.
- **Bit-Identität in der Mitte fällt gratis ab:** `norm = 0.5` → frei
  `2^0 = 1.0` exakt (IEEE-754), synced → Index 4 → `1.f`; `x * 1.f == x`.
  Kein Sonderfall nötig.
- `Instrument::set_tide(float n)` reicht an beide Parts durch
  (`instrument.h`, neben `set_morph`/`set_couple`):

```cpp
void set_tide(float n) { for (auto& p : _parts) p.mod().set_tide(n); }
```

### Was sich bewusst NICHT ändert

- **SMOOTH, SHAPE, MELO/VARIATION, STEP** wirken weiter auf alle Lanes —
  sie formen den Charakter, nicht die Stärke; kein Teil dieses Specs.
- **DENSITY** war schon immer PITCH-only (`super_modulator.h:20`).
- **Rhythmus-Pfad:** Gates entstehen aus PITCH-Lane-Fires und dem
  STEP-Sustain (`part.h:88-90`); weder MOD noch TIDE berühren Rate, Density
  oder Groove der PITCH-Lane.

## Host: VCV

### Panel (`res/gen_panel.py`)

- **Enum-Rename im Template:** `("DEPTH","DEPTH")` → `("MOD","MOD")`
  (`gen_panel.py:91`). Die Orbit-Position (Index 6) bleibt — Param-IDs
  entstehen aus der Listenposition, also **kein ID-Shift, volle
  Patch-Kompatibilität**. C++-Referenzen `DEPTH_A`/`DEPTH_B` werden zu
  `MOD_A`/`MOD_B`.
- **`REV_MOD`-Label:** `"MOD"` → `"WOBL"` (`gen_panel.py:147`). Enum/ID
  unverändert.
- **TIDE:** ans **Ende von `PARAMS`** (nach `FILT_B`, `gen_panel.py:154-160`)
  — dieselbe Ausweichbewegung wie CHOKE und FILT, damit `PART_STRIDE` und
  alle bestehenden IDs stehen bleiben. Koordinate setzt ihn optisch neben
  MORPH, in die rechte Center-Spalte zu COUPLE/DRIFT/SETL (die
  „Bewegungs-Spalte"):

```python
PARAMS = PART_A + PART_B + SHARED + [
    Ctl("FILT_A", ...), Ctl("FILT_B", ...),
    # TIDE: Textur-Rate beider Decks (spec 2026-07-17 mod-tide). Ans Ende
    # angehängt wie CHOKE/FILT: bestehende .vcv-Patches behalten ihre IDs.
    Ctl("TIDE", SMKNOB, R, 22.0, "TIDE"),
]
```

  Geometrie: MORPH (BIGKNOB, r 4.2 + Kragen) bei `(CX, 22.0)`, TIDE
  (SMKNOB, r 3.0) bei `CX+10.5` — gleicher Abstand wie die TEMPO/COUPLE-Reihe
  (y 38). Label-Baseline y 27.5, die TIME-Braue liegt bei y 31.4 — kein
  Konflikt. Der Platz links von MORPH `(L, 22.0)` bleibt bewusst frei.
- `python3 res/gen_panel.py` aus `host/vcv/` neu laufen lassen (SVG +
  `generated_panel.hpp` sind beide committed).

### C++ (`src/Spotymod.cpp`)

- **Tooltip** nach dem Muster von `RateQuantity` (`Spotymod.cpp:15-21`):

```cpp
struct TideQuantity : ParamQuantity {
    std::string getDisplayValueString() override {
        if (module && module->params[SYNC].getValue() > 0.5f)
            return spky::kTideNames[spky::tide_index(getValue())];
        return string::f("x%.2f", spky::tide_free(getValue()));
    }
};
```

- **`configControls()`:** Sonderfall vor dem generischen Zweig (wie FILT,
  `Spotymod.cpp:71-72`) — wichtig, weil `defaultFor()` für angehängte IDs
  Unsinn rechnet (`id / PART_STRIDE`):

```cpp
else if (c.id == TIDE)
    configParam<TideQuantity>(c.id, 0.f, 1.f, 0.5f, lbl);
```

- **`pushParams()`:** `inst.set_tide(params[TIDE].getValue());` neben den
  Center-Pushes (`Spotymod.cpp:208-212`). Die MOD-Zeile wird
  `inst.set_depth(p, pp(MOD_A, p));` (Rename von `Spotymod.cpp:174`).
- **`defaultFor()`:** Rename `DEPTH_A` → `MOD_A` im Part-Fold
  (`Spotymod.cpp:140`), neue Werte siehe Init-Patch.

### Init-Patch — Werte umrechnen statt raten

Heute ist der Beitrag jeder Lane `raw · RANGE · DEPTH · tdepth`; nachher
Pitch `raw · RANGE · tdepth` und Textur `raw · MOD · tdepth`. Damit der
Init-Patch (`defaultFor()`, `Spotymod.cpp:117-153`) klanglich dort bleibt,
wo er hand-abgestimmt wurde, starten beide neuen Knöpfe auf dem **Produkt
der alten Werte**:

| | RANGE alt | DEPTH alt | → RANGE neu | → MOD neu |
|---|---|---|---|---|
| Part A (Drone) | 1.00 | 0.78 | **0.78** | **0.78** |
| Part B (Bass) | 0.38 | 0.622 | **0.236** | **0.236** |

TIDE-Default **0.5** (neutral). Das ist der rechnerische Startpunkt, nicht
das Endergebnis — die Produktform `raw·(R·D)` vs. `(raw·R)·D` ist äquivalent,
aber nicht bitgleich, und ob z. B. Part B mit wieder voller interner
Textur-Range genauso atmet, entscheidet das Ohr. **Abhör-Pass nach der
Implementierung ist Teil der Definition-of-Done** (siehe Offene Risiken).

### Version

`plugin.json` → **2.4.0** (neues Feature, kein Patch-Bruch).

## Host: Desktop-Render (Szenarien)

`host/render/scenario.cpp` braucht eine Action-Zeile neben den globalen
Setters (`scenario.cpp:103-110`):

```cpp
else if (a == "set_tide") inst.set_tide(e.value);
```

`set_depth` und `set_range` existieren dort schon (`scenario.cpp:81, 84`)
und ändern nur ihre Semantik mit — vorhandene Szenarien, die sie benutzen,
beim Abhören gegenprüfen.

## Hardware-Reduzierbarkeit

Netto **+1 Poti global, ±0 pro Deck** — DEPTH wird zu MOD umgewidmet, RANGE
umdefiniert, nur TIDE kommt dazu. Auf der realen Hardware ist ein einzelner
zusätzlicher Center-Regler abbildbar; verglichen mit der verworfenen
Alternative „Speed pro Deck" (+2) ist das die sparsame Variante. Die
Projekt-Konstante „lieber mergen als hinzufügen" bleibt gewahrt: funktional
ist dieses Spec sogar ein Merge (RANGE und DEPTH teilen sich die Arbeit
sauber auf, statt sich zu multiplizieren).

## Verworfene Alternativen

Bewusst durchgespielt und verworfen — nicht erneut aufrollen ohne neuen Grund:

- **MOD global im Center (Marbles-Spread wörtlich), per-Deck-DEPTH fliegt
  raus.** Verworfen im Brainstorm: die per-Deck-Balance („Drone ruhig, Bass
  wild") ist musikalisch zu wertvoll; die Decks sind das Instrument.
- **MOD zusätzlich zu unverändertem RANGE.** Verworfen: RANGE und MOD würden
  auf den Textur-Lanes multiplizieren — exakt die Zwei-Regler-Verwirrung,
  die das Spec abbauen soll.
- **RANGE ebenfalls Textur-only, Melodie-Ambitus fest.** Verworfen: dann
  wären RANGE und MOD Fast-Duplikate und der Ambitus unspielbar.
- **Speed pro Deck (+2 Trimmer).** Verworfen: Hardware-Regel, und die Decks
  sollen sich als *ein* Instrument wälzen; die Feinbalance der Tiefe liegt
  schon pro Deck.
- **Speed an MOD koppeln (obere Reglerhälfte beschleunigt zusätzlich).**
  Verworfen: verliert genau die geforderte Unabhängigkeit — langsame *tiefe*
  Modulation wäre unerreichbar.
- **10-Stufen-Leiter mit ¾ (wie im Brainstorm skizziert).** Verworfen beim
  Ausarbeiten: gerade Stufenzahl legt die Knopf-Mitte zwischen zwei Rasten —
  die Neutralstellung wäre nicht exakt ×1. Die 9er-Leiter ist
  reziprok-symmetrisch um die Mitte.
- **TIDE moduliert auch `_master_hz`/Melodie-Clock mit kleinem Anteil.**
  YAGNI und gegen die Kern-Invariante; dafür gibt es RATE und COUPLE.
- **CV-Eingang für TIDE/MOD.** YAGNI: die Jack-Leiste ist voll
  (`gen_panel.py:169`), und im Rack kann man die Textur-Targets extern
  längst anfahren.
- **`REV_MOD`-Label behalten (zwei „MOD" am Panel).** Verworfen: der
  Namenskonflikt wäre neu geschaffene Verwirrung; „WOBL" deckt sich mit der
  etablierten Beschreibung (Tail-LFO = Wobble).

## Tests

Nach vorhandenen Mustern (`tests/test_part.cpp`, `test_super_modulator.cpp`,
`test_instrument.cpp`, `test_scenario.cpp`):

**Kern-Invariante (Instrument-/Part-Ebene):**

- **MOD lässt Melodie und Rhythmus stehen:** identische Instrument-Instanzen
  mit `set_depth(p, 0.0 / 0.5 / 1.0)` über mehrere Sekunden treiben → die
  Folge der `pitch_cv()`-Werte und die Sample-Positionen aller
  `gate()`-Flanken sind **bit-identisch**; mindestens ein Textur-Target
  (`target_value(LANE_SOURCE)`) und ein FX-Target (`fx_target_value`)
  ändern sich messbar.
- **MOD 0 = stillgelegte Textur:** `set_depth(p, 0)` → alle Textur-Targets
  stehen konstant auf `base` (`tdepth`-unabhängig), Pitch moduliert weiter.
- **RANGE ist Melodie-only:** RANGE-Sweep ändert nur den Ambitus von
  `pitch_cv()`; Textur-Targets bleiben bit-identisch.

**TIDE (SuperModulator-Ebene):**

- **Neutralität:** `set_tide(0.5)` → Lane-Raten und Output bit-identisch zu
  einer Instanz ohne `set_tide`-Aufruf, in SYNC **und** frei.
- **Skalierung:** `set_tide(0)` / `set_tide(1)` frei → Textur-Lane-Raten
  exakt ׼ / ×4; `master_hz()` und die PITCH-Lane-Rate unverändert;
  `pitch_phase()`-Verlauf bit-identisch über den Sweep.
- **Raster:** synced → `_tide_mult` trifft exakt die 9 Leiter-Werte;
  `tide_index(0.5) == 4` (×1); Umschalten SYNC an/aus rechnet um.
- **Komposition mit dem Center:** `set_rate_scale(p, m)` + TIDE →
  Textur-Rate = `base · m · tide · ratio`; bei Lockstep (SYNC, COUPLE 1)
  halten die Lanes ihre Verhältnisse ×TIDE.

**Host-Ebene:**

- `test_scenario`: `set_tide`-Action erreicht die Instrument-API.
- VCV baut; `gen_panel.py` läuft; Panel zeigt TIDE neben MORPH, das Deck-Label
  MOD, den ROOM-Regler WOBL; `PART_STRIDE` und alle Alt-IDs unverändert
  (static_asserts `Spotymod.cpp:26-28` halten).
- Ein bestehendes `.vcv`-Patch von v2.3.0 lädt und klingt bis auf die
  RANGE/MOD-Semantik-Migration unüberrascht (TIDE landet auf 0.5 = neutral).

## Offene Risiken

- **Alte Patches verschieben sich klanglich:** Ein v2.3.0-Patch behält seine
  Param-*Werte*, aber RANGE/DEPTH bedeuten jetzt anderes — Textur-Amplitude
  war `RANGE·DEPTH`, ist jetzt `MOD` allein; wer RANGE ≠ 1 gefahren hat,
  hört nach dem Update mehr Texturbewegung, und der Melodie-Ambitus hängt
  nicht mehr an DEPTH. Bewusst in Kauf genommen (das Modul ist im
  Residency-Stadium, kein öffentlicher Patch-Bestand); im Release-Eintrag
  klar benennen.
- **Init-Patch-Retune:** Die umgerechneten Startwerte (0.78 / 0.236) sind
  Rechnung, nicht Ohr. Abhör-Pass gegen v2.3.0-Renders der drei
  Standard-Szenarien, bevor getaggt wird.
- **TIDE ׼ unter Lockstep:** Bei COUPLE 1 + SYNC laufen die Textur-Lanes
  auf exakten Verhältnissen; TIDE ׼ macht daraus sehr lange Zyklen (eine
  ×½-Lane bei 4 Bars RATE braucht dann 32 Bars). Kein Bug, aber im Play-Test
  prüfen, ob das linke Reglerende noch als „Bewegung" lesbar ist oder ob
  ¼× zu weit greift (ggf. auf ⅓× stauchen — Leiter und `tide_free`-Exponent
  dann gemeinsam anpassen).
- **WOBL als Label:** kurz, aber ungewohnt. Falls es im Play-Test stolpert,
  ist jedes andere Wort ein Einzeiler in `gen_panel.py` — nur nicht wieder
  „MOD".

## Errata (Final-Review 2026-07-17)

**Die Behauptung „mathematisch geht nichts verloren" ist nur für RANGE = 1
wahr.** `apply_range` (`engine/mod/range.h`) ist affin, nicht linear: für
`r ≤ 0.5` gilt `apply_range(v, r) = r·v + r` (unipolar, positiver
DC-Offset); erst bei `r = 1` ist es das reine bipolare `v`. Folgen:

- **Init-Patch Part B** (alt RANGE 0.38, DEPTH 0.622): der Textur-/FX-Hub
  stimmt nach der Produkt-Umrechnung exakt (`0.236·v`), aber der alte
  **DC-Lift von +0.236 fehlt** — die Targets atmeten oben angepinnt und
  schwingen jetzt symmetrisch um ihre Basis. Hörbar v. a. am boot-aktiven
  LEVEL-Target (Basis 0.8): der Bass sitzt im Mittel leiser. Wenn B nach dem
  Update flach wirkt: **Target-Basen anheben, nicht MOD** — MOD ändert den
  Hub, nicht den Lift. Part A (alt RANGE 1.0) ist exakt äquivalent.
- **Render-Szenarien mit `set_range < 1`** (`ambient_wash` 0.45/0.2,
  `reverb_delay` 0.4, `reverb_wash` 0.4): verlieren den Lift UND ihr
  Textur-Hub wächst (alt `0.45·v + 0.45` → neu `1.0·v` bei depth 1). Beim
  Abhör-Pass gegen v2.3.0-Referenzen hören; ggf. `set_depth` in die
  Szenarien schreiben.
- **Release-Text v2.4.0:** Migration alter Patches heißt „weniger Lift +
  Charakter-Änderung bei RANGE < 1", nicht nur „andere Amplitude".
- Rand-Detail: `fx_target_value` liest per Index-Gleichheit `FXT_FX_MIX`(2)
  aus `LANE_PITCH`(2) — dieser eine FX-Slot folgt damit dem RANGE-Ambitus
  (boot-inaktiv, vorbestehende Zuordnung).

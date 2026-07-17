# Boot-Targets — Modulation first ist der Auslieferungszustand

**Datum:** 2026-07-17
**Status:** Design abgenommen (Play-Test-Rev-Umfang), Umsetzung direkt

## Ziel

„Modulation first" war bisher ein Opt-in: die Boot-Defaults (`part.h`)
aktivieren nur PITCH und LEVEL; TIMBRE (SOURCE), FILTER (SIZE) und MOTION
sind stumm. Im VCV-Build gibt es keine Target-Row (die kommt mit den
M6-Pads), also bewegt der MOD-Knopf in Rack fast nur die Lautstärke — der
Play-Test-Befund „die Lautstärke ist zu stark auf der Mod-Lane" war die
direkte Folge. Die Lanes laufen längst (LED-Ringe), ihr Output erreicht den
Klang nur nie.

## Entscheidung

**Engine-Boot-Default, nicht Host-Sonderweg** (Variante A im Brainstorm):

```cpp
// part.h — vorher
std::array<bool,  LANE_COUNT> _active { { false, false, true, false, true } };
std::array<float, LANE_COUNT> _tdepth { { 1.f, 1.f, 1.f, 1.f, 1.f } };
// nachher: alle fünf an, Tiefen abgestuft (ear-tunable Startwerte)
std::array<bool,  LANE_COUNT> _active { { true, true, true, true, true } };
std::array<float, LANE_COUNT> _tdepth { { 1.f, 0.55f, 1.f, 0.7f, 1.f } };
```

Tiefen-Begründung (Slot-Reihenfolge SOURCE, SIZE, PITCH, MOTION, LEVEL):

| Slot | Boot-Tiefe | warum |
|---|---|---|
| TIMBRE | 1.0 | Morph + Detune sind der Charakter — voll |
| FILTER | 0.55 | Cutoff ist exponentiell und dominiert; atmet ±, der große Hub bleibt dem FILT-Knopf |
| PITCH | 1.0 | unverändert (Melodie-Pfad, hängt an RANGE) |
| MOTION | 0.7 | Stereo-Breite/Drift bewegt sich, ohne zu pumpen |
| LEVEL | 1.0 | unverändert; der 40%-Floor (Rev 921ec81) fängt die Tiefe |

- Gilt überall: VCV, Render-Szenarien, später Hardware (M6-Pads togglen
  weiterhin — das hier ist nur der Startzustand).
- Basen bleiben `{0.5, 0.5, 0.5, 0.5, 0.8}`. FX-Targets bleiben aus
  (eigenes Thema).
- Szenarien/Tests, die sich auf stumme Boot-Targets verlassen, ändern sich
  hörbar bzw. reißen Schwellwerte — Tests werden beim Suite-Lauf gefangen
  und explizit gepinnt (`set_target_active(slot, false)` bzw.
  `set_target_depth`), Szenarien gehören in den Abhör-Pass.

## Tests

- Boot-Zustand: frisch initialisierter `Part` → alle fünf `target_value`
  bewegen sich über 1 s (min < max), bei MOD 0 stehen SOURCE/SIZE/MOTION
  auf Basis (Regression der MOD-Semantik).
- Tiefen-Staffel: `_tdepth`-Boot-Werte exakt {1, 0.55, 1, 0.7, 1}
  (über das beobachtbare Verhältnis der Swings oder einen Getter — Umsetzung
  wählt das Einfachere).
- Bestandssuite grün; gepinnte Alt-Tests dokumentieren im Diff, warum.

## Verworfen

- **Nur-VCV-Aktivierung:** verewigt „stumm ist normal", VCV und Hardware
  driften auseinander.
- **Alles auf Tiefe 1.0:** FILTER erschlägt den Rest, Init-Patch chaotisch.

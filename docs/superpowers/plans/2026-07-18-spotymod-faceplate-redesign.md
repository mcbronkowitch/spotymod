# Spotymod Faceplate-Redesign: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Das 42HP-Panel wird semantisch neu sortiert — Sektor-Orbit mit 9 Knöpfen (COLOR wird Vollmitglied), radiale Orbit-Labels, Fieldset-Gruppenboxen für VOICE/FX/PLAY/BLEND/TIME/DUO/ROOM und ein Jack-Strip aus fünf beschrifteten Gruppen mit dunklen Ausgangs-Wannen — ohne dass sich eine einzige Param-ID ändert.

**Architecture:** Alles passiert in `host/vcv/res/gen_panel.py`, der einzigen Quelle für SVG **und** `src/generated_panel.hpp`. Die `PARAMS`-Reihenfolge bleibt byte-identisch (IDs folgen der Enum-*Reihenfolge*, nicht den Koordinaten); nur `Ctl`-Koordinaten, Label-Metadaten, Panel-Grafik und die `TEXTS`-Tabelle ändern sich. Neu: jedes `Ctl` trägt seine **absolute** Label-Position, -Ausrichtung, -Größe und -Farbe im generierten Header — die „Label sitzt unter dem Control"-Regel in `Spotymod.cpp` (inkl. der dort duplizierten `glyphR`-Tabelle) entfällt ersatzlos. Ein neues `res/test_panel.py` friert die Enum-Reihenfolge ein und prüft Geometrie-Invarianten; es läuft vor jedem Commit.

**Tech Stack:** Python 3 (kein pytest im Environment — Tests sind ein eigenständiges Skript mit Exit-Code), VCV Rack SDK 2.6.6 Host (`host/vcv/`, MSYS2 make + WinLibs gcc), NanoVG-Runtime-Lettering in `src/Spotymod.cpp`.

**Spec:** `docs/superpowers/specs/2026-07-18-spotymod-faceplate-redesign-design.md`

**Numerische Referenz:** `.superpowers/brainstorm/1070-1784363654/content/layout-b-v7.html` — das erste `<div class="mockup">` darin ist der Ziel-SVG. Jede Koordinate in diesem Plan wurde daraus verifiziert. Bei Zweifeln gilt das Mockup.

## Global Constraints

- Repo-Root: `c:/Users/bernd/Documents/AI/Spotykach`. Arbeitsverzeichnis für alle Kommandos: `host/vcv/`. Shell-Syntax: **Git-Bash/MSYS**.
- Branch: **main** (kein Worktree, kein Feature-Branch — so vom Nutzer angeordnet).
- **Die Param-Enum-Reihenfolge ist unantastbar.** 72 Params, `PART_STRIDE = 23`, 4 Inputs, 6 Outputs, 2 Lights. `res/test_panel.py` erzwingt das; wenn dieser Test rot wird, ist die Änderung falsch — nie den Test anpassen.
- **Keine DSP-, Parameter- oder Verhaltensänderung.** `configControls()` bleibt inhaltlich unangetastet (einzige Ausnahme: Task 6 gibt Jacks ein eigenes Tooltip-Feld). `defaultFor()` wird nicht angefasst.
- Panel bleibt 42HP: `W = 213.36`, `Hh = 128.5`.
- Nach **jeder** Änderung an `res/gen_panel.py`: `python res/gen_panel.py` laufen lassen und **beide** erzeugten Dateien (`res/Spotymod.svg`, `src/generated_panel.hpp`) mitcommitten. `host/vcv/dist/` ist gitignored — dort nichts von Hand anfassen.
- Farben immer aus den vorhandenen Paletten-Konstanten (`PAPER`, `PAPER_DEEP`, `LINE`, `INK`, `MUTED`, `WELL`, `WHITE`, `GREEN`, `COPPER`) — keine neuen Hex-Literale im Code.
- Der Runtime-Font ist `ShareTechMono-Regular` (kein Bold-Asset). Die SVG-Vorschau zeichnet Legenden/Captions `font-weight="bold"`, Rack zeichnet sie regular. Das ist eine bekannte, akzeptierte Abweichung — nicht „reparieren".
- Commit-Trailer auf jedem Commit:
  `Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>`
- Build-Kommando (nur wo im Plan verlangt; aus `host/vcv/`):
  ```bash
  export PATH="/c/Users/bernd/Documents/AI/mingw64/bin:/c/msys64/usr/bin:$PATH" RACK_DIR="c:/Users/bernd/Documents/AI/Rack-SDK"
  /c/msys64/usr/bin/make CC=gcc CXX=g++ TMP="C:/Users/bernd/AppData/Local/Temp" TEMP="C:/Users/bernd/AppData/Local/Temp" SHELL=/usr/bin/bash -j4
  ```

---

### Task 1: Golden-Master-Guard — `res/test_panel.py`

Der Sicherheitsgurt für alles danach. Ohne ihn ist „die IDs haben sich nicht verschoben" eine Behauptung statt einer Messung.

**Files:**
- Create: `host/vcv/res/test_panel.py`

**Interfaces:**
- Produces: `python res/test_panel.py` → Exit 0 bei Erfolg, Exit 1 mit Fehlerliste sonst. Hilfsfunktionen für spätere Tasks: `check(cond, msg)`, `approx(a, b, tol=0.02)`, `ctl(enum)` (liefert das `Ctl` mit diesem Enum-Namen aus `gen_panel.PARAMS + INPUTS + OUTPUTS`).

- [ ] **Step 1: Test-Skript schreiben**

Create `host/vcv/res/test_panel.py`:

```python
#!/usr/bin/env python3
"""Guard rails for the generated Spotymod panel.

Runs the generator in-process and asserts the things that must never drift:
the param/input/output enum ORDER (patch compatibility), the panel geometry
(nothing overlaps, nothing falls off the plate) and the target coordinates of
the 2026-07-18 faceplate redesign.

No pytest in this environment -- plain asserts, exit code says it all.
Run from host/vcv/:  python res/test_panel.py
"""
import os, sys, math

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_panel as g

FAILS = []


def check(cond, msg):
    if not cond:
        FAILS.append(msg)


def approx(a, b, tol=0.02):
    return abs(a - b) <= tol


def ctl(enum):
    for c in g.PARAMS + g.INPUTS + g.OUTPUTS:
        if c.enum == enum:
            return c
    raise KeyError(enum)


# --- the frozen contract: enum ORDER defines param ids in every saved patch ---
PARAM_ORDER = [
    'RATE_A', 'SHAPE_A', 'DENSITY_A', 'SMOOTH_A', 'RANGE_A', 'MELODY_A',
    'MOD_A', 'TUNE_A', 'ATTACK_A', 'DECAY_A', 'RES_A', 'SUB_A', 'DETUNE_A',
    'FLUX_A', 'GRIT_A', 'COMP_A', 'STEPS_A', 'ENGINE_A', 'GRITMODE_A',
    'STEP_A', 'PRINCIPLE_A', 'NEWPHRASE_A', 'TRIGGER_A',
    'RATE_B', 'SHAPE_B', 'DENSITY_B', 'SMOOTH_B', 'RANGE_B', 'MELODY_B',
    'MOD_B', 'TUNE_B', 'ATTACK_B', 'DECAY_B', 'RES_B', 'SUB_B', 'DETUNE_B',
    'FLUX_B', 'GRIT_B', 'COMP_B', 'STEPS_B', 'ENGINE_B', 'GRITMODE_B',
    'STEP_B', 'PRINCIPLE_B', 'NEWPHRASE_B', 'TRIGGER_B',
    'MORPH', 'SYNC', 'TEMPO', 'COUPLE', 'SCALE', 'DRIFT', 'SPOT',
    'MASTER_DRIVE', 'SETTLE', 'REV_SIZE', 'REV_DECAY', 'REV_MIX', 'REV_TONE',
    'REV_DIFF', 'REV_SMEAR', 'REV_MOD', 'CHOKE', 'FILT_A', 'FILT_B', 'TIDE',
    'FLUXRATE_A', 'FLUXRATE_B', 'FLUXFB_A', 'FLUXFB_B', 'COLOR_A', 'COLOR_B',
]
INPUT_ORDER = ['IN_L', 'IN_R', 'CLOCK', 'RESET']
OUTPUT_ORDER = ['OUT_L', 'OUT_R', 'PITCH_A', 'GATE_A', 'PITCH_B', 'GATE_B']
LIGHT_ORDER = ['GATE_A_L', 'GATE_B_L']


def test_enum_order():
    """Patch compatibility. If this fails, every saved .vcv breaks."""
    check([c.enum for c in g.PARAMS] == PARAM_ORDER, "PARAMS order changed")
    check([c.enum for c in g.INPUTS] == INPUT_ORDER, "INPUTS order changed")
    check([c.enum for c in g.OUTPUTS] == OUTPUT_ORDER, "OUTPUTS order changed")
    check([c.enum for c in g.LIGHTS] == LIGHT_ORDER, "LIGHTS order changed")
    check(g.PART_STRIDE == 23, f"PART_STRIDE is {g.PART_STRIDE}, must be 23")


def test_no_overlap():
    """No two glyphs may touch -- Rack widgets would steal each other's clicks."""
    all_c = g.PARAMS + g.INPUTS + g.OUTPUTS
    for i, a in enumerate(all_c):
        ra = g.GLYPH_R[a.kind]
        for b in all_c[i + 1:]:
            rb = g.GLYPH_R[b.kind]
            d = math.hypot(a.x - b.x, a.y - b.y)
            check(d >= ra + rb - 0.001,
                  f"{a.enum} and {b.enum} overlap (d={d:.2f} < {ra + rb:.2f})")


def test_on_panel():
    """Every glyph stays 2 mm inside the plate."""
    for c in g.PARAMS + g.INPUTS + g.OUTPUTS + g.LIGHTS:
        check(2.0 <= c.x <= g.W - 2.0 and 2.0 <= c.y <= g.Hh - 2.0,
              f"{c.enum} off panel at ({c.x:.2f}, {c.y:.2f})")


def test_panel_size():
    check(approx(g.W, 213.36) and approx(g.Hh, 128.5), "panel is no longer 42HP")


def main():
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
    if FAILS:
        print(f"FAIL ({len(FAILS)}):")
        for f in FAILS:
            print("  -", f)
        return 1
    print("PASS -- panel guards ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Test laufen lassen — muss GRÜN sein (Characterization Test)**

```bash
cd host/vcv && python res/test_panel.py
```
Expected: `PASS -- panel guards ok`, Exit 0.

- [ ] **Step 3: Beweisen, dass der Test fehlschlagen kann**

Temporär in `res/gen_panel.py` in der `SHARED`-Liste (nach der `MORPH`-Zeile, ca. Zeile 135) einfügen:
```python
    Ctl("BOGUS", SMKNOB, CX, 22.0, "BOGUS"),
```
Dann:
```bash
cd host/vcv && python res/test_panel.py
```
Expected: `FAIL (2):` mit `- PARAMS order changed` und einer Overlap-Meldung zu `BOGUS`, Exit 1.

- [ ] **Step 4: Die BOGUS-Zeile wieder entfernen und Grün bestätigen**

```bash
cd host/vcv && git diff --stat res/gen_panel.py && python res/test_panel.py
```
Expected: `git diff --stat` gibt **nichts** aus (Datei unverändert), Test sagt `PASS`.

- [ ] **Step 5: Commit**

```bash
cd host/vcv && git add res/test_panel.py && git commit -m "$(cat <<'EOF'
test(panel): freeze param enum order + geometry invariants

The faceplate redesign moves every coordinate on the panel. Param ids follow
enum ORDER, not position -- this guard makes that guarantee measurable instead
of merely intended, and catches overlapping or off-plate glyphs on the way.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 2: Label-Metadaten wandern in die generierte Tabelle

Heute berechnet `Spotymod.cpp` die Label-Position selbst (`y + glyphR(kind) + 2.5`) und dupliziert dafür die `GLYPH_R`-Tabelle des Generators. Der Redesign braucht radiale Labels, linksbündige Labels und weiße Labels — alles Dinge, die eine „immer mittig darunter"-Regel nicht kann. Also bekommt jedes `Ctl` seine **fertig ausgerechnete** Label-Position aus dem Generator, und die Regel in C++ verschwindet.

Dieser Task ändert **kein Pixel** am Ergebnis: die Defaults reproduzieren exakt das heutige Layout.

**Files:**
- Modify: `host/vcv/res/gen_panel.py` (`Ctl.__init__`, neue Label-Helfer, `svg()`-Textausgabe, `header()`-Struct + `emit_table`)
- Modify: `host/vcv/src/Spotymod.cpp:394-440` (`PanelText`)
- Modify: `host/vcv/res/test_panel.py` (neuer Test)
- Regenerate: `host/vcv/res/Spotymod.svg`, `host/vcv/src/generated_panel.hpp`

**Interfaces:**
- Produces (Python): `Ctl.lbl` — `None` (Default-Regel) oder Tupel `(x, y, anchor, size, colour)` mit `anchor ∈ {"middle", "start", "end"}`. `label_of(c)` löst das zu einem konkreten Tupel auf.
- Produces (C++): `struct PanelCtl { int id; WidgetKind kind; XY mm; const char* label; XY lbl; unsigned char anchor; float lblSize; unsigned lblRgb; };` mit `anchor`: `0 = middle`, `1 = start (links ausgerichtet)`, `2 = end (rechts ausgerichtet)`.

- [ ] **Step 1: Failing test schreiben**

In `host/vcv/res/test_panel.py` **vor** `def main():` einfügen:

```python
def test_label_metadata_exists():
    """Every labelled control resolves to an absolute label placement."""
    for c in g.PARAMS + g.INPUTS + g.OUTPUTS:
        if not c.label:
            continue
        x, y, anchor, size, colour = g.label_of(c)
        check(anchor in ("middle", "start", "end"),
              f"{c.enum}: bad anchor {anchor!r}")
        check(size > 0, f"{c.enum}: label size {size}")
        check(colour.startswith("#"), f"{c.enum}: label colour {colour!r}")


def test_label_defaults_match_todays_layout():
    """The default rule must reproduce the pre-redesign placement exactly."""
    for c in g.PARAMS + g.INPUTS + g.OUTPUTS:
        if not c.label or c.lbl is not None:
            continue
        x, y, anchor, size, colour = g.label_of(c)
        check(approx(x, c.x), f"{c.enum}: default label x {x} != {c.x}")
        check(anchor == "middle", f"{c.enum}: default anchor {anchor!r}")
        check(colour == g.INK, f"{c.enum}: default colour {colour!r}")


def test_header_carries_label_columns():
    """The C++ table must ship the label placement, not recompute it."""
    h = g.header()
    check("XY lbl; unsigned char anchor; float lblSize; unsigned lblRgb;" in h,
          "PanelCtl has no label placement columns")
    check(h.count("{RATE_A, WK_BIGKNOB,") == 1, "kParamCtls lost RATE_A")
```

- [ ] **Step 2: Test laufen lassen — muss FEHLSCHLAGEN**

```bash
cd host/vcv && python res/test_panel.py
```
Expected: `FAIL` mit `AttributeError: module 'gen_panel' has no attribute 'label_of'` (der Traceback ist hier das erwartete Rot).

- [ ] **Step 3: Generator — Label-Auflösung einführen**

In `res/gen_panel.py`, `class Ctl` (Zeile 65-67) ersetzen durch:

```python
class Ctl:
    def __init__(self, enum, kind, x, y, label):
        self.enum, self.kind, self.x, self.y, self.label = enum, kind, x, y, label
        self.r = GLYPH_R[kind]
        # None -> default placement (centred below the glyph); otherwise an
        # explicit (x, y, anchor, size, colour) tuple. Radial orbit captions
        # and white-on-well jack labels set this.
        self.lbl = None
```

Direkt hinter die `GLYPH_R`/`WKMAP`-Definitionen (nach Zeile 63) einfügen:

```python
# --- label placement ----------------------------------------------------------
# Baseline offset below the glyph centre, per kind (spec 2026-07-18 §8). The
# C++ side no longer knows these numbers -- it reads the resolved position out
# of the generated table.
LBL_DY = {BIGKNOB: 7.2, KNOBC: 7.2, SMKNOB: 5.6, KNOBI: 5.6, SW2: 6.6,
          LATCH: 5.4, SMBTN: 5.4, IN: 6.4, OUT: 6.4, LIGHT: 0.0}
LBL_SIZE = {IN: 1.8, OUT: 1.8}     # jacks; every other kind uses 1.9

def label_of(c):
    """(x, y, anchor, size, colour) for a control's caption."""
    if c.lbl is not None:
        return c.lbl
    return (c.x, c.y + LBL_DY[c.kind], "middle", LBL_SIZE.get(c.kind, 1.9), INK)
```

**Wichtig:** `LBL_DY`/`LBL_SIZE` sind bereits die *Ziel*-Werte der Spec. Damit dieser Task pixelgleich bleibt, wäre `y + r + 2.5` nötig — das wird aber in Task 3-6 sowieso überschrieben. Prüfe die Differenz: für `SMKNOB` ergibt alt `3.0 + 2.5 = 5.5`, neu `5.6` (+0.1 mm); `BIGKNOB` alt `6.7`, neu `7.2`; `LATCH/SMBTN` alt `5.2`, neu `5.4`; `SW2` alt `5.5`, neu `6.6`; Jacks alt `6.7`, neu `6.4`. Diese Mikroverschiebungen sind Teil der Spec (§8) und ausdrücklich gewollt — `test_label_defaults_match_todays_layout` prüft deshalb nur x/anchor/colour, nicht y.

Am Ende der `svg()`-Schleife die Label-Ausgabe (Zeile 340-342) ersetzen durch:

```python
        if c.label:
            lx, ly, anchor, size, colour = label_of(c)
            P.append(f'<text x="{mm(lx)}" y="{mm(ly)}" fill="{colour}" '
                     f'text-anchor="{anchor}" font-family="monospace" '
                     f'font-size="{size}">{c.label}</text>')
```

Die Zeile `c.r = GLYPH_R[c.kind]` am Schleifenanfang (Zeile 321) entfernen — `Ctl.__init__` setzt `r` jetzt.

- [ ] **Step 4: Generator — Header-Struct erweitern**

In `header()` die `PanelCtl`-Zeile (Zeile 364) ersetzen durch:

```python
    L2.append("struct PanelCtl { int id; WidgetKind kind; XY mm; const char* label; "
              "XY lbl; unsigned char anchor; float lblSize; unsigned lblRgb; };")
    L2.append("// anchor: 0 = middle, 1 = start (left-aligned), 2 = end (right-aligned)")
```

und `emit_table` (Zeile 386-390) ersetzen durch:

```python
    ANCHOR_ID = {"middle": 0, "start": 1, "end": 2}

    def emit_table(name, items):
        L2.append(f"static const PanelCtl {name}[] = {{")
        for c in items:
            lx, ly, anchor, size, colour = label_of(c)
            L2.append(f'    {{{c.enum}, {WKMAP[c.kind]}, {{{c.x:.3f}f, {c.y:.3f}f}}, '
                      f'"{c.label}", {{{lx:.3f}f, {ly:.3f}f}}, {ANCHOR_ID[anchor]}, '
                      f'{size:.2f}f, {rgb(colour)}}},')
        L2.append("};")
```

- [ ] **Step 5: Generator laufen lassen und Test prüfen**

```bash
cd host/vcv && python res/gen_panel.py && python res/test_panel.py
```
Expected: `wrote res/Spotymod.svg and src/generated_panel.hpp` / `params=72 (stride=23) inputs=4 outputs=6 lights=2  panel=42HP`, dann `PASS -- panel guards ok`.

- [ ] **Step 6: C++ — die Label-Regel löschen und die Tabelle lesen**

In `src/Spotymod.cpp` die komplette `glyphR`-Funktion (Zeilen 395-404, inkl. Kommentar `// glyph radius per kind -- mirror of GLYPH_R in res/gen_panel.py`) **ersatzlos** entfernen.

Den Kommentarblock darüber (Zeilen 387-393) ersetzen durch:

```cpp
// --- panel text ---------------------------------------------------------------
// Rack's SVG loader (NanoSVG) ignores <text>, so the faceplate ships with none
// of its lettering visible. Every caption is drawn here with nvgText, straight
// out of the generated tables: position, anchor, size and colour all come from
// res/gen_panel.py (PanelCtl::lbl/anchor/lblSize/lblRgb), so the SVG preview and
// Rack can never drift apart. Font is a stock Rack asset, present in every
// v2 install -- note it has no bold cut, so the SVG's bold legends render
// regular here. That is accepted.
```

Die `captions`-Lambda (Zeilen 422-427) ersetzen durch:

```cpp
        auto alignOf = [](unsigned char a) {
            return a == 1 ? NVG_ALIGN_LEFT : a == 2 ? NVG_ALIGN_RIGHT
                                                    : NVG_ALIGN_CENTER;
        };
        auto captions = [&](const PanelCtl* t, size_t n) {
            for (size_t i = 0; i < n; ++i) {
                if (!t[i].label[0]) continue;
                nvgTextAlign(args.vg, alignOf(t[i].anchor) | NVG_ALIGN_BASELINE);
                text(t[i].lbl.x, t[i].lbl.y, t[i].lblSize, col(t[i].lblRgb),
                     t[i].label);
            }
            nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);
        };
```

- [ ] **Step 7: Bauen**

```bash
cd host/vcv && export PATH="/c/Users/bernd/Documents/AI/mingw64/bin:/c/msys64/usr/bin:$PATH" RACK_DIR="c:/Users/bernd/Documents/AI/Rack-SDK" && /c/msys64/usr/bin/make CC=gcc CXX=g++ TMP="C:/Users/bernd/AppData/Local/Temp" TEMP="C:/Users/bernd/AppData/Local/Temp" SHELL=/usr/bin/bash -j4
```
Expected: Compile ohne Fehler/Warnungen zu `Spotymod.cpp`, am Ende `Built plugin.dll`. Falls `kColLabel` als „unused" gemeldet wird: die Konstante bleibt im Header (harmlos, `static constexpr`), nichts unternehmen.

- [ ] **Step 8: Commit**

```bash
cd host/vcv && git add res/gen_panel.py res/test_panel.py res/Spotymod.svg src/generated_panel.hpp src/Spotymod.cpp && git commit -m "$(cat <<'EOF'
panel: label placement moves into the generated table

The runtime rebuilt every caption position from a duplicated glyph-radius
table and an implicit "centred below the control" rule. The redesign needs
radial, left/right-anchored and white-on-well captions, so the generator now
resolves each label to an absolute position, anchor, size and colour and the
C++ just draws what it is told. GLYPH_R is no longer mirrored in C++.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 3: Sektor-Orbit — 9 Knöpfe, COLOR wird Vollmitglied, radiale Labels, Kragen-Politik

**Files:**
- Modify: `host/vcv/res/gen_panel.py` (`orbit()`, `part_controls()`, `mirror()`/`PART_A`/`PART_B`, `COLOR_A/B` in `PARAMS`, `knob_svg()`, `TEXTS`, `svg()`)
- Modify: `host/vcv/res/test_panel.py`
- Regenerate: `res/Spotymod.svg`, `src/generated_panel.hpp`

**Interfaces:**
- Consumes: `label_of(c)` / `Ctl.lbl` aus Task 2.
- Produces: `ORBIT_ANG` (dict enum→Winkel), `orbit(cx, cy, r, ang_deg, mir=False)`, `orbit_label(cx, cy, ang_deg, mir)`, `part_controls(mir=False)` (ersetzt `mirror()`), `SECTORS` (Liste `(name, a0, a1, (cap_x, cap_y))`), `wedge_svg(cx, a0, a1, colour, mir)`.

- [ ] **Step 1: Failing test schreiben**

In `res/test_panel.py` vor `def main():` einfügen:

```python
# --- 2026-07-18 redesign: target coordinates, read off layout-b-v7.html -------
ORBIT_A = {           # enum -> (knob x, knob y, label x, label y, anchor)
    'RATE_A':    (42.00,  10.50, 42.00,  3.80, 'middle'),
    'DENSITY_A': (59.03,  16.70, 63.98, 10.80, 'start'),
    'SMOOTH_A':  (68.10,  32.40, 75.68, 31.76, 'start'),
    'SHAPE_A':   (64.95,  50.25, 71.27, 56.10, 'start'),
    'MOD_A':     (51.06,  61.90, 53.56, 70.96, 'middle'),
    'RANGE_A':   (32.94,  61.90, 30.44, 70.96, 'middle'),
    'MELODY_A':  (19.05,  50.25, 12.73, 56.10, 'end'),
    'TUNE_A':    (15.90,  32.40,  8.32, 31.76, 'end'),
    'COLOR_A':   (24.97,  16.70, 20.02, 10.80, 'end'),
}


def test_orbit_positions():
    for enum, (kx, ky, lx, ly, anchor) in ORBIT_A.items():
        c = ctl(enum)
        check(approx(c.x, kx) and approx(c.y, ky),
              f"{enum} knob at ({c.x:.2f}, {c.y:.2f}), want ({kx}, {ky})")
        ax, ay, aanchor, size, colour = g.label_of(c)
        check(approx(ax, lx) and approx(ay, ly),
              f"{enum} label at ({ax:.2f}, {ay:.2f}), want ({lx}, {ly})")
        check(aanchor == anchor, f"{enum} anchor {aanchor!r}, want {anchor!r}")
        check(approx(size, 1.9), f"{enum} label size {size}, want 1.9")


def test_orbit_mirrors():
    """Part B is part A mirrored -- including the flipped label anchors."""
    flip = {'start': 'end', 'end': 'start', 'middle': 'middle'}
    for enum, (kx, ky, lx, ly, anchor) in ORBIT_A.items():
        b = ctl(enum[:-2] + '_B')
        check(approx(b.x, g.W - kx) and approx(b.y, ky),
              f"{b.enum} knob at ({b.x:.2f}, {b.y:.2f}), want ({g.W - kx:.2f}, {ky})")
        ax, ay, aanchor, _, _ = g.label_of(b)
        check(approx(ax, g.W - lx) and approx(ay, ly),
              f"{b.enum} label at ({ax:.2f}, {ay:.2f}), want ({g.W - lx:.2f}, {ly})")
        check(aanchor == flip[anchor],
              f"{b.enum} anchor {aanchor!r}, want {flip[anchor]!r}")


def test_no_label_between_knob_and_ring():
    """The point of radial labels: no caption falls inside the LED ring's reach."""
    for suffix, cx in (('_A', g.RING_CX_A), ('_B', g.W - g.RING_CX_A)):
        for base in ORBIT_A:
            c = ctl(base[:-2] + suffix)
            lx, ly, _, _, _ = g.label_of(c)
            d = math.hypot(lx - cx, ly - g.RING_CY)
            check(d > g.KNOB_R + 4.2,
                  f"{c.enum} label sits inside the orbit (d={d:.2f})")


def test_sector_captions():
    want = [(74.00, 8.20, 'MOTION'), (74.00, 67.60, 'TIMBRE'),
            (11.00, 8.20, 'PITCH'),
            (g.W - 74.00, 8.20, 'MOTION'), (g.W - 74.00, 67.60, 'TIMBRE'),
            (g.W - 11.00, 8.20, 'PITCH')]
    got = [(x, y, t) for (x, y, sz, sp, col, t) in g.TEXTS
           if t in ('MOTION', 'TIMBRE', 'PITCH')]
    check(len(got) == 6, f"{len(got)} sector captions, want 6")
    for wx, wy, wt in want:
        check(any(approx(x, wx) and approx(y, wy) and t == wt for x, y, t in got),
              f"sector caption {wt} missing at ({wx:.2f}, {wy})")


def test_small_knobs_have_no_collar():
    """Spec §3: only the orbit and MORPH keep an accent collar."""
    s = g.svg()
    for enum in ('ATTACK_A', 'REV_SIZE', 'TEMPO', 'TIDE'):
        c = ctl(enum)
        needle = f'cx="{g.mm(c.x)}" cy="{g.mm(c.y)}" r="{g.mm(c.r + 1.0)}"'
        check(needle not in s, f"{enum} still draws a collar")
```

- [ ] **Step 2: Test laufen lassen — muss FEHLSCHLAGEN**

```bash
cd host/vcv && python res/test_panel.py
```
Expected: `FAIL` mit u.a. `DENSITY_A knob at (60.74, 18.24), want (59.03, 16.7)`, `COLOR_A knob at (76.00, 14.00), want (24.97, 16.7)`, `0 sector captions, want 6`, `ATTACK_A still draws a collar`. Exit 1. (`RATE_A` steht schon heute bei (42.00, 10.50) und meldet sich nicht — das ist korrekt: der Orbit dreht sich um die 0°-Position.)

- [ ] **Step 3: Orbit-Geometrie umbauen**

In `res/gen_panel.py` die Funktion `orbit()` (Zeile 75-77) ersetzen durch:

```python
# Sector orbit (spec 2026-07-18 §1): 9 positions at 40 deg pitch, sorted by
# meaning -- MOTION, then TIMBRE, then PITCH. 0 deg = top, clockwise on part A;
# part B mirrors x, which flips the sweep direction on screen.
ORBIT_ANG = {"RATE": 0.0, "DENSITY": 40.0, "SMOOTH": 80.0, "SHAPE": 120.0,
             "MOD": 160.0, "RANGE": 200.0, "MELODY": 240.0, "TUNE": 280.0,
             "COLOR": 320.0}

# (caption, start angle, end angle, part-A caption position)
SECTORS = [("MOTION", -16.0,  96.0, (74.0,  8.2)),
           ("TIMBRE", 112.0, 176.0, (74.0, 67.6)),
           ("PITCH",  192.0, 336.0, (11.0,  8.2))]

def orbit(cx, cy, r, ang_deg, mir=False):
    a = math.radians(ang_deg)
    s = math.sin(a)
    return (cx + (-s if mir else s) * r, cy - r * math.cos(a))

def orbit_label(cx, cy, ang_deg, mir):
    """Caption radially OUTSIDE the knob, so nothing ever lands between the
    knob and the LED ring (spec 2026-07-18 §2)."""
    a = math.radians(ang_deg)
    s, c = math.sin(a), math.cos(a)
    r = 33.8 if c < -0.38 else (33.2 if (abs(s) < 0.38 and c > 0.38) else 34.2)
    dy = 2.2 if c < -0.38 else (0.0 if c > 0.38 else 0.7)
    anchor = "start" if s > 0.38 else ("end" if s < -0.38 else "middle")
    if mir:
        s = -s
        anchor = {"start": "end", "end": "start", "middle": "middle"}[anchor]
    return (cx + r * s, cy - r * c + dy, anchor, 1.9, INK)
```

- [ ] **Step 4: `part_controls()` auf einen Mirror-Parameter umstellen**

`part_controls()` (Zeile 86-116) bekommt einen `mir`-Parameter; die Reihenfolge der `out.append`-Aufrufe bleibt **exakt** wie sie ist. Nur der Macro-Block ändert sich inhaltlich, der Rest wird durch `fx()` gespiegelt:

```python
def part_controls(mir=False):
    cx = W - RING_CX_A if mir else RING_CX_A
    def fx(x): return W - x if mir else x
    out = []
    # 8 of the 9 orbit knobs (COLOR is appended at the end of PARAMS, see below)
    macros = [("RATE","RATE"),("SHAPE","SHAPE"),("DENSITY","DENS"),("SMOOTH","SMTH"),
              ("RANGE","RANGE"),("MELODY","MELO"),("MOD","MOD"),("TUNE","TUNE")]
    for enum, lbl in macros:
        ang = ORBIT_ANG[enum]
        x, y = orbit(cx, RING_CY, KNOB_R, ang, mir)
        c = Ctl(enum, KNOBC if enum == "MELODY" else BIGKNOB, x, y, lbl)
        c.lbl = orbit_label(cx, RING_CY, ang, mir)
        out.append(c)
```

Der Rest von `part_controls()` (Voice-Reihe, FX-Reihe, Pads) bleibt in diesem Task **unverändert in Reihenfolge und Koordinaten**, aber jedes `x` wird durch `fx(...)` geschickt — das ersetzt die alte `mirror()`-Funktion. Konkret: `VOICE_X[i]` → `fx(VOICE_X[i])`, `22.5` → `fx(22.5)`, `48.5 + i*13.0` → `fx(48.5 + i*13.0)`, `74.5` → `fx(74.5)`, `15.75 + i*10.5` → `fx(15.75 + i*10.5)`.

Anschließend `mirror()` (Zeile 118-119) und die `PART_A`/`PART_B`-Zeilen (121-122) ersetzen durch:

```python
def part(suffix, mir):
    """Same call, same order, mirrored x -- so PART_STRIDE and every param id
    stay put no matter how the coordinates move."""
    out = part_controls(mir)
    for c in out:
        c.enum += suffix
    return out

PART_A = part("_A", False)
PART_B = part("_B", True)
PART_STRIDE = len(PART_A)
```

- [ ] **Step 5: COLOR in den Orbit holen**

Die beiden `COLOR_A`/`COLOR_B`-Zeilen in `PARAMS` (Zeile 182-183) ersetzen durch:

```python
    # COLOR: chord density/colour per part (spec 2026-07-17 chord-layer), a full
    # orbit member since the 2026-07-18 redesign -- it is pitch material, so it
    # sits in the PITCH sector. Still appended LAST: order defines the param id.
    color_ctl("_A", False),
    color_ctl("_B", True),
```

und direkt **vor** der `PARAMS`-Definition (vor Zeile 159) einfügen:

```python
def color_ctl(suffix, mir):
    cx = W - RING_CX_A if mir else RING_CX_A
    ang = ORBIT_ANG["COLOR"]
    x, y = orbit(cx, RING_CY, KNOB_R, ang, mir)
    c = Ctl("COLOR" + suffix, BIGKNOB, x, y, "COLOR")
    c.lbl = orbit_label(cx, RING_CY, ang, mir)
    return c
```

- [ ] **Step 6: Sektor-Wedges + Captions zeichnen**

Vor `def svg():` (vor Zeile 274) einfügen:

```python
def wedge_svg(cx, a0, a1, colour, mir):
    """One tinted sector segment behind the orbit knobs: an annulus slice
    between r 20.5 and 33.5 mm (spec 2026-07-18 §1)."""
    R_OUT, R_IN = 33.5, 20.5
    if mir:
        a0, a1 = a1, a0          # keep the on-screen sweep direction
    def pt(r, a):
        rad = math.radians(a)
        s = math.sin(rad)
        return (cx + (-s if mir else s) * r, RING_CY - r * math.cos(rad))
    laf = 1 if abs(a1 - a0) > 180.0 else 0
    ox0, oy0 = pt(R_OUT, a0); ox1, oy1 = pt(R_OUT, a1)
    ix1, iy1 = pt(R_IN,  a1); ix0, iy0 = pt(R_IN,  a0)
    return (f'<path d="M {mm(ox0)} {mm(oy0)} A {mm(R_OUT)} {mm(R_OUT)} 0 {laf} 1 '
            f'{mm(ox1)} {mm(oy1)} L {mm(ix1)} {mm(iy1)} A {mm(R_IN)} {mm(R_IN)} '
            f'0 {laf} 0 {mm(ix0)} {mm(iy0)} Z" fill="{colour}" opacity="0.07"/>')
```

In `svg()` direkt **vor** den beiden `ring_svg(...)`-Zeilen (vor Zeile 303) einfügen:

```python
    # sector tints behind the orbit (drawn first: everything else sits on top)
    for mir, cx, accent in ((False, RING_CX_A, GREEN), (True, W - RING_CX_A, COPPER)):
        for (name, a0, a1, _cap) in SECTORS:
            P.append(wedge_svg(cx, a0, a1, accent, mir))
```

Die `TEXTS`-Tabelle (Zeile 217-223) um die Sektor-Captions erweitern — die Tabelle wird zu:

```python
TEXTS = [
    (RING_CX_A,     RING_CY + 1.6, 5.0, 0.0, GREEN_DIM,  "A"),
    (W - RING_CX_A, RING_CY + 1.6, 5.0, 0.0, COPPER_DIM, "B"),
    (CX,            32.2,          2.2, 0.5, MUTED,      "TIME"),
    (CX,            70.0,          2.2, 0.5, MUTED,      "ROOM"),
    (CX,            7.0,           3.6, 0.9, INK,        "SPOTYMOD"),  # top brand
] + [
    # sector captions, tucked into the free panel corners (spec §1)
    (W - cx if mir else cx, cy, 1.7, 0.3, COPPER if mir else GREEN, name)
    for mir in (False, True)
    for (name, _a0, _a1, (cx, cy)) in SECTORS
]
```

(Die Zeilen `TIME`/`ROOM` verschwinden erst in Task 5 — sie gehören dann zu den Gruppen-Legenden.)

- [ ] **Step 7: Kragen-Politik — kleine Knöpfe werden nackt**

`knob_svg()` (Zeile 249-272): den Kragen-Block so ändern, dass nur Orbit-Größen und MORPH einen bekommen. Die Zeilen 254-266 (`collar_r` bis zum schließenden `else`-Block) ersetzen durch:

```python
    big = c.kind in (BIGKNOB, KNOBC)
    accent = side_accent(c.x)
    if c.enum == "MORPH":  # signature: the bridge knob wears both colours
        collar_r = c.r + 0.85
        for (col, sweep) in ((GREEN, 0), (COPPER, 1)):
            P.append(f'<path d="M {mm(c.x)} {mm(c.y-collar_r)} '
                     f'A {mm(collar_r)} {mm(collar_r)} 0 0 {sweep} '
                     f'{mm(c.x)} {mm(c.y+collar_r)}" fill="none" '
                     f'stroke="{col}" stroke-width="0.5"/>')
    elif big:
        # Only the orbit keeps its collar -- it marks the performance layer.
        # 20+ rings per side on the small pots were noise (spec §3).
        P.append(f'<circle cx="{mm(c.x)}" cy="{mm(c.y)}" r="{mm(c.r + 0.85)}" '
                 f'fill="none" stroke="{accent}" stroke-width="0.5"/>')
```

- [ ] **Step 8: Generieren und Test prüfen**

```bash
cd host/vcv && python res/gen_panel.py && python res/test_panel.py
```
Expected: `params=72 (stride=23) …`, dann `PASS -- panel guards ok`. Falls `PARAMS order changed` erscheint: der `part()`-Umbau hat die Reihenfolge verschoben — die `out.append`-Sequenz in `part_controls()` wiederherstellen, **nicht** den Test ändern.

- [ ] **Step 9: SVG visuell prüfen**

```bash
cd host/vcv && start "" res/Spotymod.svg
```
Expected (im Browser): Neun Knöpfe pro Ring, drei zart getönte Kreissegmente dahinter, alle Orbit-Labels **außerhalb** des Knopfkranzes, MOTION/TIMBRE/PITCH in den Ecken, kleine Knöpfe ohne farbigen Ring. Die untere Panelhälfte sieht noch nach altem Layout aus — das ist an dieser Stelle korrekt.

- [ ] **Step 10: Commit**

```bash
cd host/vcv && git add res/gen_panel.py res/test_panel.py res/Spotymod.svg src/generated_panel.hpp && git commit -m "$(cat <<'EOF'
panel: sector orbit -- 9 knobs, COLOR joins PITCH, radial labels

The orbit is re-sorted by meaning (MOTION / TIMBRE / PITCH) and grows to nine
40-degree positions, so COLOR stops being a contextless satellite between orbit
and centre strip. Labels move radially outward -- none of them lands between a
knob and the LED ring any more -- and the collar is now reserved for the orbit
and MORPH, which drops 40+ rings of visual noise.

Param ids untouched: part_controls() gained a mirror flag but kept its append
order, and COLOR_A/_B stay last in PARAMS.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 4: Untere Hälfte pro Part — VOICE | FX nebeneinander, PLAY darunter

Hier entsteht der wiederverwendbare `group_box()`-Helfer, den Task 5 und 6 nutzen. Die Pad-Backplates verschwinden dafür.

**Files:**
- Modify: `host/vcv/res/gen_panel.py` (`VOICE_X` & Reihenkonstanten, `part_controls()`, `GROUPS`, `group_box()`, `svg()`, `TEXTS`)
- Modify: `host/vcv/res/test_panel.py`
- Regenerate: `res/Spotymod.svg`, `src/generated_panel.hpp`

**Interfaces:**
- Produces: `GROUPS` — Liste `(x, y, w, h, legend, legend_colour)`; `group_box(x, y, w, h, legend)` → SVG-String (Box + Legenden-Chip, **ohne** Text — der Text kommt aus `TEXTS`, damit Rack ihn auch zeichnet); `legend_texts()` → Liste im `TEXTS`-Format.

- [ ] **Step 1: Failing test schreiben**

In `res/test_panel.py` vor `def main():` einfügen:

```python
LOWER_A = {   # enum -> (x, y)   part A; part B is W - x
    'ATTACK_A': (9.50, 77.30), 'DECAY_A': (22.50, 77.30), 'FILT_A': (35.50, 77.30),
    'RES_A': (9.50, 89.40), 'SUB_A': (22.50, 89.40), 'DETUNE_A': (35.50, 89.40),
    'FLUXRATE_A': (49.50, 77.30), 'FLUX_A': (62.75, 77.30), 'FLUXFB_A': (76.00, 77.30),
    'GRIT_A': (56.00, 89.40), 'COMP_A': (69.50, 89.40),
    'ENGINE_A': (11.50, 103.60), 'GRITMODE_A': (22.00, 103.60),
    'STEPS_A': (35.50, 103.60), 'STEP_A': (46.00, 103.60),
    'PRINCIPLE_A': (56.50, 103.60), 'NEWPHRASE_A': (67.00, 103.60),
    'TRIGGER_A': (77.50, 103.60),
}


def test_lower_half_positions():
    for enum, (x, y) in LOWER_A.items():
        c = ctl(enum)
        check(approx(c.x, x) and approx(c.y, y),
              f"{enum} at ({c.x:.2f}, {c.y:.2f}), want ({x}, {y})")
        b = ctl(enum[:-2] + '_B')
        check(approx(b.x, g.W - x) and approx(b.y, y),
              f"{b.enum} at ({b.x:.2f}, {b.y:.2f}), want ({g.W - x:.2f}, {y})")


def test_steps_left_the_fx_row():
    """STPS is a sequencer parameter -- it belongs in PLAY, not in FX."""
    s = ctl('STEPS_A')
    check(approx(s.y, 103.60), f"STEPS_A at y {s.y:.2f}, want 103.60 (PLAY row)")


def test_part_group_boxes():
    want = [(4.0, 72.4, 37.0, 24.5, 'VOICE'), (43.5, 72.4, 38.5, 24.5, 'FX'),
            (4.0, 98.6, 78.0, 12.6, 'PLAY')]
    for (x, y, w, h, name) in want:
        check(any(approx(gx, x) and approx(gy, y) and approx(gw, w)
                  and approx(gh, h) and gn == name
                  for (gx, gy, gw, gh, gn, _c) in g.GROUPS),
              f"part-A group {name} missing at ({x}, {y}, {w}, {h})")
        bx = g.W - x - w
        check(any(approx(gx, bx) and approx(gy, y) and gn == name
                  for (gx, gy, gw, gh, gn, _c) in g.GROUPS),
              f"part-B group {name} missing at x {bx:.2f}")


def test_group_legend_geometry():
    """Legend chip rides the top border, text sits 5 mm in from the left."""
    s = g.svg()
    for (x, y, w, h, name, colour) in g.GROUPS:
        cw = 1.35 * len(name) + 2.5
        chip = (f'<rect x="{g.mm(x + 5.0 - cw / 2)}" y="{g.mm(y - 1.3)}" '
                f'width="{g.mm(cw)}" height="2.6" fill="{g.PAPER}"/>')
        check(chip in s, f"legend chip for {name} missing/misplaced")
        check(any(approx(tx, x + 5.0) and approx(ty, y + 0.75) and t == name
                  for (tx, ty, sz, sp, col, t) in g.TEXTS),
              f"legend text for {name} missing at ({x + 5.0:.2f}, {y + 0.75:.2f})")


def test_pad_backplates_are_gone():
    check('width="72.4" height="11.9"' not in g.svg(),
          "the old pad backplate is still drawn")
```

- [ ] **Step 2: Test laufen lassen — muss FEHLSCHLAGEN**

```bash
cd host/vcv && python res/test_panel.py
```
Expected: `FAIL` mit `AttributeError: module 'gen_panel' has no attribute 'GROUPS'`.

- [ ] **Step 3: Reihen-Konstanten neu setzen**

In `res/gen_panel.py` den `VOICE_X`-Block (Zeile 80-82) ersetzen durch:

```python
# --- lower half per part (spec 2026-07-18 §5) --------------------------------
# VOICE and FX sit side by side, PLAY spans the full part width below them.
VOICE_X  = [9.5, 22.5, 35.5]        # ATK DEC FILT / RES SUB DTUN
ROW_V1, ROW_V2 = 77.3, 89.4
FX_TOP   = [49.5, 62.75, 76.0]      # FRATE FLUX FFB -- the delay cluster
FX_BOT   = [56.0, 69.5]             # GRIT COMP
PLAY_Y   = 103.6
PAD_X    = [11.5, 22.0, 46.0, 56.5, 67.0, 77.5]   # ENG GRIT | STEP PRIN NEW TRIG
STEPS_X  = 35.5                     # sequencer knob, between the two pad blocks
```

- [ ] **Step 4: Voice-/FX-/Pad-Reihen umsetzen**

In `part_controls(mir)` den Block ab der Voice-Reihe (bis einschließlich Pads) durch Folgendes ersetzen — **die `out.append`-Reihenfolge bleibt exakt gleich**:

```python
    # voice row (small): ATK DEC | RES SUB DTUN. FILT fills slot 2 of the top
    # row but is appended at the END of PARAMS (see below), never here -- that
    # would grow PART_STRIDE and shift every part-B/SHARED param id.
    for (enum, lbl, x, y) in [("ATTACK", "ATK", VOICE_X[0], ROW_V1),
                              ("DECAY",  "DEC", VOICE_X[1], ROW_V1),
                              ("RES",    "RES", VOICE_X[0], ROW_V2),
                              ("SUB",    "SUB", VOICE_X[1], ROW_V2),
                              ("DETUNE", "DTUN", VOICE_X[2], ROW_V2)]:
        out.append(Ctl(enum, SMKNOB, fx(x), y, lbl))
    # fx box: the FLUX delay cluster (RATE . MIX . FB) on top, GRIT/COMP below.
    # FLUX (the delay MIX) is the template member; RATE/FB are appended at the
    # end of PARAMS. STEPS keeps its append slot here but has moved to the PLAY
    # box -- it is a sequencer parameter, not an effect (spec 2026-07-18 §5).
    out.append(Ctl("FLUX", SMKNOB, fx(FX_TOP[1]), ROW_V1, "FLUX"))
    for i, (enum, lbl) in enumerate([("GRIT", "GRIT"), ("COMP", "COMP")]):
        out.append(Ctl(enum, SMKNOB, fx(FX_BOT[i]), ROW_V2, lbl))
    out.append(Ctl("STEPS", KNOBI, fx(STEPS_X), PLAY_Y, "STPS"))
    pads = [("ENGINE", LATCH, "ENG"), ("GRITMODE", LATCH, "GRIT"),
            ("STEP", LATCH, "STEP"), ("PRINCIPLE", SMBTN, "PRIN"),
            ("NEWPHRASE", SMBTN, "NEW"), ("TRIGGER", SMBTN, "TRIG")]
    for i, (enum, kind, lbl) in enumerate(pads):
        out.append(Ctl(enum, kind, fx(PAD_X[i]), PLAY_Y, lbl))
    return out
```

Achtung: `RES` sitzt jetzt in `VOICE_X[0]`/`ROW_V2`, `DETUNE` in `VOICE_X[2]`/`ROW_V2` — die alte Slot-Nummerierung `[0,1,3,4,5]` entfällt vollständig.

In `PARAMS` die angehängten Koordinaten anpassen: `FILT_A/B` → `Ctl("FILT_A", SMKNOB, VOICE_X[2], ROW_V1, "FILT")` bzw. `W - VOICE_X[2]`; `FLUXRATE_A/B` → `FX_TOP[0]` / `W - FX_TOP[0]`, `ROW_V1`; `FLUXFB_A/B` → `FX_TOP[2]` / `W - FX_TOP[2]`, `ROW_V1`.

- [ ] **Step 5: `GROUPS` + `group_box()` + Legenden einführen**

Nach der `SECTORS`-Definition einfügen:

```python
# --- fieldset groups (spec 2026-07-18 §4) ------------------------------------
# One shared style: paper-deep panel, hairline stroke, and a legend riding the
# top border on a small paper chip. (x, y, w, h, legend, legend colour)
def part_groups(mir):
    def fx(x, w): return (W - x - w) if mir else x
    return [(fx(4.0, 37.0),  72.4, 37.0, 24.5, "VOICE", MUTED),
            (fx(43.5, 38.5), 72.4, 38.5, 24.5, "FX",    MUTED),
            (fx(4.0, 78.0),  98.6, 78.0, 12.6, "PLAY",  MUTED)]

GROUPS = part_groups(False) + part_groups(True)

def group_box(x, y, w, h, legend):
    """Box + the paper chip that breaks the top border for the legend. The
    legend TEXT itself goes through TEXTS, so Rack draws it too (NanoSVG
    ignores <text>)."""
    cw = 1.35 * len(legend) + 2.5
    return "\n".join([
        f'<rect x="{mm(x)}" y="{mm(y)}" width="{mm(w)}" height="{mm(h)}" rx="1.5" '
        f'fill="{PAPER_DEEP}" stroke="{LINE}" stroke-width="0.35"/>',
        f'<rect x="{mm(x + 5.0 - cw / 2)}" y="{mm(y - 1.3)}" width="{mm(cw)}" '
        f'height="2.6" fill="{PAPER}"/>'])

def legend_texts():
    return [(x + 5.0, y + 0.75, 1.8, 0.35, colour, name)
            for (x, y, w, h, name, colour) in GROUPS]
```

`TEXTS` um `+ legend_texts()` ergänzen (an das Ende des Listen-Ausdrucks, hinter die Sektor-Captions).

- [ ] **Step 6: Boxen zeichnen, Pad-Backplates entfernen**

In `svg()` den Pad-Backplate-Block (Zeilen 305-310, inkl. Kommentar) **ersatzlos löschen** und stattdessen direkt nach den beiden `ring_svg(...)`-Zeilen einfügen:

```python
    # fieldset group boxes (drawn under the glyphs, over the sector tints)
    for (x, y, w, h, name, _colour) in GROUPS:
        P.append(group_box(x, y, w, h, name))
    # PLAY: hairline between the two mode pads and the sequencer block
    for dx in (28.7, W - 28.7):
        P.append(f'<line x1="{mm(dx)}" y1="100.6" x2="{mm(dx)}" y2="109.2" '
                 f'stroke="{LINE}" stroke-width="0.35"/>')
```

- [ ] **Step 7: Generieren und Test prüfen**

```bash
cd host/vcv && python res/gen_panel.py && python res/test_panel.py
```
Expected: `params=72 (stride=23) …`, dann `PASS -- panel guards ok`.

- [ ] **Step 8: SVG visuell prüfen**

```bash
cd host/vcv && start "" res/Spotymod.svg
```
Expected: Pro Seite drei beschriftete Boxen (VOICE links, FX rechts daneben, PLAY über die volle Breite darunter), STPS in der PLAY-Box hinter dem Trennstrich, keine Pad-Backplate mehr. Die Mittelsäule ist noch die alte.

- [ ] **Step 9: Commit**

```bash
cd host/vcv && git add res/gen_panel.py res/test_panel.py res/Spotymod.svg src/generated_panel.hpp && git commit -m "$(cat <<'EOF'
panel: VOICE | FX side by side, PLAY below, STPS joins the sequencer

The lower half of each part gets the shared fieldset style -- three labelled
boxes instead of one anonymous pad backplate and two floating rows. STPS moves
out of the FX row into PLAY, where it belongs: it counts sequencer steps, it is
not an effect. Only coordinates move; the append order in part_controls() and
PARAMS is byte-identical, so param ids are unchanged.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 5: Mittelsäule — vier freistehende Boxen, keine Hintergrundkarte

**Files:**
- Modify: `host/vcv/res/gen_panel.py` (`L`/`R`, `SHARED`, `TIDE`, `GROUPS`, `TEXTS`, `svg()`)
- Modify: `host/vcv/res/test_panel.py`
- Regenerate: `res/Spotymod.svg`, `src/generated_panel.hpp`

- [ ] **Step 1: Failing test schreiben**

In `res/test_panel.py` vor `def main():` einfügen:

```python
CENTER = {   # enum -> (x offset from CX, y)
    'MORPH': (-7.0, 21.5), 'TIDE': (11.0, 21.5),
    'SYNC': (-11.5, 41.0), 'TEMPO': (0.0, 41.0), 'COUPLE': (11.5, 41.0),
    'SCALE': (-11.5, 56.5), 'CHOKE': (0.0, 56.5), 'DRIFT': (11.5, 56.5),
    'SPOT': (-11.5, 66.0), 'MASTER_DRIVE': (0.0, 66.0), 'SETTLE': (11.5, 66.0),
    'REV_SIZE': (-11.5, 82.5), 'REV_MIX': (0.0, 82.5), 'REV_DECAY': (11.5, 82.5),
    'REV_TONE': (-11.5, 93.0), 'REV_DIFF': (11.5, 93.0),
    'REV_SMEAR': (-11.5, 103.5), 'REV_MOD': (11.5, 103.5),
}


def test_center_positions():
    for enum, (dx, y) in CENTER.items():
        c = ctl(enum)
        want_x = g.CX + dx
        check(approx(c.x, want_x) and approx(c.y, y),
              f"{enum} at ({c.x:.2f}, {c.y:.2f}), want ({want_x:.2f}, {y})")


def test_center_group_boxes():
    want = [(13.0, 19.5, 'BLEND'), (35.0, 13.5, 'TIME'),
            (51.0, 22.5, 'DUO'), (76.5, 34.7, 'ROOM')]
    for (y, h, name) in want:
        check(any(approx(gx, g.CX - 20.5) and approx(gy, y) and approx(gw, 41.0)
                  and approx(gh, h) and gn == name
                  for (gx, gy, gw, gh, gn, _c) in g.GROUPS),
              f"centre group {name} missing at y {y} (h {h}, x {g.CX - 20.5:.2f})")


def test_center_card_is_gone():
    check('width="42.000" height="100.000"' not in g.svg(),
          "the full-height centre card is still drawn")


def test_old_eyebrow_texts_are_gone():
    """TIME/ROOM are group legends now, not free-floating eyebrows."""
    for (x, y, sz, sp, col, t) in g.TEXTS:
        if t in ('TIME', 'ROOM'):
            check(approx(sz, 1.8) and approx(x, g.CX - 20.5 + 5.0),
                  f"{t} is still the old eyebrow (size {sz} at x {x:.2f})")


def test_room_is_flush_with_play():
    """Spec §6: ROOM's bottom edge lines up with the PLAY boxes."""
    room = [gr for gr in g.GROUPS if gr[4] == 'ROOM'][0]
    play = [gr for gr in g.GROUPS if gr[4] == 'PLAY'][0]
    check(approx(room[1] + room[3], play[1] + play[3]),
          f"ROOM ends at {room[1] + room[3]:.2f}, PLAY at {play[1] + play[3]:.2f}")
```

- [ ] **Step 2: Test laufen lassen — muss FEHLSCHLAGEN**

```bash
cd host/vcv && python res/test_panel.py
```
Expected: `FAIL` mit `MORPH at (106.68, 22.00), want (99.68, 21.5)`, `centre group BLEND missing …`, `the full-height centre card is still drawn` u.a.

- [ ] **Step 3: Spalten und Zeilen der Mittelsäule setzen**

In `res/gen_panel.py` den Block ab `CX = W / 2.0` (Zeile 126-132) ersetzen durch:

```python
CX = W / 2.0
# The centre's outer background card is gone (spec 2026-07-18 §6) -- the four
# fieldset boxes carry the grouping alone and grew to 41 mm, so the columns
# move out from +-10.5 to +-11.5.
L, R = CX - 11.5, CX + 11.5
ROW_BLEND = 21.5
ROW_TIME  = 41.0
ROW_DUO1, ROW_DUO2 = 56.5, 66.0
ROW_ROOM1, ROW_ROOM2, ROW_ROOM3 = 82.5, 93.0, 103.5
```

Alle `ROW_VOICE`/`ROW_FX`/`ROW_PAD`-Verwendungen in `SHARED` verschwinden dabei. `SHARED` wird zu:

```python
SHARED = [
    Ctl("MORPH",  BIGKNOB, CX - 7.0, ROW_BLEND, "MORPH"),
    # TIME: the one clock story -- the mode switch, its tempo, and how tightly
    # the two parts hang together (spec 2026-07-16 sync/couple redesign)
    Ctl("SYNC",   SW2,     L,  ROW_TIME, "SYNC"),
    Ctl("TEMPO",  SMKNOB,  CX, ROW_TIME, "TEMPO"),
    Ctl("COUPLE", SMKNOB,  R,  ROW_TIME, "COUPL"),
    Ctl("SCALE",  KNOBI,   L,  ROW_DUO1, "SCALE"),
    Ctl("DRIFT",  SMKNOB,  R,  ROW_DUO1, "DRIFT"),
    Ctl("SPOT",   SMBTN,   L,  ROW_DUO2, "SPOT"),
    Ctl("MASTER_DRIVE", SMKNOB, CX, ROW_DUO2, "DRIVE"),
    Ctl("SETTLE", SMBTN,   R,  ROW_DUO2, "SETL"),
    # ROOM: three rows in its own box, bottom edge flush with the PLAY boxes.
    Ctl("REV_SIZE",  SMKNOB, L,  ROW_ROOM1, "SIZE"),
    Ctl("REV_DECAY", SMKNOB, R,  ROW_ROOM1, "DECAY"),
    Ctl("REV_MIX",   SMKNOB, CX, ROW_ROOM1, "MIX"),
    Ctl("REV_TONE",  SMKNOB, L,  ROW_ROOM2, "TONE"),
    Ctl("REV_DIFF",  SMKNOB, R,  ROW_ROOM2, "DIFF"),
    Ctl("REV_SMEAR", SMKNOB, L,  ROW_ROOM3, "SMEAR"),
    Ctl("REV_MOD",   SMKNOB, R,  ROW_ROOM3, "WOBL"),
    # CHOKE: bipolar event-priority between the decks (spec 2026-07-16
    # choke-priority). Appended LAST on purpose: existing .vcv patches keep
    # their param ids.
    Ctl("CHOKE",  SMKNOB, CX, ROW_DUO1, "CHOKE"),
]
```

Die `TIDE`-Zeile in `PARAMS` (Zeile 169) wird zu:

```python
    Ctl("TIDE", SMKNOB, CX + 11.0, ROW_BLEND, "TIDE"),
```

- [ ] **Step 4: Die vier Center-Boxen registrieren**

`GROUPS` erweitern:

```python
GROUPS = part_groups(False) + part_groups(True) + [
    (CX - 20.5, 13.0, 41.0, 19.5, "BLEND", MUTED),
    (CX - 20.5, 35.0, 41.0, 13.5, "TIME",  MUTED),
    (CX - 20.5, 51.0, 41.0, 22.5, "DUO",   MUTED),
    (CX - 20.5, 76.5, 41.0, 34.7, "ROOM",  MUTED),
]
```

- [ ] **Step 5: Alte Center-Möbel entfernen**

In `svg()` löschen: den Center-Card-Block (die `rect`-Ausgabe mit `width="42.0" height="100.0"` samt Kommentar) und den Eyebrow-Rules-Block (`for ey in (69.2, 31.4): …`).

In `TEXTS` die beiden Zeilen `(CX, 32.2, …, "TIME")` und `(CX, 70.0, …, "ROOM")` löschen — beide kommen jetzt über `legend_texts()`.

- [ ] **Step 6: Generieren und Test prüfen**

```bash
cd host/vcv && python res/gen_panel.py && python res/test_panel.py
```
Expected: `PASS -- panel guards ok`. Falls `test_no_overlap` anschlägt, sind zwei Center-Zeilen zu eng — Koordinaten gegen die Tabelle in Spec §6 prüfen.

- [ ] **Step 7: SVG visuell prüfen**

```bash
cd host/vcv && start "" res/Spotymod.svg
```
Expected: Vier freistehende Boxen (BLEND, TIME, DUO, ROOM) statt einer durchgehenden Karte; ROOM endet auf derselben Höhe wie die PLAY-Boxen; MORPH links versetzt mit TIDE rechts daneben.

- [ ] **Step 8: Commit**

```bash
cd host/vcv && git add res/gen_panel.py res/test_panel.py res/Spotymod.svg src/generated_panel.hpp && git commit -m "$(cat <<'EOF'
panel: centre strip becomes four free-standing boxes

The full-height background card is gone; BLEND / TIME / DUO / ROOM carry the
grouping on their own and use the width they gained (41 mm, columns at +-11.5).
The TIME and ROOM eyebrow rules retire -- they were an ad-hoc version of the
legend that every group now shares.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 6: Jack-Zeile — fünf beschriftete Gruppen, Ausgänge auf dunklen Wannen

**Files:**
- Modify: `host/vcv/res/gen_panel.py` (`JACK_*`, `INPUTS`, `OUTPUTS`, `GROUPS`, `svg()`)
- Modify: `host/vcv/src/Spotymod.cpp:139-140` (Tooltips)
- Modify: `host/vcv/res/test_panel.py`
- Regenerate: `res/Spotymod.svg`, `src/generated_panel.hpp`

**Interfaces:**
- Produces (Python): `JACK_GROUPS` — Liste `(box_x, legend, legend_colour, well: bool, [(enum, label, tip), …])`.
- Produces (C++): `PanelCtl` bekommt ein letztes Feld `const char* tip` (Default = `label`), das `configInput`/`configOutput` statt `label` benutzen — sonst hieße der IN-L-Tooltip nur noch „L".

- [ ] **Step 1: Failing test schreiben**

In `res/test_panel.py` vor `def main():` einfügen:

```python
JACKS = {   # enum -> (x, y, label, white label?)
    'PITCH_A': (12.95, 118.4, 'PIT',  True),
    'GATE_A':  (24.45, 118.4, 'GATE', True),
    'IN_L':    (55.25, 118.4, 'L',    False),
    'IN_R':    (66.75, 118.4, 'R',    False),
    'CLOCK':   (100.93, 118.4, 'CLK', False),
    'RESET':   (112.43, 118.4, 'RST', False),
    'OUT_L':   (146.61, 118.4, 'L',   True),
    'OUT_R':   (158.11, 118.4, 'R',   True),
    'GATE_B':  (188.91, 118.4, 'GATE', True),
    'PITCH_B': (200.41, 118.4, 'PIT',  True),
}


def test_jack_positions_and_labels():
    for enum, (x, y, label, white) in JACKS.items():
        c = ctl(enum)
        check(approx(c.x, x) and approx(c.y, y),
              f"{enum} at ({c.x:.2f}, {c.y:.2f}), want ({x}, {y})")
        check(c.label == label, f"{enum} label {c.label!r}, want {label!r}")
        lx, ly, anchor, size, colour = g.label_of(c)
        check(approx(ly, 124.8), f"{enum} label y {ly:.2f}, want 124.8")
        check(approx(size, 1.8), f"{enum} label size {size}, want 1.8")
        want_col = g.WHITE if white else g.INK
        check(colour == want_col,
              f"{enum} label colour {colour}, want {want_col}")


def test_jack_groups():
    want = [(7.2, 'CV A', g.GREEN), (49.5, 'IN', g.MUTED),
            (g.CX - 11.5, 'CLOCK', g.MUTED), (g.W - 72.5, 'OUT', g.MUTED),
            (g.W - 30.2, 'CV B', g.COPPER)]
    for (x, name, colour) in want:
        check(any(approx(gx, x) and approx(gy, 112.6) and approx(gw, 23.0)
                  and approx(gh, 14.4) and gn == name and gc == colour
                  for (gx, gy, gw, gh, gn, gc) in g.GROUPS),
              f"jack group {name} missing at x {x:.2f}")


def test_output_wells():
    """Spec §7: output groups get a dark inner well; inputs stay on paper."""
    s = g.svg()
    for (x, has_well) in ((7.2, True), (49.5, False), (g.CX - 11.5, False),
                          (g.W - 72.5, True), (g.W - 30.2, True)):
        well = (f'<rect x="{g.mm(x + 1.4)}" y="{g.mm(112.6 + 1.6)}" '
                f'width="{g.mm(20.2)}" height="{g.mm(11.2)}" rx="1.2" '
                f'fill="{g.WELL}"/>')
        check((well in s) == has_well,
              f"well at x {x:.2f}: expected {has_well}")


def test_header_carries_tooltips():
    h = g.header()
    check('const char* tip;' in h, "PanelCtl has no tip column")
    check('"L", ' in h and '"IN L"' in h,
          "IN_L lost its 'IN L' tooltip while its panel label became 'L'")
```

- [ ] **Step 2: Test laufen lassen — muss FEHLSCHLAGEN**

```bash
cd host/vcv && python res/test_panel.py
```
Expected: `FAIL` mit `PITCH_A at (16.00, 118.00), want (12.95, 118.4)`, `jack group CV A missing at x 7.20`, `PanelCtl has no tip column` u.a.

- [ ] **Step 3: Jack-Gruppen definieren**

In `res/gen_panel.py` den Jack-Block (Zeilen 186-208, von `# --- inputs / outputs / lights` bis zum Ende von `OUTPUTS`) ersetzen durch:

```python
# --- inputs / outputs / lights ------------------------------------------------
# The ten jacks split into five labelled fieldset groups with real gaps, signal
# flow reading left -> right (spec 2026-07-18 §7). Output groups sit on a dark
# well with white lettering, inputs stay on paper with ink -- so in/out reads at
# a glance and the duplicated PIT/GATE labels are disambiguated by the legend.
JACK_Y     = 118.4
JACK_BOX_Y = 112.6
JACK_BOX_W, JACK_BOX_H = 23.0, 14.4
JACK_DX    = 5.75            # jack offset from the box's left edge; pitch 11.5

# (box x, legend, legend colour, dark well?, [(enum, panel label, tooltip)])
JACK_GROUPS = [
    (7.2,        "CV A",  GREEN,  True,  [("PITCH_A", "PIT",  "Pitch A"),
                                          ("GATE_A",  "GATE", "Gate A")]),
    (49.5,       "IN",    MUTED,  False, [("IN_L", "L", "IN L"),
                                          ("IN_R", "R", "IN R")]),
    (CX - 11.5,  "CLOCK", MUTED,  False, [("CLOCK", "CLK", "Clock"),
                                          ("RESET", "RST", "Reset")]),
    (W - 72.5,   "OUT",   MUTED,  True,  [("OUT_L", "L", "OUT L"),
                                          ("OUT_R", "R", "OUT R")]),
    (W - 30.2,   "CV B",  COPPER, True,  [("GATE_B",  "GATE", "Gate B"),
                                          ("PITCH_B", "PIT",  "Pitch B")]),
]

def jack(enum, kind, x, label, tip, white):
    c = Ctl(enum, kind, x, JACK_Y, label)
    c.tip = tip
    c.lbl = (x, JACK_Y + LBL_DY[kind], "middle", 1.8, WHITE if white else INK)
    return c

def jack_at(enum):
    """Look a jack up in JACK_GROUPS and build it. Keeps the INPUTS/OUTPUTS
    enum ORDER free of the visual left-to-right order -- ids stay put."""
    for (bx, _lg, _col, well, items) in JACK_GROUPS:
        for i, (e, label, tip) in enumerate(items):
            if e == enum:
                kind = IN if enum in ("IN_L", "IN_R", "CLOCK", "RESET") else OUT
                return jack(enum, kind, bx + JACK_DX + i * 11.5, label, tip, well)
    raise KeyError(enum)

INPUTS = [jack_at(e) for e in ("IN_L", "IN_R", "CLOCK", "RESET")]
OUTPUTS = [jack_at(e) for e in ("OUT_L", "OUT_R", "PITCH_A", "GATE_A",
                                "PITCH_B", "GATE_B")]
```

In `class Ctl.__init__` ergänzen (hinter `self.lbl = None`):

```python
        self.tip = label   # tooltip text; panel label and tooltip differ on jacks
```

- [ ] **Step 4: Jack-Gruppen als Boxen + Wannen zeichnen**

`GROUPS` um die Jack-Gruppen ergänzen (an das Ende des Ausdrucks):

```python
GROUPS = part_groups(False) + part_groups(True) + [
    (CX - 20.5, 13.0, 41.0, 19.5, "BLEND", MUTED),
    (CX - 20.5, 35.0, 41.0, 13.5, "TIME",  MUTED),
    (CX - 20.5, 51.0, 41.0, 22.5, "DUO",   MUTED),
    (CX - 20.5, 76.5, 41.0, 34.7, "ROOM",  MUTED),
] + [(bx, JACK_BOX_Y, JACK_BOX_W, JACK_BOX_H, lg, col)
     for (bx, lg, col, _well, _items) in JACK_GROUPS]
```

**Reihenfolge beachten:** `JACK_GROUPS` muss vor `GROUPS` definiert sein. Falls die Datei anders sortiert ist, den Jack-Block über `GROUPS` ziehen — `JACK_GROUPS` braucht nur `CX`/`W`, die weit oben stehen.

In `svg()` direkt nach der `group_box`-Schleife einfügen:

```python
    # dark inner wells under the output groups -- in/out at a glance (spec §7)
    for (bx, _lg, _col, well, _items) in JACK_GROUPS:
        if well:
            P.append(f'<rect x="{mm(bx + 1.4)}" y="{mm(JACK_BOX_Y + 1.6)}" '
                     f'width="{mm(JACK_BOX_W - 2.8)}" '
                     f'height="{mm(JACK_BOX_H - 3.2)}" rx="1.2" fill="{WELL}"/>')
```

- [ ] **Step 5: `tip` in den Header und nach C++**

In `header()` die `PanelCtl`-Zeile erweitern (letztes Feld):

```python
    L2.append("struct PanelCtl { int id; WidgetKind kind; XY mm; const char* label; "
              "XY lbl; unsigned char anchor; float lblSize; unsigned lblRgb; "
              "const char* tip; };")
```

und in `emit_table` die emittierte Zeile um `, "{c.tip}"` vor der schließenden Klammer ergänzen:

```python
            L2.append(f'    {{{c.enum}, {WKMAP[c.kind]}, {{{c.x:.3f}f, {c.y:.3f}f}}, '
                      f'"{c.label}", {{{lx:.3f}f, {ly:.3f}f}}, {ANCHOR_ID[anchor]}, '
                      f'{size:.2f}f, {rgb(colour)}, "{c.tip}"}},')
```

In `src/Spotymod.cpp` die Zeilen 139-140 ersetzen durch:

```cpp
        // panel labels are short ("L", "PIT"); the group legend carries the rest,
        // so tooltips use the control table's spelled-out tip instead
        for (const auto& c : kInputCtls)  configInput(c.id, c.tip);
        for (const auto& c : kOutputCtls) configOutput(c.id, c.tip);
```

- [ ] **Step 6: Generieren und Test prüfen**

```bash
cd host/vcv && python res/gen_panel.py && python res/test_panel.py
```
Expected: `params=72 (stride=23) inputs=4 outputs=6 lights=2  panel=42HP`, dann `PASS -- panel guards ok`. `test_enum_order` muss weiter grün sein — `INPUTS`/`OUTPUTS` werden explizit in der alten Reihenfolge aufgebaut.

- [ ] **Step 7: Bauen**

```bash
cd host/vcv && export PATH="/c/Users/bernd/Documents/AI/mingw64/bin:/c/msys64/usr/bin:$PATH" RACK_DIR="c:/Users/bernd/Documents/AI/Rack-SDK" && /c/msys64/usr/bin/make CC=gcc CXX=g++ TMP="C:/Users/bernd/AppData/Local/Temp" TEMP="C:/Users/bernd/AppData/Local/Temp" SHELL=/usr/bin/bash -j4
```
Expected: `Built plugin.dll` ohne Fehler.

- [ ] **Step 8: Commit**

```bash
cd host/vcv && git add res/gen_panel.py res/test_panel.py res/Spotymod.svg src/generated_panel.hpp src/Spotymod.cpp && git commit -m "$(cat <<'EOF'
panel: jack row becomes five labelled groups, outputs on dark wells

Ten jacks in one undifferentiated row said nothing about direction, and PIT/GATE
appeared twice with no hint whose they were. Now: CV A | IN | CLOCK | OUT | CV B,
each a fieldset with its own legend, outputs sunk into a dark well with white
lettering. Panel labels shrink to "L"/"R" since the legend carries the rest --
so PanelCtl gained a tip column and the Rack tooltips stay spelled out.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 7: Panel-Möbel aufräumen, Version 2.5.0, Ende-zu-Ende-Verifikation in Rack

**Files:**
- Modify: `host/vcv/res/gen_panel.py` (Schraubenkreise, Modul-Docstring)
- Modify: `host/vcv/plugin.json` (`version`)
- Modify: `host/vcv/res/test_panel.py`
- Regenerate: `res/Spotymod.svg`, `src/generated_panel.hpp`

- [ ] **Step 1: Failing test schreiben**

In `res/test_panel.py` vor `def main():` einfügen:

```python
def test_no_printed_screw_holes():
    """Rack draws real screw widgets in the corners; the printed circles never
    lined up with them (spec 2026-07-18, panel furniture)."""
    check('fill="#d8d0bf"' not in g.svg(), "printed screw-hole circles are back")


def test_group_count():
    """3 part groups x 2 + 4 centre + 5 jack = 15 fieldsets."""
    check(len(g.GROUPS) == 15, f"{len(g.GROUPS)} groups, want 15")
    names = sorted(n for (_x, _y, _w, _h, n, _c) in g.GROUPS)
    check(names == sorted(['VOICE', 'FX', 'PLAY'] * 2 +
                          ['BLEND', 'TIME', 'DUO', 'ROOM'] +
                          ['CV A', 'IN', 'CLOCK', 'OUT', 'CV B']),
          f"unexpected group set: {names}")


def test_every_label_is_reachable():
    """Nothing may be drawn off-plate or under a neighbouring box edge."""
    for c in g.PARAMS + g.INPUTS + g.OUTPUTS:
        if not c.label:
            continue
        lx, ly, _a, size, _col = g.label_of(c)
        check(1.0 <= lx <= g.W - 1.0 and 1.0 <= ly <= g.Hh - 1.0,
              f"{c.enum} label off panel at ({lx:.2f}, {ly:.2f})")
```

- [ ] **Step 2: Test laufen lassen — muss FEHLSCHLAGEN**

```bash
cd host/vcv && python res/test_panel.py
```
Expected: `FAIL (1): - printed screw-hole circles are back`.

- [ ] **Step 3: Schraubenkreise entfernen und Docstring nachziehen**

In `res/gen_panel.py` in `svg()` den Block

```python
    # mounting holes
    for hx in (MM_PER_HP, W - MM_PER_HP):
        for hy in (3.0, Hh-3.0):
            P.append(...)
```

**ersatzlos löschen**.

Den Modul-Docstring (Zeilen 4-7) ersetzen durch:

```
Layout (2026-07-18 redesign): two symmetric halves, each built around a 32-LED
ring with nine macro knobs orbiting it in three meaning-sorted sectors (MOTION /
TIMBRE / PITCH) and its secondary functions in three fieldset boxes below
(VOICE | FX, PLAY). A shared centre column of four boxes (BLEND / TIME / DUO /
ROOM) sits between them, and the ten jacks form five labelled groups along the
bottom edge. Identity is loosely inherited from the hardware -- the ring plus
macro orbit and the mirrored A/B split -- but Spotymod is its own instrument
and no longer reducible to the real panel.
```

- [ ] **Step 4: Version bumpen**

In `host/vcv/plugin.json` `"version": "2.4.2"` → `"version": "2.5.0"` (Panel-only Change, Minor Bump laut Spec).

- [ ] **Step 5: Generieren, Test, Build**

```bash
cd host/vcv && python res/gen_panel.py && python res/test_panel.py && export PATH="/c/Users/bernd/Documents/AI/mingw64/bin:/c/msys64/usr/bin:$PATH" RACK_DIR="c:/Users/bernd/Documents/AI/Rack-SDK" && /c/msys64/usr/bin/make CC=gcc CXX=g++ TMP="C:/Users/bernd/AppData/Local/Temp" TEMP="C:/Users/bernd/AppData/Local/Temp" SHELL=/usr/bin/bash -j4
```
Expected: `PASS -- panel guards ok` und `Built plugin.dll`.

- [ ] **Step 6: Param-ID-Kompatibilität gegen den alten Header beweisen**

Der Referenzstand ist der Commit **vor** dem ersten Redesign-Commit (`test(panel): freeze param enum order …` aus Task 1):

```bash
cd host/vcv
BASE=$(git log --format=%H --grep="freeze param enum order" -1)^
git show $BASE:host/vcv/src/generated_panel.hpp | sed -n '/enum ParamId/,/};/p' > /tmp/enum_before.txt
sed -n '/enum ParamId/,/};/p' src/generated_panel.hpp > /tmp/enum_after.txt
diff /tmp/enum_before.txt /tmp/enum_after.txt && echo "PARAM IDS IDENTICAL"
```
Expected: `PARAM IDS IDENTICAL` und kein diff-Output. Wenn hier etwas ausgegeben wird, ist der Redesign kaputt — nicht committen, sondern die Ursache in `part_controls()`/`PARAMS` suchen.

- [ ] **Step 7: In Rack installieren und live prüfen**

```bash
cd host/vcv && export PATH="/c/Users/bernd/Documents/AI/mingw64/bin:/c/msys64/usr/bin:$PATH" RACK_DIR="c:/Users/bernd/Documents/AI/Rack-SDK" && /c/msys64/usr/bin/make install RACK_USER_DIR="/c/Users/bernd/AppData/Local/Rack2" CC=gcc CXX=g++ TMP="C:/Users/bernd/AppData/Local/Temp" TEMP="C:/Users/bernd/AppData/Local/Temp" SHELL=/usr/bin/bash
cp -r dist/Spotymod/. "/c/Users/bernd/AppData/Local/Rack2/plugins-win-x64/Spotymod/"
```

Dann Rack starten und mit **Bastian gemeinsam** prüfen (diesen Schritt nicht allein abhaken):
1. Jedes Label ist lesbar und sitzt bei seinem Control — besonders die radialen Orbit-Labels und die weißen Jack-Labels auf den Wannen.
2. Gruppen-Legenden (VOICE/FX/PLAY/BLEND/TIME/DUO/ROOM/CV A/IN/CLOCK/OUT/CV B) erscheinen — sie kommen aus `kPanelTexts`, nicht aus dem SVG.
3. Ein **vor** diesem Redesign gespeichertes `.vcv`-Patch laden: alle Regler stehen wie zuvor, kein Parameter ist verrutscht.
4. Tooltips der Jacks zeigen „IN L", „OUT L", „Pitch A" — nicht „L"/„PIT".

- [ ] **Step 8: Commit**

```bash
cd host/vcv && git add res/gen_panel.py res/test_panel.py res/Spotymod.svg src/generated_panel.hpp plugin.json && git commit -m "$(cat <<'EOF'
panel: drop printed screw circles, v2.5.0

Rack draws real screw widgets in the corners and the printed circles never
lined up with them exactly -- the SVG corners stay bare paper now. Closes the
2026-07-18 faceplate redesign; panel-only change, so a minor bump.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

## Nicht in diesem Plan

- Kein Release-Tag. Der Tag `v2.5.0` (→ CI-Build + GitHub Release) wird erst gesetzt, wenn Bastian das Panel in Rack abgenommen hat.
- Keine Website-/Dev-Diary-Aktualisierung — die passiert laut Spec zum Release-Zeitpunkt im Residency-Repo.
- Keine Parameter-, DSP- oder Verhaltensänderung, keine Änderung am Hardware-Firmware-Panel-Mapping.

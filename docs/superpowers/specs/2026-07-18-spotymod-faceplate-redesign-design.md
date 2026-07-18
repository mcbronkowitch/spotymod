# Spotymod faceplate redesign — sector orbit, group boxes, sorted jacks

**Date:** 2026-07-18
**Status:** approved (brainstorm with Bastian, 2026-07-18; mockup iterated to v7
in `.superpowers/brainstorm/1070-1784363654/content/layout-b-v7.html`)

## Problem

The panel grew control by control: every new param (FILT, TIDE, FLUX rate/FB,
COLOR) was appended last for patch-id stability and placed in whatever corner
was free. The result:

- **COLOR** sits contextless between orbit and center strip — a big knob with
  no visual relationship to anything, though it is pitch material by spec.
- **STPS** lives in the FX row but is a sequencer parameter.
- **Labels touch the colored collars** — text baseline at `y + r + 2.5` leaves
  no air below the collar ring.
- **No visual grouping**: only the pad row has a backplate; voice/FX rows and
  the center strip's five sub-clusters float free.
- The ten jacks sit in one undifferentiated row; PIT/GATE labels appear twice
  with nothing saying which part they belong to, and nothing distinguishes
  inputs from outputs.

## Constraints

- **42HP stays.** No panel growth; everything is re-arranged within 213.36mm.
- **Identity "loosely inherited" from the hardware:** the LED ring + macro
  orbit and the mirrored A/B split remain recognizable; strict
  hardware-reducibility is dropped — Spotymod is its own instrument now.
- **Param ids must not change.** Ids follow enum *order* in
  `res/gen_panel.py`, not coordinates. This redesign only moves coordinates,
  labels, and panel graphics; the PARAMS list order stays byte-identical.
  Existing `.vcv` patches keep working.

## Design

### 1. Sector orbit — 9 knobs, COLOR joins PITCH

The orbit is re-sorted semantically and grows from 8 to 9 positions (40°
pitch). COLOR becomes a full orbit member — no more satellite. Part A angles
(0° = top, clockwise; part B mirrors x):

| Sector | Knobs (angle) |
|--------|---------------|
| MOTION | RATE (0°), DENS (40°), SMTH (80°) |
| TIMBRE | SHAPE (120°), MOD (160°) |
| PITCH  | RANGE (200°), MELO (240°), TUNE (280°), COLOR (320°) |

Sectors are drawn as **tinted wedge segments** behind the knobs (annulus
r 20.5..33.5mm, side accent color at opacity 0.07, angular gaps between
sectors: MOTION −16..96°, TIMBRE 112..176°, PITCH 192..336°). Sector captions
(1.7mm, bold, accent color) sit in the free panel corners: MOTION top-inner
(74.0, 8.2), TIMBRE bottom-inner (74.0, 67.6), PITCH top-outer (11.0, 8.2)
— part-A coordinates, mirrored for B.

### 2. Radial orbit labels

Orbit labels move from "always below" to **radially outside** the knob, so no
label ever falls between knob and LED ring. Placement rule (a = knob angle,
s = sin a, c = cos a):

- radius: 33.8 if c < −0.38 (bottom, clears the group boxes); 33.2 if
  |s| < 0.38 and c > 0.38 (straight up, stays on-panel); else 34.2
- anchor: start if s > 0.38, end if s < −0.38, else middle
- baseline shift: +2.2 if c < −0.38; 0 if c > 0.38; else +0.7

### 3. Collars only where they work

- **Orbit knobs keep** their accent collar (green A / copper B) — they mark
  the performance layer.
- **All small knobs lose the collar** (bare graphite on paper) — 20+ rings
  per side were visual noise.
- **MORPH keeps** its split green/copper collar (the bridge signature).
- ~~Pads keep their accent border~~; the edge bands, ring letter, and sector
  tints carry side identity everywhere else.

**Errata (2026-07-18, after the first Rack look):** the pads lost their accent
border too. Rack's button widgets are round and sit almost exactly on the
square key, so the coloured stroke survived only as a halo peeking out around
each button in the PLAY row — noise of the same kind the collar cull removed.
The pads are now a plain paper key bed.

### 4. Group boxes (fieldset style)

One shared style for every group: fill `PAPER_DEEP`, stroke `LINE` 0.35mm,
rx 1.5, and a **legend riding the top border** — a small paper chip
(`PAPER` fill) with 1.8mm bold muted text at x = box left + 5.0. Exceptions:
the jack-row CV groups use green/copper legend text.

### 5. Lower half per part — VOICE | FX side by side, PLAY below

Part-A coordinates (part B mirrors x → W−x):

- **VOICE box** x 4.0..41.0, y 72.4..96.9:
  rows y 77.3 / 89.4 at x 9.5 / 22.5 / 35.5 — ATK DEC FILT / RES SUB DTUN.
- **FX box** x 43.5..82.0, y 72.4..96.9:
  top row FRATE (49.5) FLUX (62.75) FFB (76.0) — the delay cluster;
  bottom row GRIT (56.0) COMP (69.5).
- **PLAY box** x 4.0..82.0, y 98.6..111.2, elements at y 103.6:
  mode pads ENG (11.5) GRIT (22.0) | hairline divider at x 28.7 |
  **STPS knob (35.5, moved here from the FX row — it is a sequencer param)**
  then STEP (46.0) PRIN (56.5) NEW (67.0) TRIG (77.5).

### 6. Center strip — four free-standing boxes, no outer card

The full-height background card is removed; the fieldset boxes carry the
grouping alone and grow to 41mm width (CX ± 20.5). Columns sit at CX ± 11.5.

| Box | y extent | contents |
|-----|----------|----------|
| BLEND | 13.0..32.5 | MORPH (big, split collar, CX−7.0, y 21.5) · TIDE (CX+11.0, y 21.5) |
| TIME  | 35.0..48.5 | SYNC switch (L) · TEMPO (CX) · COUPL (R), y 41.0 |
| DUO   | 51.0..73.5 | SCALE CHOKE DRIFT (y 56.5) / SPOT · DRIVE · SETL (y 66.0) |
| ROOM  | 76.5..111.2 | SIZE MIX DECAY (y 82.5) / TONE · DIFF (y 93.0) / SMEAR · WOBL (y 103.5) |

ROOM's bottom edge (111.2) is flush with the PLAY boxes. Group names
BLEND/TIME/DUO/ROOM are the approved working titles; SCALE and DRIVE stay in
DUO (accepted during review).

### 7. Jack row — five labeled groups, outputs on dark wells

The ten jacks split into five fieldset groups with real gaps, signal flow
reading left → right. Group boxes: width 23.0, height 14.4, top y 112.6
(legend chip clears the PLAY boxes above); jacks at y 118.4, 11.5mm pitch
inside a group; jack labels at y 124.8, 1.8mm.

| Group | box left x | jacks | legend color |
|-------|-----------|-------|--------------|
| CV A  | 7.2   | PIT, GATE | green |
| IN    | 49.5  | L, R      | muted |
| CLOCK | CX − 11.5 | CLK, RST | muted |
| OUT   | W − 72.5 | L, R    | muted |
| CV B  | W − 30.2 | GATE, PIT | copper |

**Output groups (CV A, OUT, CV B) get a dark inner well** (`WELL` fill,
inset 1.4mm horizontally / 1.6mm top, rx 1.2) with **white** jack labels;
input groups stay on paper with ink labels. In/out reads at a glance and the
duplicated PIT/GATE labels are disambiguated by the group legends.

### 8. Label spacing

- small knobs: baseline `y + 5.6` (1.9mm font) — close but clear of the bare
  knob edge
- big knobs with below-label (MORPH only): `y + 7.2`
- pads: `y + 5.4`; switch: `y + 6.6`; jacks: `y + 6.4` (white inside wells)

## Panel furniture

The printed screw-hole circles in the corners are **removed**: they never
lined up exactly, and Rack draws real screw widgets there anyway. The SVG
corners stay bare paper.

## Implementation notes

- Everything lives in `res/gen_panel.py`; the SVG and
  `src/generated_panel.hpp` are both regenerated. **PARAMS enum order is
  untouched** — only `Ctl` coordinates, the SVG drawing code, and the text
  tables change.
- The runtime side (`Spotymod.cpp`) draws labels from the generated header
  with an implicit "below the control" rule. The radial orbit labels and
  white-on-well jack labels need the header's control/text tables to carry an
  explicit label anchor/offset (and color for jack labels) per control —
  extend `PanelCtl`/`PanelTxt` accordingly rather than hardcoding in C++.
- Wedges, group boxes, legends, and sector captions are static panel
  graphics → SVG only.
- The mockup generator (scratch, `gen_mockups_v2.py`) matches this spec
  numerically and can serve as the coordinate reference during
  implementation.
- Version: panel-only change, minor bump (v2.5.0) when released.

## Out of scope

- No parameter, DSP, or behavior changes; no renames of param enums.
- No changes to the hardware firmware panel mapping.
- Website/dev-diary update happens at release time, not in this change.

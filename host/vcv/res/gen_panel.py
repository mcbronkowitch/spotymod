#!/usr/bin/env python3
"""Single source of truth for the Spotymod VCV panel.

Layout mirrors the real Spotykach hardware: two symmetric halves, each built
around a 32-LED ring with its macro knobs encircling it and its secondary
functions as a row of small "pads" (buttons) below; a narrow shared center
strip (interaction + room + master + clock) sits between them.

Visual identity comes from the residency devlog ("workbench paper"): a warm
paper plate with ink lettering and one accent per side -- solder green for
part A (left), copper orange for part B (right). The shared center stays
neutral; MORPH, the knob that bridges the two parts, wears a split
green/copper collar.

Emits (both committed):
  - res/Spotymod.svg          the faceplate
  - src/generated_panel.hpp   enums + PART_STRIDE + control/text tables

Run from host/vcv/:  python3 res/gen_panel.py
The C++ never hardcodes a coordinate, label or colour; it reads them from the
generated header, so panel graphics and widget placement can never drift apart.
"""
import os, math

HP = 42
MM_PER_HP = 5.08
W  = HP * MM_PER_HP          # 213.36 mm
Hh = 128.5                   # standard Eurorack height

# --- devlog palette (website/styles.css) ---------------------------------------
PAPER      = "#f7f4ec"   # plate
PAPER_HI   = "#faf8f2"   # plate gradient top
PAPER_LO   = "#f0ebdd"   # plate gradient bottom
PAPER_DEEP = "#ede5d6"   # cards / pad backplates
LINE       = "#d7cdbb"   # hairlines
INK        = "#171713"   # lettering
MUTED      = "#656056"   # eyebrows / neutral collars
GRAPHITE   = "#252721"   # knob caps, jack wells
WELL       = "#1d1f1a"   # LED-ring well (dark, so the glow reads)
WHITE      = "#fffdf7"   # pad keys / knob ticks
GREEN      = "#1d6f5f"   # part A accent (solder green)
COPPER     = "#b96532"   # part B accent (copper orange)
GREEN_DIM  = "#2e6355"   # A ring track dots / letter (on the dark well)
COPPER_DIM = "#8a5230"   # B ring track dots / letter
GLOW       = (0x3FBF9C, 0xE8945A)   # runtime LED glow per part (A, B)

# --- control kinds ------------------------------------------------------------
BIGKNOB = "BIGKNOB"   # macro pot (0..1)
KNOBC   = "KNOBC"     # bipolar macro (-1..1)  (MELODY)
SMKNOB  = "SMKNOB"    # small secondary pot (0..1)
KNOBI   = "KNOBI"     # small integer pot (snap)
SW2     = "SW2"       # 2-pos switch (global SYNC)
LATCH   = "LATCH"     # on/off pad button (binary)
SMBTN   = "SMBTN"     # momentary pad button
IN      = "IN"
OUT     = "OUT"
LIGHT   = "LIGHT"

GLYPH_R = {BIGKNOB:4.2, KNOBC:4.2, SMKNOB:3.0, KNOBI:3.0, SW2:3.0,
           LATCH:2.7, SMBTN:2.7, IN:4.2, OUT:4.2, LIGHT:1.7}
WKMAP = {BIGKNOB:"WK_BIGKNOB", KNOBC:"WK_KNOBC", SMKNOB:"WK_SMKNOB",
         KNOBI:"WK_KNOBI", SW2:"WK_SW2", LATCH:"WK_LATCH", SMBTN:"WK_SMBTN",
         IN:"WK_IN", OUT:"WK_OUT", LIGHT:"WK_LIGHT"}

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

class Ctl:
    def __init__(self, enum, kind, x, y, label):
        self.enum, self.kind, self.x, self.y, self.label = enum, kind, x, y, label
        self.r = GLYPH_R[kind]
        # None -> default placement (centred below the glyph); otherwise an
        # explicit (x, y, anchor, size, colour) tuple. Radial orbit captions
        # and white-on-well jack labels set this.
        self.lbl = None
        self.tip = label   # tooltip text; panel label and tooltip differ on jacks

# geometry of a side ring
RING_CY   = 37.0
RING_R    = 16.0       # LED dot radius
KNOB_R    = 26.5       # macro-knob orbit radius
RING_CX_A = 42.0       # left ring center; B is mirrored (W - x)

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

# --- fieldset groups (spec 2026-07-18 §4) ------------------------------------
# One shared style: paper-deep panel, hairline stroke, and a legend riding the
# top border on a small paper chip. (x, y, w, h, legend, legend colour)
def part_groups(mir):
    def fx(x, w): return (W - x - w) if mir else x
    return [(fx(4.0, 37.0),  72.4, 37.0, 24.5, "VOICE", MUTED),
            (fx(43.5, 38.5), 72.4, 38.5, 24.5, "FX",    MUTED),
            (fx(4.0, 78.0),  98.6, 78.0, 12.6, "PLAY",  MUTED)]

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

# --- lower half per part (spec 2026-07-18 §5) --------------------------------
# VOICE and FX sit side by side, PLAY spans the full part width below them.
VOICE_X  = [9.5, 22.5, 35.5]        # ATK DEC FILT / RES SUB DTUN
ROW_V1, ROW_V2 = 77.3, 89.4
FX_TOP   = [49.5, 62.75, 76.0]      # FRATE FLUX FFB -- the delay cluster
FX_BOT   = [56.0, 69.5]             # GRIT COMP
PLAY_Y   = 103.6
PAD_X    = [11.5, 22.0, 46.0, 56.5, 67.0, 77.5]   # ENG GRIT | STEP PRIN NEW TRIG
STEPS_X  = 35.5                     # sequencer knob, between the two pad blocks

# --- per-part control template (ORDER defines enum order; identical A/B) ------
# Returns Ctl list with per-part coordinates (mirrored for B when mir=True).
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

# --- shared center strip ------------------------------------------------------
CX = W / 2.0

# --- inputs / outputs / lights ------------------------------------------------
# The ten jacks split into five labelled fieldset groups with real gaps, signal
# flow reading left -> right (spec 2026-07-18 §7). Output groups sit on a dark
# well with white lettering, inputs stay on paper with ink -- so in/out reads at
# a glance and the duplicated PIT/GATE labels are disambiguated by the legend.
# NOTE: this block sits here (above GROUPS, not down by LIGHTS) because
# JACK_GROUPS feeds GROUPS below and must be defined before it runs; it only
# needs CX/W, the colour constants and the JACK_* constants, all already in scope.
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

# The centre's outer background card is gone (spec 2026-07-18 §6) -- the four
# fieldset boxes carry the grouping alone and grew to 41 mm, so the columns
# move out from +-10.5 to +-11.5.
L, R = CX - 11.5, CX + 11.5
ROW_BLEND = 21.5
ROW_TIME  = 41.0
ROW_DUO1, ROW_DUO2 = 56.5, 66.0
ROW_ROOM1, ROW_ROOM2, ROW_ROOM3 = 82.5, 93.0, 103.5
# The four free-standing centre boxes (spec 2026-07-18 §6); GROUPS is assigned
# here, not alongside part_groups() above, because these entries need CX.
GROUPS = part_groups(False) + part_groups(True) + [
    (CX - 20.5, 13.0, 41.0, 19.5, "BLEND", MUTED),
    (CX - 20.5, 35.0, 41.0, 13.5, "TIME",  MUTED),
    (CX - 20.5, 51.0, 41.0, 22.5, "DUO",   MUTED),
    (CX - 20.5, 76.5, 41.0, 34.7, "ROOM",  MUTED),
] + [(bx, JACK_BOX_Y, JACK_BOX_W, JACK_BOX_H, lg, col)
     for (bx, lg, col, _well, _items) in JACK_GROUPS]
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

def color_ctl(suffix, mir):
    cx = W - RING_CX_A if mir else RING_CX_A
    ang = ORBIT_ANG["COLOR"]
    x, y = orbit(cx, RING_CY, KNOB_R, ang, mir)
    c = Ctl("COLOR" + suffix, BIGKNOB, x, y, "COLOR")
    c.lbl = orbit_label(cx, RING_CY, ang, mir)
    return c

PARAMS = PART_A + PART_B + SHARED + [
    # FILT: bipolar cutoff trim (spec 2026-07-17). Appended LAST like CHOKE so
    # existing .vcv patches keep their param ids; coordinates put it in the
    # top voice row, third slot (after ATK, DEC).
    Ctl("FILT_A", SMKNOB, VOICE_X[2],     ROW_V1, "FILT"),
    Ctl("FILT_B", SMKNOB, W - VOICE_X[2], ROW_V1, "FILT"),
    # TIDE: texture-lane rate of both decks (spec 2026-07-17 mod-tide).
    # Appended LAST like CHOKE/FILT so existing .vcv patches keep their ids;
    # the coordinate puts it beside MORPH in the centre's movement column
    # (COUPLE/DRIFT/SETL).
    Ctl("TIDE", SMKNOB, CX + 11.0, ROW_BLEND, "TIDE"),
    # FLUX synced-delay controls (spec 2026-07-17 flux-synced-delay). Per part,
    # appended LAST like FILT/TIDE/CHOKE so existing .vcv patches keep their ids.
    # They complete the FLUX delay cluster atop the FX box: RATE (FX_TOP[0]),
    # MIX (FX_TOP[1], from the template), FB (FX_TOP[2]) sit together;
    # GRIT/COMP fill FX_BOT below.
    Ctl("FLUXRATE_A", SMKNOB, FX_TOP[0],     ROW_V1, "FRATE"),
    Ctl("FLUXRATE_B", SMKNOB, W - FX_TOP[0], ROW_V1, "FRATE"),
    Ctl("FLUXFB_A",   SMKNOB, FX_TOP[2],     ROW_V1, "FFB"),
    Ctl("FLUXFB_B",   SMKNOB, W - FX_TOP[2], ROW_V1, "FFB"),
    # COLOR: chord density/colour per part (spec 2026-07-17 chord-layer), a full
    # orbit member since the 2026-07-18 redesign -- it is pitch material, so it
    # sits in the PITCH sector. Still appended LAST: order defines the param id.
    color_ctl("_A", False),
    color_ctl("_B", True),
]

# --- lights --------------------------------------------------------------------
# INPUTS/OUTPUTS are built above (see JACK_GROUPS, near CX) -- they had to move
# ahead of GROUPS, which folds the jack boxes in. Only the lights are left here.
LIGHTS = [
    # glow at each ring center, driven by that part's gate
    Ctl("GATE_A_L", LIGHT, RING_CX_A,     RING_CY, ""),
    Ctl("GATE_B_L", LIGHT, W - RING_CX_A, RING_CY, ""),
]

# --- shared panel lettering (drawn by SVG for preview, by C++ at runtime) -----
# (x, y baseline, size mm, letter-spacing mm, hex colour, text)
TEXTS = [
    (RING_CX_A,     RING_CY + 1.6, 5.0, 0.0, GREEN_DIM,  "A"),
    (W - RING_CX_A, RING_CY + 1.6, 5.0, 0.0, COPPER_DIM, "B"),
    (CX,            7.0,           3.6, 0.9, INK,        "SPOTYMOD"),  # top brand
] + [
    # sector captions, tucked into the free panel corners (spec §1)
    (W - cx if mir else cx, cy, 1.7, 0.3, COPPER if mir else GREEN, name)
    for mir in (False, True)
    for (name, _a0, _a1, (cx, cy)) in SECTORS
] + legend_texts()

# =============================================================================
#  SVG
# =============================================================================
def mm(v): return f"{v:.3f}"

def side_accent(x):
    """Panel accent for a control: green left half, copper right half,
    neutral muted inside the center strip."""
    if x < CX - 21.0: return GREEN
    if x > CX + 21.0: return COPPER
    return MUTED

def ring_svg(cx, dot):
    """One LED ring: dark well + 32 dim track dots (the 'off' bed). The live
    SpkyRing widget draws the accent glow on top of these at runtime."""
    P = [f'<circle cx="{mm(cx)}" cy="{mm(RING_CY)}" r="{mm(RING_R+2.4)}" '
         f'fill="{WELL}" stroke="{GRAPHITE}" stroke-width="0.5"/>']
    for i in range(32):
        a = math.radians(360.0 * i / 32.0)
        x = cx + RING_R * math.sin(a)
        y = RING_CY - RING_R * math.cos(a)
        P.append(f'<circle cx="{mm(x)}" cy="{mm(y)}" r="0.7" fill="{dot}"/>')
    return "\n".join(P)

def knob_svg(c):
    """Graphite cap + accent collar + paper tick. The collar sits OUTSIDE the
    runtime widget's footprint (RoundBlackKnob r4.2, Trimpot ~r3.3), so the
    side colour stays visible in Rack, not just in this preview."""
    P = []
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
    P.append(f'<circle cx="{mm(c.x)}" cy="{mm(c.y)}" r="{mm(c.r)}" '
             f'fill="url(#knobCap)" stroke="{GRAPHITE}" stroke-width="0.3"/>')
    P.append(f'<line x1="{mm(c.x)}" y1="{mm(c.y)}" x2="{mm(c.x)}" '
             f'y2="{mm(c.y-c.r+0.7)}" stroke="{WHITE}" stroke-width="0.5" '
             f'stroke-linecap="round"/>')
    return "\n".join(P)

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

def svg():
    P = []
    P.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{mm(W)}mm" '
             f'height="{mm(Hh)}mm" viewBox="0 0 {mm(W)} {mm(Hh)}">')
    P.append(
        '<defs>'
        '<radialGradient id="knobCap" cx="38%" cy="32%" r="75%">'
        f'<stop offset="0%" stop-color="#3a3d35"/>'
        f'<stop offset="55%" stop-color="{GRAPHITE}"/>'
        f'<stop offset="100%" stop-color="#15160f"/></radialGradient>'
        '<linearGradient id="plate" x1="0" y1="0" x2="0" y2="1">'
        f'<stop offset="0%" stop-color="{PAPER_HI}"/>'
        f'<stop offset="100%" stop-color="{PAPER_LO}"/></linearGradient>'
        '</defs>')
    P.append(f'<rect x="0" y="0" width="{mm(W)}" height="{mm(Hh)}" fill="url(#plate)"/>')
    # the two-colour identity: a solder-green band on A's edge, copper on B's
    P.append(f'<rect x="0" y="0" width="1.4" height="{mm(Hh)}" fill="{GREEN}"/>')
    P.append(f'<rect x="{mm(W-1.4)}" y="0" width="1.4" height="{mm(Hh)}" fill="{COPPER}"/>')
    # mounting holes
    for hx in (MM_PER_HP, W - MM_PER_HP):
        for hy in (3.0, Hh-3.0):
            P.append(f'<circle cx="{mm(hx)}" cy="{mm(hy)}" r="1.6" '
                     f'fill="#d8d0bf" stroke="{LINE}" stroke-width="0.3"/>')
    # sector tints behind the orbit (drawn first: everything else sits on top)
    for mir, cx, accent in ((False, RING_CX_A, GREEN), (True, W - RING_CX_A, COPPER)):
        for (name, a0, a1, _cap) in SECTORS:
            P.append(wedge_svg(cx, a0, a1, accent, mir))
    # two rings (dark well + dim track; the live SpkyRing widget lights them)
    P.append(ring_svg(RING_CX_A, GREEN_DIM))
    P.append(ring_svg(W - RING_CX_A, COPPER_DIM))
    # fieldset group boxes (drawn under the glyphs, over the sector tints)
    for (x, y, w, h, name, _colour) in GROUPS:
        P.append(group_box(x, y, w, h, name))
    # dark inner wells under the output groups -- in/out at a glance (spec §7)
    for (bx, _lg, _col, well, _items) in JACK_GROUPS:
        if well:
            P.append(f'<rect x="{mm(bx + 1.4)}" y="{mm(JACK_BOX_Y + 1.6)}" '
                     f'width="{mm(JACK_BOX_W - 2.8)}" '
                     f'height="{mm(JACK_BOX_H - 3.2)}" rx="1.2" fill="{WELL}"/>')
    # PLAY: hairline between the two mode pads and the sequencer block
    for dx in (28.7, W - 28.7):
        P.append(f'<line x1="{mm(dx)}" y1="100.6" x2="{mm(dx)}" y2="109.2" '
                 f'stroke="{LINE}" stroke-width="0.35"/>')
    # brand flanking dots -- one per colour, flanking the top SPOTYMOD logo
    P.append(f'<circle cx="{mm(CX-15)}" cy="5.9" r="0.9" fill="{GREEN}"/>')
    P.append(f'<circle cx="{mm(CX+15)}" cy="5.9" r="0.9" fill="{COPPER}"/>')
    # glyphs + labels
    for c in PARAMS + INPUTS + OUTPUTS + LIGHTS:
        if c.kind in (IN, OUT):
            P.append(f'<circle cx="{mm(c.x)}" cy="{mm(c.y)}" r="{mm(c.r)}" '
                     f'fill="{GRAPHITE}" stroke="#4a4a40" stroke-width="0.4"/>')
            P.append(f'<circle cx="{mm(c.x)}" cy="{mm(c.y)}" r="1.3" fill="#0e0e0c"/>')
        elif c.kind == LIGHT:  # dark LED housing; live YellowLight glows amber on top
            P.append(f'<circle cx="{mm(c.x)}" cy="{mm(c.y)}" r="{mm(c.r)}" '
                     f'fill="#1a1206" stroke="#3a2c12" stroke-width="0.25"/>')
        elif c.kind == SW2:
            P.append(f'<rect x="{mm(c.x-1.7)}" y="{mm(c.y-3.0)}" width="3.4" '
                     f'height="6.0" rx="0.8" fill="{WHITE}" stroke="{INK}" stroke-width="0.35"/>')
            P.append(f'<rect x="{mm(c.x-1.1)}" y="{mm(c.y-2.4)}" width="2.2" '
                     f'height="2.4" rx="0.5" fill="{GRAPHITE}"/>')
        elif c.kind in (LATCH, SMBTN):   # pads: raised paper keys, side-coloured edge
            P.append(f'<rect x="{mm(c.x-c.r)}" y="{mm(c.y-c.r)}" width="{mm(2*c.r)}" '
                     f'height="{mm(2*c.r)}" rx="1.0" fill="{WHITE}" '
                     f'stroke="{side_accent(c.x)}" stroke-width="0.35"/>')
        else:
            P.append(knob_svg(c))
        if c.label:
            lx, ly, anchor, size, colour = label_of(c)
            P.append(f'<text x="{mm(lx)}" y="{mm(ly)}" fill="{colour}" '
                     f'text-anchor="{anchor}" font-family="monospace" '
                     f'font-size="{size}">{c.label}</text>')
    # shared lettering (preview only -- Rack draws these via PanelText)
    for (x, y, size, spacing, col, txt) in TEXTS:
        P.append(f'<text x="{mm(x)}" y="{mm(y)}" fill="{col}" text-anchor="middle" '
                 f'font-family="monospace" font-size="{size}" '
                 f'letter-spacing="{spacing}" font-weight="bold">{txt}</text>')
    P.append('</svg>')
    return "\n".join(P)

# =============================================================================
#  C++ header
# =============================================================================
def rgb(hexcol): return "0x" + hexcol.lstrip("#").upper()

def header():
    L2 = []
    L2.append("// GENERATED by res/gen_panel.py -- do not edit by hand.")
    L2.append("#pragma once")
    L2.append("namespace spkyvcv {")
    L2.append("struct XY { float x, y; };")
    L2.append("enum WidgetKind { WK_BIGKNOB, WK_KNOBC, WK_SMKNOB, WK_KNOBI, "
              "WK_SW2, WK_LATCH, WK_SMBTN, WK_IN, WK_OUT, WK_LIGHT };")
    L2.append("struct PanelCtl { int id; WidgetKind kind; XY mm; const char* label; "
              "XY lbl; unsigned char anchor; float lblSize; unsigned lblRgb; "
              "const char* tip; };")
    L2.append("// anchor: 0 = middle, 1 = start (left-aligned), 2 = end (right-aligned)")
    L2.append("struct PanelTxt { XY mm; float size; float spacing; unsigned rgb; const char* str; };")
    L2.append(f"static constexpr int PART_STRIDE = {PART_STRIDE};")
    L2.append(f"static constexpr float kRingR = {RING_R:.3f}f;      // mm, LED-dot orbit")
    L2.append(f"static constexpr float kRingDotR = 0.95f;   // mm, lit-dot radius")
    L2.append("static constexpr int kRingDots = 32;")
    L2.append(f"static constexpr unsigned kColLabel = {rgb(INK)};   // ink captions")
    L2.append(f"static constexpr unsigned kColGlow[2] = {{{hex(GLOW[0])}, {hex(GLOW[1])}}}; "
              "// per-part LED glow (A green, B copper)")

    def emit_enum(name, items, terminator):
        L2.append(f"enum {name} {{")
        for c in items:
            L2.append(f"    {c.enum},")
        L2.append(f"    {terminator}")
        L2.append("};")

    emit_enum("ParamId",  PARAMS,  "NUM_PARAMS")
    emit_enum("InputId",  INPUTS,  "NUM_INPUTS")
    emit_enum("OutputId", OUTPUTS, "NUM_OUTPUTS")
    emit_enum("LightId",  LIGHTS,  "NUM_LIGHTS")

    ANCHOR_ID = {"middle": 0, "start": 1, "end": 2}

    def emit_table(name, items):
        L2.append(f"static const PanelCtl {name}[] = {{")
        for c in items:
            lx, ly, anchor, size, colour = label_of(c)
            L2.append(f'    {{{c.enum}, {WKMAP[c.kind]}, {{{c.x:.3f}f, {c.y:.3f}f}}, '
                      f'"{c.label}", {{{lx:.3f}f, {ly:.3f}f}}, {ANCHOR_ID[anchor]}, '
                      f'{size:.2f}f, {rgb(colour)}, "{c.tip}"}},')
        L2.append("};")

    emit_table("kParamCtls",  PARAMS)
    emit_table("kInputCtls",  INPUTS)
    emit_table("kOutputCtls", OUTPUTS)
    emit_table("kLightCtls",  LIGHTS)

    L2.append("static const PanelTxt kPanelTexts[] = {")
    for (x, y, size, spacing, col, txt) in TEXTS:
        L2.append(f'    {{{{{x:.3f}f, {y:.3f}f}}, {size:.2f}f, {spacing:.2f}f, {rgb(col)}, "{txt}"}},')
    L2.append("};")
    L2.append("} // namespace spkyvcv")
    return "\n".join(L2) + "\n"

if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    with open(os.path.join(here, "Spotymod.svg"), "w") as f:
        f.write(svg())
    with open(os.path.join(root, "src", "generated_panel.hpp"), "w") as f:
        f.write(header())
    print("wrote res/Spotymod.svg and src/generated_panel.hpp")
    print(f"params={len(PARAMS)} (stride={PART_STRIDE}) inputs={len(INPUTS)} "
          f"outputs={len(OUTPUTS)} lights={len(LIGHTS)}  panel={HP}HP")

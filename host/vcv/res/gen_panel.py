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

class Ctl:
    def __init__(self, enum, kind, x, y, label):
        self.enum, self.kind, self.x, self.y, self.label = enum, kind, x, y, label

# geometry of a side ring
RING_CY   = 37.0
RING_R    = 16.0       # LED dot radius
KNOB_R    = 26.5       # macro-knob orbit radius
RING_CX_A = 42.0       # left ring center; B is mirrored (W - x)

def orbit(cx, cy, r, i, n, start_deg=0.0):
    a = math.radians(start_deg + 360.0 * i / n)
    return (cx + r * math.sin(a), cy - r * math.cos(a))

# voice row x slots (6 @ 13 mm pitch, centred on the ring axis x = 42);
# slot 2 = FILT, appended at the END of PARAMS (never in the template --
# that grows PART_STRIDE and shifts every part-B/SHARED param id).
VOICE_X = [9.5, 22.5, 35.5, 48.5, 61.5, 74.5]

# --- per-part control template (ORDER defines enum order; identical A/B) ------
# Returns Ctl list with LEFT-side coordinates. Mirror for B by x -> W-x.
def part_controls():
    cx = RING_CX_A
    out = []
    # 8 macro knobs encircling the ring
    macros = [("RATE","RATE"),("SHAPE","SHAPE"),("DENSITY","DENS"),("SMOOTH","SMTH"),
              ("RANGE","RANGE"),("MELODY","MELO"),("MOD","MOD"),("TUNE","TUNE")]
    for i,(enum,lbl) in enumerate(macros):
        x,y = orbit(cx, RING_CY, KNOB_R, i, 8, start_deg=0.0)
        out.append(Ctl(enum, KNOBC if enum=="MELODY" else BIGKNOB, x, y, lbl))
    # voice + fx rows (small), centred on the ring axis. Vertically the two
    # rows sit with equal 3.6 mm gaps in the band between the orbit's bottom
    # label (RANGE baseline, y 70.2) and the pad backplate top (y 98.1).
    for i,(enum,lbl) in zip([0,1,3,4,5],
                            [("ATTACK","ATK"),("DECAY","DEC"),("RES","RES"),
                             ("SUB","SUB"),("DETUNE","DTUN")]):
        out.append(Ctl(enum, SMKNOB, VOICE_X[i], 76.8, lbl))
    # fx row: the FLUX delay cluster (RATE . MIX . FB) sits together on the
    # left. RATE (x 9.5) and FB (x 35.5) are appended in PARAMS for patch-id
    # stability; MIX stays at 22.5, GRIT/COMP/STEPS fill 48.5/61.5/74.5.
    # The append ORDER (FLUX, GRIT, COMP, STEPS) is unchanged, so PART_STRIDE
    # and every param id stay put -- only the x coordinates move.
    out.append(Ctl("FLUX", SMKNOB, 22.5, 88.9, "FLUX"))   # delay MIX
    for i,(enum,lbl) in enumerate([("GRIT","GRIT"),("COMP","COMP")]):
        out.append(Ctl(enum, SMKNOB, 48.5 + i*13.0, 88.9, lbl))
    out.append(Ctl("STEPS", KNOBI, 74.5, 88.9, "STPS"))
    pads = [("ENGINE",LATCH,"ENG"),("GRITMODE",LATCH,"GRIT"),
            ("STEP",LATCH,"STEP"),("PRINCIPLE",SMBTN,"PRIN"),
            ("NEWPHRASE",SMBTN,"NEW"),("TRIGGER",SMBTN,"TRIG")]
    for i,(enum,kind,lbl) in enumerate(pads):
        out.append(Ctl(enum, kind, 15.75 + i*10.5, 102.8, lbl))
    return out

def mirror(ctls):
    return [Ctl(c.enum, c.kind, W - c.x, c.y, c.label) for c in ctls]

PART_A = [Ctl(c.enum + "_A", c.kind, c.x, c.y, c.label) for c in part_controls()]
PART_B = [Ctl(c.enum + "_B", c.kind, c.x, c.y, c.label) for c in mirror(part_controls())]
PART_STRIDE = len(PART_A)

# --- shared center strip ------------------------------------------------------
CX = W / 2.0
L, R = CX - 10.5, CX + 10.5
# The center box now runs the full height (bottom-aligned with the A/B pad
# boxes at y 110). Its lower rows line up horizontally with the part rows:
ROW_VOICE = 76.8    # ATK/DEC/RES/SUB/DTUN
ROW_FX    = 88.9    # FLUX/GRIT/COMP/STPS
ROW_PAD   = 102.8   # ENG/.../TRIG pads
SHARED = [
    Ctl("MORPH",  BIGKNOB, CX,  22.0, "MORPH"),
    # TIME: the one clock story — the mode switch, its tempo, and how tightly
    # the two parts hang together (spec 2026-07-16 sync/couple redesign)
    Ctl("SYNC",   SW2,     L,   38.0, "SYNC"),
    Ctl("TEMPO",  SMKNOB,  CX,  38.0, "TEMPO"),
    Ctl("COUPLE", SMKNOB,  R,   38.0, "COUPL"),
    Ctl("SCALE",  KNOBI,   L,   51.0, "SCALE"),
    Ctl("DRIFT",  SMKNOB,  R,   51.0, "DRIFT"),
    Ctl("SPOT",   SMBTN,   L,   62.0, "SPOT"),
    Ctl("MASTER_DRIVE", SMKNOB, CX, 62.0, "DRIVE"),
    Ctl("SETTLE", SMBTN,   R,   62.0, "SETL"),
    # ROOM: unchanged rows; TEMPO has moved out, SMEAR/MOD keep flanking.
    Ctl("REV_SIZE",  SMKNOB, L,  ROW_VOICE, "SIZE"),
    Ctl("REV_DECAY", SMKNOB, R,  ROW_VOICE, "DECAY"),
    Ctl("REV_MIX",   SMKNOB, CX, (ROW_VOICE + ROW_FX) / 2.0, "MIX"),
    Ctl("REV_TONE",  SMKNOB, L,  ROW_FX,    "TONE"),
    Ctl("REV_DIFF",  SMKNOB, R,  ROW_FX,    "DIFF"),
    Ctl("REV_SMEAR", SMKNOB, L,  ROW_PAD, "SMEAR"),
    Ctl("REV_MOD",   SMKNOB, R,  ROW_PAD, "WOBL"),
    # CHOKE: bipolar event-priority between the decks (spec 2026-07-16
    # choke-priority). Fills the free centre slot between SCALE and DRIFT.
    # Appended LAST on purpose: existing .vcv patches keep their param ids.
    Ctl("CHOKE",  SMKNOB, CX,  51.0, "CHOKE"),
]

PARAMS = PART_A + PART_B + SHARED + [
    # FILT: bipolar cutoff trim (spec 2026-07-17). Appended LAST like CHOKE so
    # existing .vcv patches keep their param ids; coordinates put it in the
    # voice row (slot 2, between DEC and RES).
    Ctl("FILT_A", SMKNOB, VOICE_X[2],     76.8, "FILT"),
    Ctl("FILT_B", SMKNOB, W - VOICE_X[2], 76.8, "FILT"),
    # TIDE: texture-lane rate of both decks (spec 2026-07-17 mod-tide).
    # Appended LAST like CHOKE/FILT so existing .vcv patches keep their ids;
    # the coordinate puts it beside MORPH in the centre's movement column
    # (COUPLE/DRIFT/SETL).
    Ctl("TIDE", SMKNOB, R, 22.0, "TIDE"),
    # FLUX synced-delay controls (spec 2026-07-17 flux-synced-delay). Per part,
    # appended LAST like FILT/TIDE/CHOKE so existing .vcv patches keep their ids.
    # They complete the FLUX delay cluster on the left of the FX row: RATE (9.5),
    # MIX (22.5, from the template), FB (35.5) sit together; GRIT/COMP/STEPS
    # follow at 48.5/61.5/74.5.
    Ctl("FLUXRATE_A", SMKNOB, 9.5,       88.9, "FRATE"),
    Ctl("FLUXRATE_B", SMKNOB, W - 9.5,   88.9, "FRATE"),
    Ctl("FLUXFB_A",   SMKNOB, 35.5,      88.9, "FFB"),
    Ctl("FLUXFB_B",   SMKNOB, W - 35.5,  88.9, "FFB"),
]

# --- inputs / outputs / lights ------------------------------------------------
# All ten jacks live on ONE line along the very bottom, outside the center box,
# spanning the full width. Mirror-symmetric about center: the two part CV pairs
# (PIT/GATE) bookend the row, the shared audio + clock I/O sits in the middle.
#   PIT_A GATE_A | IN_L IN_R CLK RST OUT_L OUT_R | GATE_B PIT_B
JACK_Y   = 118.0
JACK_MRG = 16.0                                   # x of the outermost jacks
JS = [JACK_MRG + i * (W - 2 * JACK_MRG) / 9.0 for i in range(10)]  # 10 even slots
INPUTS = [
    Ctl("IN_L",  IN, JS[2], JACK_Y, "IN L"),
    Ctl("IN_R",  IN, JS[3], JACK_Y, "IN R"),
    Ctl("CLOCK", IN, JS[4], JACK_Y, "CLK"),
    Ctl("RESET", IN, JS[5], JACK_Y, "RST"),
]
OUTPUTS = [
    Ctl("OUT_L", OUT, JS[6], JACK_Y, "OUT L"),
    Ctl("OUT_R", OUT, JS[7], JACK_Y, "OUT R"),
    # per-part modulation taps bookending the row (A outer-left, B outer-right)
    Ctl("PITCH_A", OUT, JS[0], JACK_Y, "PIT"),
    Ctl("GATE_A",  OUT, JS[1], JACK_Y, "GATE"),
    Ctl("PITCH_B", OUT, JS[9], JACK_Y, "PIT"),
    Ctl("GATE_B",  OUT, JS[8], JACK_Y, "GATE"),
]
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
    (CX,            32.2,          2.2, 0.5, MUTED,      "TIME"),
    (CX,            70.0,          2.2, 0.5, MUTED,      "ROOM"),
    (CX,            7.0,           3.6, 0.9, INK,        "SPOTYMOD"),  # top brand
]

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
    collar_r = c.r + (0.85 if big else 1.0)
    collar_w = 0.5 if big else 0.45
    if c.enum == "MORPH":  # signature: the bridge knob wears both colours
        for (col, sweep) in ((GREEN, 0), (COPPER, 1)):
            P.append(f'<path d="M {mm(c.x)} {mm(c.y-collar_r)} '
                     f'A {mm(collar_r)} {mm(collar_r)} 0 0 {sweep} '
                     f'{mm(c.x)} {mm(c.y+collar_r)}" fill="none" '
                     f'stroke="{col}" stroke-width="{collar_w}"/>')
    else:
        P.append(f'<circle cx="{mm(c.x)}" cy="{mm(c.y)}" r="{mm(collar_r)}" '
                 f'fill="none" stroke="{accent}" stroke-width="{collar_w}"/>')
    P.append(f'<circle cx="{mm(c.x)}" cy="{mm(c.y)}" r="{mm(c.r)}" '
             f'fill="url(#knobCap)" stroke="{GRAPHITE}" stroke-width="0.3"/>')
    P.append(f'<line x1="{mm(c.x)}" y1="{mm(c.y)}" x2="{mm(c.x)}" '
             f'y2="{mm(c.y-c.r+0.7)}" stroke="{WHITE}" stroke-width="0.5" '
             f'stroke-linecap="round"/>')
    return "\n".join(P)

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
    # center strip card (neutral zone between the two coloured halves). Runs
    # the full height, bottom edge (y 110) level with the A/B pad boxes; the
    # jack strip lives below it on the bare bottom edge.
    P.append(f'<rect x="{mm(CX-21)}" y="10.0" width="42.0" height="100.0" rx="2" '
             f'fill="{PAPER_DEEP}" stroke="{LINE}" stroke-width="0.3"/>')
    # two rings (dark well + dim track; the live SpkyRing widget lights them)
    P.append(ring_svg(RING_CX_A, GREEN_DIM))
    P.append(ring_svg(W - RING_CX_A, COPPER_DIM))
    # pad-row backplates, centred on the ring axis; bottom edge flush with the
    # center card (y=110). Uniform 2.0 mm padding: pads span x 7.8..76.2,
    # y 100.1..105.5; label baselines sit at y 108.0 (caps only, no descenders).
    for x0 in (5.8, W - 78.2):
        P.append(f'<rect x="{mm(x0)}" y="98.1" width="72.4" height="11.9" '
                 f'rx="1.5" fill="{PAPER_DEEP}" stroke="{LINE}" stroke-width="0.3"/>')
    # ROOM + TIME eyebrow rules (text itself comes from TEXTS)
    for ey in (69.2, 31.4):
        for (x0, x1) in ((CX-19.0, CX-8.0), (CX+8.0, CX+19.0)):
            P.append(f'<line x1="{mm(x0)}" y1="{mm(ey)}" x2="{mm(x1)}" y2="{mm(ey)}" '
                     f'stroke="{LINE}" stroke-width="0.25"/>')
    # brand flanking dots -- one per colour, flanking the top SPOTYMOD logo
    P.append(f'<circle cx="{mm(CX-15)}" cy="5.9" r="0.9" fill="{GREEN}"/>')
    P.append(f'<circle cx="{mm(CX+15)}" cy="5.9" r="0.9" fill="{COPPER}"/>')
    # glyphs + labels
    for c in PARAMS + INPUTS + OUTPUTS + LIGHTS:
        c.r = GLYPH_R[c.kind]
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
            P.append(f'<text x="{mm(c.x)}" y="{mm(c.y+c.r+2.5)}" fill="{INK}" '
                     f'text-anchor="middle" font-family="monospace" font-size="2.0">{c.label}</text>')
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
    L2.append("struct PanelCtl { int id; WidgetKind kind; XY mm; const char* label; };")
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

    def emit_table(name, items):
        L2.append(f"static const PanelCtl {name}[] = {{")
        for c in items:
            L2.append(f'    {{{c.enum}, {WKMAP[c.kind]}, {{{c.x:.3f}f, {c.y:.3f}f}}, "{c.label}"}},')
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

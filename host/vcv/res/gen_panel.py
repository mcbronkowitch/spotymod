#!/usr/bin/env python3
"""Single source of truth for the Spotymod VCV panel.

Layout mirrors the real Spotykach hardware: two symmetric halves, each built
around a 32-LED ring with its macro knobs encircling it and its secondary
functions as a row of small "pads" (buttons) below; a narrow shared center
strip (interaction + room + master + clock) sits between them.

Emits (both committed):
  - res/Spotymod.svg          the faceplate
  - src/generated_panel.hpp   enums + PART_STRIDE + control table

Run from host/vcv/:  python3 res/gen_panel.py
The C++ never hardcodes a coordinate; it reads them from the generated header,
so panel graphics and widget placement can never drift apart.
"""
import os, math

HP = 42
MM_PER_HP = 5.08
W  = HP * MM_PER_HP          # 213.36 mm
Hh = 128.5                   # standard Eurorack height

# --- control kinds ------------------------------------------------------------
BIGKNOB = "BIGKNOB"   # macro pot (0..1)
KNOBC   = "KNOBC"     # bipolar macro (-1..1)  (ENTROPY)
SMKNOB  = "SMKNOB"    # small secondary pot (0..1)
KNOBI   = "KNOBI"     # small integer pot (snap)
SW3     = "SW3"       # 3-pos switch (SYNC)
LATCH   = "LATCH"     # on/off pad button (binary)
SMBTN   = "SMBTN"     # momentary pad button
IN      = "IN"
OUT     = "OUT"
LIGHT   = "LIGHT"

GLYPH_R = {BIGKNOB:4.2, KNOBC:4.2, SMKNOB:3.0, KNOBI:3.0, SW3:2.2,
           LATCH:2.7, SMBTN:2.7, IN:4.2, OUT:4.2, LIGHT:1.7}
WKMAP = {BIGKNOB:"WK_BIGKNOB", KNOBC:"WK_KNOBC", SMKNOB:"WK_SMKNOB",
         KNOBI:"WK_KNOBI", SW3:"WK_SW3", LATCH:"WK_LATCH", SMBTN:"WK_SMBTN",
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

# --- per-part control template (ORDER defines enum order; identical A/B) ------
# Returns Ctl list with LEFT-side coordinates. Mirror for B by x -> W-x.
def part_controls():
    cx = RING_CX_A
    out = []
    # 8 macro knobs encircling the ring
    macros = [("RATE","RATE"),("SHAPE","SHAPE"),("PROB","PROB"),("SMOOTH","SMTH"),
              ("RANGE","RANGE"),("ENTROPY","ENTR"),("DEPTH","DEPTH"),("TUNE","TUNE")]
    for i,(enum,lbl) in enumerate(macros):
        x,y = orbit(cx, RING_CY, KNOB_R, i, 8, start_deg=0.0)
        out.append(Ctl(enum, KNOBC if enum=="ENTROPY" else BIGKNOB, x, y, lbl))
    # voice row (small)
    for i,(enum,lbl) in enumerate([("ATTACK","ATK"),("DECAY","DEC"),("RES","RES"),
                                   ("SUB","SUB"),("DETUNE","DTUN")]):
        out.append(Ctl(enum, SMKNOB, 16.0 + i*13.0, 74.0, lbl))
    # fx row (small) + steps
    for i,(enum,lbl) in enumerate([("FLUX","FLUX"),("GRIT","GRIT"),("COMP","COMP")]):
        out.append(Ctl(enum, SMKNOB, 22.0 + i*13.0, 86.0, lbl))
    out.append(Ctl("STEPS", KNOBI, 61.0, 86.0, "STPS"))
    # pad row: secondary functions as small buttons (mirror of hardware pads)
    pads = [("SYNC",SW3,"SYNC"),("ENGINE",LATCH,"ENG"),("GRITMODE",LATCH,"GRIT"),
            ("STEP",LATCH,"STEP"),("REPLAY",LATCH,"RPLY"),
            ("CAPTURE",SMBTN,"CAP"),("TRIGGER",SMBTN,"TRIG")]
    for i,(enum,kind,lbl) in enumerate(pads):
        out.append(Ctl(enum, kind, 13.0 + i*10.5, 99.0, lbl))
    return out

def mirror(ctls):
    return [Ctl(c.enum, c.kind, W - c.x, c.y, c.label) for c in ctls]

PART_A = [Ctl(c.enum + "_A", c.kind, c.x, c.y, c.label) for c in part_controls()]
PART_B = [Ctl(c.enum + "_B", c.kind, c.x, c.y, c.label) for c in mirror(part_controls())]
PART_STRIDE = len(PART_A)

# --- shared center strip ------------------------------------------------------
CX = W / 2.0
L, R = CX - 10.5, CX + 10.5
SHARED = [
    Ctl("MORPH",  BIGKNOB, CX,  16.0, "MORPH"),
    Ctl("COUPLE", SMKNOB,  L,   32.0, "COUPL"),
    Ctl("SCALE",  KNOBI,   CX,  32.0, "SCALE"),
    Ctl("DRIFT",  SMKNOB,  R,   32.0, "DRIFT"),
    Ctl("SPOT",   SMBTN,   L,   45.0, "SPOT"),
    Ctl("MASTER_DRIVE", SMKNOB, CX, 45.0, "DRIVE"),
    Ctl("SETTLE", SMBTN,   R,   45.0, "SETL"),
    Ctl("REV_SIZE",  SMKNOB, L,  60.0, "SIZE"),
    Ctl("REV_DECAY", SMKNOB, R,  60.0, "DECAY"),
    Ctl("REV_TONE",  SMKNOB, L,  72.0, "TONE"),
    Ctl("REV_DEPTH", SMKNOB, R,  72.0, "DEPTH"),
    Ctl("TEMPO",  SMKNOB, CX,  66.0, "TEMPO"),
]

PARAMS = PART_A + PART_B + SHARED

# --- inputs / outputs / lights ------------------------------------------------
INPUTS = [
    Ctl("IN_L",  IN, L,  88.0, "IN L"),
    Ctl("IN_R",  IN, R,  88.0, "IN R"),
    Ctl("CLOCK", IN, CX, 88.0, "CLK"),
    Ctl("RESET", IN, CX, 101.0, "RST"),
]
OUTPUTS = [
    Ctl("OUT_L", OUT, L, 101.0, "OUT L"),
    Ctl("OUT_R", OUT, R, 101.0, "OUT R"),
    # per-part modulation taps, bottom-outer on each side (mirrored)
    Ctl("PITCH_A", OUT, 20.0,     117.0, "PIT"),
    Ctl("GATE_A",  OUT, 33.0,     117.0, "GATE"),
    Ctl("PITCH_B", OUT, W - 20.0, 117.0, "PIT"),
    Ctl("GATE_B",  OUT, W - 33.0, 117.0, "GATE"),
]
LIGHTS = [
    # glow at each ring center, driven by that part's gate
    Ctl("GATE_A_L", LIGHT, RING_CX_A,     RING_CY, ""),
    Ctl("GATE_B_L", LIGHT, W - RING_CX_A, RING_CY, ""),
]

# =============================================================================
#  SVG
# =============================================================================
def mm(v): return f"{v:.3f}"

def ring_svg(cx, lit):
    """One LED ring. `lit` is a set of dot indices drawn glowing mint."""
    P = [f'<circle cx="{mm(cx)}" cy="{mm(RING_CY)}" r="{mm(RING_R+2.4)}" '
         f'fill="#0a0b10" stroke="#22252f" stroke-width="0.4"/>']
    for i in range(32):
        a = math.radians(360.0 * i / 32.0)
        x = cx + RING_R * math.sin(a)
        y = RING_CY - RING_R * math.cos(a)
        if i in lit:  # accent = LED mint, unlit = dim track (#1C4A43 family)
            P.append(f'<circle cx="{mm(x)}" cy="{mm(y)}" r="0.95" fill="#6de0c8" '
                     f'filter="url(#ledGlow)"/>')
        else:
            P.append(f'<circle cx="{mm(x)}" cy="{mm(y)}" r="0.7" fill="#1c4a43"/>')
    return "\n".join(P)

def svg():
    P = []
    P.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{mm(W)}mm" '
             f'height="{mm(Hh)}mm" viewBox="0 0 {mm(W)} {mm(Hh)}">')
    # design tokens realised as reusable defs (knob metal, LED glow, plate)
    P.append(
        '<defs>'
        '<radialGradient id="knobMetal" cx="38%" cy="32%" r="75%">'
        '<stop offset="0%" stop-color="#2b2e3a"/>'
        '<stop offset="55%" stop-color="#1b1d26"/>'
        '<stop offset="100%" stop-color="#0f1017"/></radialGradient>'
        '<radialGradient id="plate" cx="50%" cy="-5%" r="120%">'
        '<stop offset="0%" stop-color="#15161f"/>'
        '<stop offset="100%" stop-color="#0d0e15"/></radialGradient>'
        '<filter id="ledGlow" x="-140%" y="-140%" width="380%" height="380%">'
        '<feGaussianBlur stdDeviation="0.6" result="b"/>'
        '<feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge>'
        '</filter></defs>')
    P.append(f'<rect x="0" y="0" width="{mm(W)}" height="{mm(Hh)}" rx="2.5" fill="url(#plate)"/>')
    # subtle vertical seams framing the center strip
    for sx in (CX - 22.5, CX + 22.5):
        P.append(f'<line x1="{mm(sx)}" y1="6" x2="{mm(sx)}" y2="{mm(Hh-6)}" '
                 f'stroke="#20222c" stroke-width="0.4"/>')
    # mounting holes
    for hx in (MM_PER_HP, W - MM_PER_HP):
        for hy in (3.0, Hh-3.0):
            P.append(f'<circle cx="{mm(hx)}" cy="{mm(hy)}" r="1.6" fill="#2a2c38"/>')
    # two rings (a suggestive lit pattern per side; runtime widget animates them)
    P.append(ring_svg(RING_CX_A,     {0, 3, 4, 9, 14, 15, 22, 27}))
    P.append(ring_svg(W - RING_CX_A, {0, 5, 10, 11, 18, 23, 24, 29}))
    # pad-row backplates
    for (x0, x1) in ((8.0, 84.0), (W-84.0, W-8.0)):
        P.append(f'<rect x="{mm(x0)}" y="94.0" width="{mm(x1-x0)}" height="11.0" '
                 f'rx="1.5" fill="#0c0d13" stroke="#20222c" stroke-width="0.3"/>')
    # center strip plate
    P.append(f'<rect x="{mm(CX-21)}" y="10.0" width="42.0" height="98.0" rx="2" '
             f'fill="#0c0d13" stroke="#20222c" stroke-width="0.3"/>')
    # titles
    P.append(f'<text x="{mm(RING_CX_A)}" y="{mm(RING_CY+1)}" fill="#3a5d55" '
             f'text-anchor="middle" font-family="sans-serif" font-size="5" '
             f'font-weight="bold">A</text>')
    P.append(f'<text x="{mm(W-RING_CX_A)}" y="{mm(RING_CY+1)}" fill="#3a5d55" '
             f'text-anchor="middle" font-family="sans-serif" font-size="5" '
             f'font-weight="bold">B</text>')
    for (ty, txt) in ((57.0,"ROOM"), (85.0,"CLOCK / MIX")):
        P.append(f'<text x="{mm(CX)}" y="{mm(ty)}" fill="#6de0c8" text-anchor="middle" '
                 f'font-family="sans-serif" font-size="2.4">{txt}</text>')
    # brand
    P.append(f'<text x="{mm(CX)}" y="{mm(Hh-2.5)}" fill="#6de0c8" text-anchor="middle" '
             f'font-family="monospace" font-size="4.5" font-weight="bold">spotymod</text>')
    # glyphs + labels
    for c in PARAMS + INPUTS + OUTPUTS + LIGHTS:
        r = GLYPH_R[c.kind]
        if c.kind in (IN, OUT):
            P.append(f'<circle cx="{mm(c.x)}" cy="{mm(c.y)}" r="{mm(r)}" '
                     f'fill="#08080d" stroke="#5a5d6d" stroke-width="0.4"/>')
            P.append(f'<circle cx="{mm(c.x)}" cy="{mm(c.y)}" r="1.3" fill="#2a2c38"/>')
        elif c.kind == LIGHT:  # gate glow at ring centre -- warm signal hue
            P.append(f'<circle cx="{mm(c.x)}" cy="{mm(c.y)}" r="{mm(r)}" '
                     f'fill="#ffb454" opacity="0.85" filter="url(#ledGlow)"/>')
        elif c.kind == SW3:
            P.append(f'<rect x="{mm(c.x-1.4)}" y="{mm(c.y-2.4)}" width="2.8" '
                     f'height="4.8" rx="0.6" fill="#20222c" stroke="#5a5d6d" stroke-width="0.3"/>')
        elif c.kind in (LATCH, SMBTN):   # pads
            P.append(f'<rect x="{mm(c.x-r)}" y="{mm(c.y-r)}" width="{mm(2*r)}" '
                     f'height="{mm(2*r)}" rx="1.0" fill="#1a1c24" '
                     f'stroke="#6de0c8" stroke-width="0.3"/>')
        else:  # knobs -- metal cap + accent collar + indicator tick
            big = c.kind in (BIGKNOB, KNOBC)
            outer = "#6de0c8" if big else "#4a8d80"
            tick  = "#6de0c8" if big else "#8fbfb6"
            P.append(f'<circle cx="{mm(c.x)}" cy="{mm(c.y)}" r="{mm(r)}" '
                     f'fill="url(#knobMetal)" stroke="{outer}" '
                     f'stroke-width="{0.42 if big else 0.34}"/>')
            P.append(f'<line x1="{mm(c.x)}" y1="{mm(c.y)}" x2="{mm(c.x)}" '
                     f'y2="{mm(c.y-r+0.7)}" stroke="{tick}" stroke-width="0.5" '
                     f'stroke-linecap="round"/>')
        if c.label:
            P.append(f'<text x="{mm(c.x)}" y="{mm(c.y+r+2.5)}" fill="#c9ccd8" '
                     f'text-anchor="middle" font-family="sans-serif" font-size="2.0">{c.label}</text>')
    P.append('</svg>')
    return "\n".join(P)

# =============================================================================
#  C++ header
# =============================================================================
def header():
    L2 = []
    L2.append("// GENERATED by res/gen_panel.py -- do not edit by hand.")
    L2.append("#pragma once")
    L2.append("namespace spkyvcv {")
    L2.append("struct XY { float x, y; };")
    L2.append("enum WidgetKind { WK_BIGKNOB, WK_KNOBC, WK_SMKNOB, WK_KNOBI, "
              "WK_SW3, WK_LATCH, WK_SMBTN, WK_IN, WK_OUT, WK_LIGHT };")
    L2.append("struct PanelCtl { int id; WidgetKind kind; XY mm; const char* label; };")
    L2.append(f"static constexpr int PART_STRIDE = {PART_STRIDE};")

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

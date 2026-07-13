#!/usr/bin/env python3
"""Single source of truth for the Spotymod VCV panel.

Emits two generated artifacts (both committed):
  - res/Spotymod.svg          the faceplate
  - src/generated_panel.hpp   enums + a control table the C++ loops over

Run from host/vcv/:  python3 res/gen_panel.py
The C++ never hardcodes a coordinate; it reads them from the generated header,
so panel graphics and widget placement can never drift apart.
"""
import os

HP = 42
MM_PER_HP = 5.08
W = HP * MM_PER_HP          # 213.36 mm
Hh = 128.5                  # standard Eurorack height

# --- control kinds ------------------------------------------------------------
# name -> (svg glyph radius mm, c++ WidgetKind)
KNOB   = "KNOB"     # 0..1
KNOBC  = "KNOBC"    # bipolar -1..1
KNOBI  = "KNOBI"    # integer snap
SW2    = "SW2"      # 2-pos switch
SW3    = "SW3"      # 3-pos switch
TOG    = "TOG"      # on/off
BTN    = "BTN"      # momentary
IN     = "IN"       # input jack
OUT    = "OUT"      # output jack
LIGHT  = "LIGHT"

GLYPH_R = {KNOB:4.0, KNOBC:4.0, KNOBI:4.0, SW2:2.2, SW3:2.2, TOG:2.2,
           BTN:2.6, IN:4.2, OUT:4.2, LIGHT:1.6}

# --- layout helpers -----------------------------------------------------------
class Ctl:
    def __init__(self, enum, kind, x, y, label):
        self.enum, self.kind, self.x, self.y, self.label = enum, kind, x, y, label

controls = []   # order defines enum order within each id-space

def grid(items, x0, y0, cols, dx, dy, suffix=""):
    """Place items on a grid, return list of Ctl."""
    out = []
    for i, (enum, kind, label) in enumerate(items):
        c, r = i % cols, i // cols
        out.append(Ctl(enum + suffix, kind, x0 + c*dx, y0 + r*dy, label))
    return out

# Per-part control template (same for A and B). 3 columns.
def part_block(x0, y0, part):  # part: "A"/"B"
    s = "_" + part
    items = [
        ("RATE",    KNOB,  "RATE"),
        ("SHAPE",   KNOB,  "SHAPE"),
        ("PROB",    KNOB,  "PROB"),
        ("SMOOTH",  KNOB,  "SMOOTH"),
        ("RANGE",   KNOB,  "RANGE"),
        ("ENTROPY", KNOBC, "ENTR"),
        ("DEPTH",   KNOB,  "DEPTH"),
        ("TUNE",    KNOB,  "TUNE"),
        ("ATTACK",  KNOB,  "ATK"),
        ("DECAY",   KNOB,  "DEC"),
        ("RES",     KNOB,  "RES"),
        ("SUB",     KNOB,  "SUB"),
        ("DETUNE",  KNOB,  "DTUN"),
        ("FLUX",    KNOB,  "FLUX"),
        ("GRIT",    KNOB,  "GRIT"),
        ("COMP",    KNOB,  "COMP"),
    ]
    block = grid(items, x0, y0, 4, 13.0, 15.0, suffix=s)
    # switches / buttons row beneath the knob grid
    ry = y0 + 4*15.0 + 2.0
    sw = [
        ("SYNC",     SW3, "SYNC"),
        ("ENGINE",   SW2, "ENG"),
        ("STEP",     TOG, "STEP"),
        ("STEPS",    KNOBI, "STPS"),
        ("GRITMODE", SW2, "G.MD"),
        ("REPLAY",   TOG, "RPLY"),
        ("CAPTURE",  BTN, "CAP"),
        ("TRIGGER",  BTN, "TRIG"),
    ]
    block += grid(sw, x0, ry, 4, 13.0, 12.0, suffix=s)
    return block

# ---- assemble the param-space (order == ParamId order) -----------------------
PARAMS = []
PARAMS += part_block(12.0, 20.0, "A")
PARAMS += part_block(64.0, 20.0, "B")

# shared center / reverb / master / global column
shared = [
    ("MORPH",  KNOB,  "MORPH"),
    ("COUPLE", KNOB,  "COUPL"),
    ("DRIFT",  KNOB,  "DRIFT"),
    ("SPOT",   BTN,   "SPOT"),
    ("SETTLE", BTN,   "SETL"),
    ("REV_SIZE",  KNOB, "SIZE"),
    ("REV_DECAY", KNOB, "DECAY"),
    ("REV_TONE",  KNOB, "TONE"),
    ("REV_DEPTH", KNOB, "DEPTH"),
    ("MASTER_DRIVE", KNOB, "DRIVE"),
    ("SCALE",  KNOBI, "SCALE"),
    ("TEMPO",  KNOB,  "TEMPO"),
]
PARAMS += grid(shared, 116.0, 20.0, 2, 15.0, 15.0)

# ---- inputs / outputs / lights ----------------------------------------------
INPUTS = [
    Ctl("IN_L", IN, 150.0, 20.0, "IN L"),
    Ctl("IN_R", IN, 162.0, 20.0, "IN R"),
    Ctl("CLOCK", IN, 150.0, 34.0, "CLK"),
    Ctl("RESET", IN, 162.0, 34.0, "RST"),
]
OUTPUTS = [
    Ctl("OUT_L", OUT, 150.0, 108.0, "OUT L"),
    Ctl("OUT_R", OUT, 162.0, 108.0, "OUT R"),
    Ctl("PITCH_A", OUT, 150.0, 60.0, "PIT A"),
    Ctl("GATE_A",  OUT, 162.0, 60.0, "GAT A"),
    Ctl("PITCH_B", OUT, 150.0, 74.0, "PIT B"),
    Ctl("GATE_B",  OUT, 162.0, 74.0, "GAT B"),
]
LIGHTS = [
    Ctl("GATE_A_L", LIGHT, 150.0, 52.0, ""),
    Ctl("GATE_B_L", LIGHT, 162.0, 52.0, ""),
]

# =============================================================================
#  SVG
# =============================================================================
def mm(v): return f"{v:.3f}"

def svg():
    P = []
    P.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{mm(W)}mm" '
             f'height="{mm(Hh)}mm" viewBox="0 0 {mm(W)} {mm(Hh)}">')
    P.append(f'<rect x="0" y="0" width="{mm(W)}" height="{mm(Hh)}" fill="#12131a"/>')
    # mounting holes
    for hx in (2*MM_PER_HP*0.5, W - MM_PER_HP):
        for hy in (3.0, Hh-3.0):
            P.append(f'<circle cx="{mm(hx)}" cy="{mm(hy)}" r="1.6" fill="#2a2c38"/>')
    # section frames + titles
    frames = [
        (7, 13, 50, 96, "PART A"),
        (59, 13, 50, 96, "PART B"),
        (112, 13, 30, 96, "SHARED"),
        (145, 13, 24, 96, "I / O"),
    ]
    for (fx, fy, fw, fh, title) in frames:
        P.append(f'<rect x="{mm(fx)}" y="{mm(fy)}" width="{mm(fw)}" height="{mm(fh)}" '
                 f'rx="2" fill="none" stroke="#3a3d4d" stroke-width="0.3"/>')
        P.append(f'<text x="{mm(fx+2)}" y="{mm(fy+4)}" fill="#6de0c8" '
                 f'font-family="sans-serif" font-size="3.2">{title}</text>')
    # brand
    P.append(f'<text x="{mm(8)}" y="{mm(Hh-4)}" fill="#6de0c8" '
             f'font-family="monospace" font-size="4.5" font-weight="bold">spotymod</text>')
    P.append(f'<text x="{mm(W-40)}" y="{mm(Hh-4)}" fill="#4a4d5d" '
             f'font-family="sans-serif" font-size="3">modulation-first</text>')
    # glyphs + labels
    for c in PARAMS + INPUTS + OUTPUTS + LIGHTS:
        r = GLYPH_R[c.kind]
        col = "#c9ccd8"
        if c.kind in (IN, OUT):
            P.append(f'<circle cx="{mm(c.x)}" cy="{mm(c.y)}" r="{mm(r)}" '
                     f'fill="#0a0a0f" stroke="#5a5d6d" stroke-width="0.4"/>')
            P.append(f'<circle cx="{mm(c.x)}" cy="{mm(c.y)}" r="1.3" fill="#2a2c38"/>')
        elif c.kind == LIGHT:
            P.append(f'<circle cx="{mm(c.x)}" cy="{mm(c.y)}" r="{mm(r)}" fill="#301010"/>')
        elif c.kind in (SW2, SW3, TOG):
            P.append(f'<rect x="{mm(c.x-1.4)}" y="{mm(c.y-2.4)}" width="2.8" '
                     f'height="4.8" rx="0.6" fill="#20222c" stroke="#5a5d6d" stroke-width="0.3"/>')
        elif c.kind == BTN:
            P.append(f'<circle cx="{mm(c.x)}" cy="{mm(c.y)}" r="{mm(r)}" '
                     f'fill="#20222c" stroke="#5a5d6d" stroke-width="0.4"/>')
        else:  # knobs
            P.append(f'<circle cx="{mm(c.x)}" cy="{mm(c.y)}" r="{mm(r)}" '
                     f'fill="#1b1d26" stroke="#6de0c8" stroke-width="0.35"/>')
            P.append(f'<circle cx="{mm(c.x)}" cy="{mm(c.y-r+0.9)}" r="0.5" fill="#6de0c8"/>')
        if c.label:
            P.append(f'<text x="{mm(c.x)}" y="{mm(c.y+r+2.6)}" fill="{col}" '
                     f'text-anchor="middle" font-family="sans-serif" font-size="2.1">{c.label}</text>')
    P.append('</svg>')
    return "\n".join(P)

# =============================================================================
#  C++ header
# =============================================================================
def cpp_vec(c):
    return f"{{{c.x:.3f}f, {c.y:.3f}f}}"

def header():
    L = []
    L.append("// GENERATED by res/gen_panel.py -- do not edit by hand.")
    L.append("#pragma once")
    L.append("namespace spkyvcv {")
    L.append("struct XY { float x, y; };")
    L.append("enum WidgetKind { WK_KNOB, WK_KNOBC, WK_KNOBI, WK_SW2, WK_SW3, "
             "WK_TOG, WK_BTN, WK_IN, WK_OUT, WK_LIGHT };")
    L.append("struct PanelCtl { int id; WidgetKind kind; XY mm; const char* label; };")

    def emit_enum(name, items, terminator):
        L.append(f"enum {name} {{")
        for c in items:
            L.append(f"    {c.enum},")
        L.append(f"    {terminator}")
        L.append("};")

    kmap = {KNOB:"WK_KNOB", KNOBC:"WK_KNOBC", KNOBI:"WK_KNOBI", SW2:"WK_SW2",
            SW3:"WK_SW3", TOG:"WK_TOG", BTN:"WK_BTN", IN:"WK_IN", OUT:"WK_OUT",
            LIGHT:"WK_LIGHT"}

    emit_enum("ParamId",  PARAMS,  "NUM_PARAMS")
    emit_enum("InputId",  INPUTS,  "NUM_INPUTS")
    emit_enum("OutputId", OUTPUTS, "NUM_OUTPUTS")
    emit_enum("LightId",  LIGHTS,  "NUM_LIGHTS")

    def emit_table(name, items):
        L.append(f"static const PanelCtl {name}[] = {{")
        for c in items:
            L.append(f'    {{{c.enum}, {kmap[c.kind]}, {cpp_vec(c)}, "{c.label}"}},')
        L.append("};")

    emit_table("kParamCtls",  PARAMS)
    emit_table("kInputCtls",  INPUTS)
    emit_table("kOutputCtls", OUTPUTS)
    emit_table("kLightCtls",  LIGHTS)
    L.append("} // namespace spkyvcv")
    return "\n".join(L) + "\n"

if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    with open(os.path.join(here, "Spotymod.svg"), "w") as f:
        f.write(svg())
    with open(os.path.join(root, "src", "generated_panel.hpp"), "w") as f:
        f.write(header())
    print("wrote res/Spotymod.svg and src/generated_panel.hpp")
    print(f"params={len(PARAMS)} inputs={len(INPUTS)} outputs={len(OUTPUTS)} "
          f"lights={len(LIGHTS)}  panel={HP}HP")

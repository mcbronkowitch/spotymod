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

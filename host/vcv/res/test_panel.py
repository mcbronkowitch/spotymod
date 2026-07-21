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
    'DUST_A', 'DUST_B', 'ROT_A', 'ROT_B',
    'REC_A', 'REC_B',
]
INPUT_ORDER = ['IN_L', 'IN_R', 'CLOCK', 'RESET']
OUTPUT_ORDER = ['OUT_L', 'OUT_R', 'PITCH_A', 'GATE_A', 'PITCH_B', 'GATE_B']
LIGHT_ORDER = ['GATE_A_L', 'GATE_B_L', 'REC_A_L', 'REC_B_L']


def test_enum_order():
    """Patch compatibility. If this fails, every saved .vcv breaks."""
    check([c.enum for c in g.PARAMS] == PARAM_ORDER, "PARAMS order changed")
    check([c.enum for c in g.INPUTS] == INPUT_ORDER, "INPUTS order changed")
    check([c.enum for c in g.OUTPUTS] == OUTPUT_ORDER, "OUTPUTS order changed")
    check([c.enum for c in g.LIGHTS] == LIGHT_ORDER, "LIGHTS order changed")
    check(g.PART_STRIDE == 23, f"PART_STRIDE is {g.PART_STRIDE}, must be 23")


def test_dust_params():
    """DUST/ROT are appended at the end of PARAMS, not templated into
    part_controls() -- appending keeps PART_STRIDE unchanged so TRIGGER_A/B,
    every part-B id and every already-appended tail param keep their id."""
    check(g.PART_STRIDE == 23, "PART_STRIDE must stay 23")
    ids = {c.enum: i for i, c in enumerate(g.PARAMS)}
    for e in ("DUST_A", "DUST_B", "ROT_A", "ROT_B"):
        check(e in ids, f"{e} missing")
        check(ids[e] >= 2 * g.PART_STRIDE, f"{e} must be appended, not templated")


def test_dust_rot_kind():
    """DUST/ROT must render as the small knob (GLYPH_R[SMKNOB] = 3.0 mm), not
    the big knob (GLYPH_R[BIGKNOB] = 4.2 mm) -- a SMKNOB->BIGKNOB typo grows
    the glyph radius by 1.2 mm and still clears test_no_overlap's minimum
    spacing in the FX row by 0.43 mm, so it would ship silently without a kind
    pin of its own. Same idiom as test_header_carries_label_columns's
    RATE_A/WK_BIGKNOB check -- read the actual generated header string, not
    g.PARAMS' in-memory `.kind`, so a generator bug that diverges the two
    still fails here (two previous panel guards on this project asserted
    against strings the generator never emits and were vacuous on arrival)."""
    h = g.header()
    for enum in ("DUST_A", "DUST_B", "ROT_A", "ROT_B"):
        check(h.count(f"{{{enum}, WK_SMKNOB,") == 1,
              f"{enum} is not WK_SMKNOB in the generated header")


def test_rec_params():
    """REC is appended, not templated -- appending keeps PART_STRIDE at 23 so
    every saved .vcv keeps its param ids. Same guard shape as
    test_dust_params, and the kind is pinned the same way test_dust_rot_kind
    pins DUST/ROT: a LATCH that silently became an SMBTN would still clear
    test_no_overlap (identical radius), so the kind needs its own check."""
    check(g.PART_STRIDE == 23, "PART_STRIDE must stay 23")
    ids = {c.enum: i for i, c in enumerate(g.PARAMS)}
    for e in ("REC_A", "REC_B"):
        check(e in ids, f"{e} missing")
        check(ids[e] >= 2 * g.PART_STRIDE, f"{e} must be appended, not templated")
    check(ids["REC_A"] > ids["ROT_B"], "REC must append AFTER the existing tail")
    h = g.header()
    for e in ("REC_A", "REC_B"):
        check(h.count(f"{{{e}, WK_LATCH,") == 1,
              f"{e} is not WK_LATCH in the generated header")


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
    got = [(x, y, t) for (x, y, sz, sp, col, an, t) in g.TEXTS
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
        needle = f'cx="{g.mm(c.x)}" cy="{g.mm(c.y)}" r="{g.mm(c.r + 0.85)}"'
        check(needle not in s, f"{enum} still draws a collar")


LOWER_A = {   # enum -> (x, y)   part A; part B is W - x
    'ATTACK_A': (9.50, 77.30), 'DECAY_A': (22.50, 77.30), 'FILT_A': (35.50, 77.30),
    'RES_A': (9.50, 89.40), 'SUB_A': (22.50, 89.40), 'DETUNE_A': (35.50, 89.40),
    'FLUXRATE_A': (49.50, 77.30), 'FLUX_A': (62.75, 77.30), 'FLUXFB_A': (76.00, 77.30),
    'GRIT_A': (49.50, 89.40), 'COMP_A': (58.333, 89.40),
    'DUST_A': (67.167, 89.40), 'ROT_A': (76.00, 89.40),
    'ENGINE_A': (10.00, 103.60), 'GRITMODE_A': (17.50, 103.60),
    'STEPS_A': (37.00, 103.60), 'STEP_A': (46.00, 103.60),
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
                  for (tx, ty, sz, sp, col, an, t) in g.TEXTS),
              f"legend text for {name} missing at ({x + 5.0:.2f}, {y + 0.75:.2f})")


def test_pad_backplates_are_gone():
    check('width="72.4" height="11.9"' not in g.svg(),
          "the old pad backplate is still drawn")


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
    check('width="42.0" height="100.0"' not in g.svg(),
          "the full-height centre card is still drawn")


def test_old_eyebrow_texts_are_gone():
    """TIME/ROOM are group legends now, not free-floating eyebrows."""
    for (x, y, sz, sp, col, an, t) in g.TEXTS:
        if t in ('TIME', 'ROOM'):
            check(approx(sz, 1.8) and approx(x, g.CX - 20.5 + 5.0),
                  f"{t} is still the old eyebrow (size {sz} at x {x:.2f})")


def test_room_is_flush_with_play():
    """Spec §6: ROOM's bottom edge lines up with the PLAY boxes."""
    room = next((gr for gr in g.GROUPS if gr[4] == 'ROOM'), None)
    play = next((gr for gr in g.GROUPS if gr[4] == 'PLAY'), None)
    check(room is not None, "ROOM group missing from g.GROUPS")
    check(play is not None, "PLAY group missing from g.GROUPS")
    if room is not None and play is not None:
        check(approx(room[1] + room[3], play[1] + play[3]),
              f"ROOM ends at {room[1] + room[3]:.2f}, PLAY at {play[1] + play[3]:.2f}")


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


def test_config_wires_tip_not_label():
    """The generated header carries a real tooltip in `tip` (see
    test_header_carries_tooltips), but a header can be correct while the C++
    that reads it quietly regresses -- e.g. `configInput(c.id, c.label)`
    still compiles, still passes every other test here, and only shows up
    when a human hovers a jack in Rack and sees "L" instead of "IN L". This
    guard reads the actual C++ source so that regression fails the suite
    instead of waiting for a human to notice (spec 2026-07-18, Task 6 review)."""
    here = os.path.dirname(os.path.abspath(__file__))
    cpp_path = os.path.join(here, "..", "src", "Spotymod.cpp")
    with open(cpp_path) as f:
        cpp = f.read()
    check("configInput(c.id, c.tip)" in cpp,
          "configInput is not wired to c.tip -- jack tooltips will show panel labels")
    check("configOutput(c.id, c.tip)" in cpp,
          "configOutput is not wired to c.tip -- jack tooltips will show panel labels")


# --- 2026-07-21 morphagene-controls: the sampler meanings on the plate --------
# ENG remaps four knobs. Three get a second caption line; DENSITY deliberately
# does not -- DENS reads correctly in both engines (groove density / grain
# density), and the obvious alternative "MRPH" is already the global A/B knob's
# name, so putting it on a part knob would be an operating error by design.
SAMPLER_CAPTIONS = [("MELODY", "SCAN"), ("SUB", "SIZE"), ("DETUNE", "ORG")]


def sampler_text(word, near):
    """The SCAN/SIZE/ORG entry nearest to a given control glyph. Picking by
    distance rather than by exact coordinate keeps this test independent of
    how the generator derives the position -- it can only pass if the caption
    really landed next to its knob."""
    hits = [t for t in g.TEXTS if t[-1] == word]
    if not hits:
        return None
    return min(hits, key=lambda t: math.hypot(t[0] - near.x, t[1] - near.y))


def test_sampler_captions_exist():
    """Every remapped knob carries its sampler meaning on the plate."""
    txt = [t[-1] for t in g.TEXTS]
    for _base, word in SAMPLER_CAPTIONS:
        check(txt.count(word) == 2,
              f"sampler caption {word!r} appears {txt.count(word)}x, want 2 (A and B)")
    check("MRPH" not in txt,
          "DENS must keep its label -- MORPH is the global A/B control")
    check(ctl('DENSITY_A').label == 'DENS' and ctl('DENSITY_B').label == 'DENS',
          "DENSITY lost its DENS label")


def test_sampler_captions_sit_outside_their_labels():
    """The sampler line goes further out than the main caption -- never between
    the knob and the LED ring, and never on top of the caption it belongs to."""
    for suffix, cx, colour in (('_A', g.RING_CX_A, g.GREEN),
                               ('_B', g.W - g.RING_CX_A, g.COPPER)):
        for base, word in SAMPLER_CAPTIONS:
            c = ctl(base + suffix)
            lx, ly, anchor, _s, _col = g.label_of(c)
            t = sampler_text(word, c)
            check(t is not None, f"{c.enum}: no {word} caption at all")
            if t is None:
                continue
            main_d = math.hypot(lx - cx, ly - g.RING_CY)
            d = math.hypot(t[0] - cx, t[1] - g.RING_CY)
            check(d > main_d,
                  f"{c.enum}: {word} sits inside {c.label} (d={d:.2f} <= {main_d:.2f})")
            check(math.hypot(t[0] - c.x, t[1] - c.y) > g.GLYPH_R[c.kind],
                  f"{c.enum}: {word} sits on the knob glyph")
            # A left/right-aligned parent needs a left/right-aligned second
            # line, or the two captions stagger. This is why PanelTxt grew an
            # anchor column.
            check(t[5] == anchor,
                  f"{c.enum}: {word} anchored {t[5]!r}, parent is {anchor!r}")
            check(approx(t[2], 1.5), f"{c.enum}: {word} size {t[2]}, want 1.5")
            check(t[4] == colour,
                  f"{c.enum}: {word} colour {t[4]}, want {colour}")
            # Enough baseline separation that the glyph boxes cannot touch:
            # the parent caption is 1.9 mm tall.
            check(t[1] - ly >= 2.4,
                  f"{c.enum}: {word} only {t[1] - ly:.2f} mm below {c.label}")


def test_sampler_captions_clear_the_voice_box():
    """SIZE/ORG hang below the VOICE box. They must clear its bottom hairline
    instead of being struck through by it, and stay off the PLAY box above."""
    voice = next(gr for gr in g.GROUPS if gr[4] == 'VOICE')
    play = next(gr for gr in g.GROUPS if gr[4] == 'PLAY')
    bottom, top = voice[1] + voice[3], play[1]
    for word in ('SIZE', 'ORG'):
        for t in (x for x in g.TEXTS if x[-1] == word):
            cap_top = t[1] - 0.7 * t[2]     # monospace cap height ~0.7 em
            check(cap_top >= bottom,
                  f"{word} at y {t[1]:.2f} is struck by the VOICE border ({bottom})")
            check(t[1] <= top - 0.3,
                  f"{word} at y {t[1]:.2f} runs into the PLAY box ({top})")


def test_panel_texts_stay_on_the_plate():
    for t in g.TEXTS:
        check(1.0 <= t[0] <= g.W - 1.0 and 1.0 <= t[1] <= g.Hh - 1.0,
              f"panel text {t[-1]!r} off plate at ({t[0]:.2f}, {t[1]:.2f})")


def test_header_carries_text_anchor():
    """The SVG preview and Rack must align these the same way; the C++ can only
    do that if the generated table ships the anchor."""
    h = g.header()
    check("unsigned char anchor; const char* str;" in h,
          "PanelTxt has no anchor column")
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "..", "src", "Spotymod.cpp")) as f:
        cpp = f.read()
    check("alignOf(t.anchor)" in cpp,
          "the kPanelTexts draw loop ignores the anchor column")


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

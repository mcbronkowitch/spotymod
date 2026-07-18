# COLOR as a MOTION target — chord density that varies per note

**Date:** 2026-07-18
**Status:** approved (brainstorm with Bastian, 2026-07-18; revised same day:
bipolar additive with zero-gate instead of multiplicative ceiling — subtle
±1-zone motion, and a barely-open knob may modulate *upward* into chords)

## Problem

COLOR is the only pitch-layer macro that nothing can modulate. It is a plain
per-part scalar: `Instrument::set_color` → `Part::set_color` → `ChordBuilder`.
A hand can sweep it; the instrument cannot. So chords are as dense as you last
left them — every stab in a phrase carries the same number of notes.

What is wanted: **density that varies per note.** One stab lands as a single
tone, the next as a four-note chord, and the pattern walks against the melody
instead of tracking it.

### There is no free target slot

The starting assumption for this work was that a modulation target had come
free. It has not. The architecture is strictly 1:1 — `lane index == pad slot ==
target slot`, `LANE_COUNT = 5` — and every slot is consumed:

| Lane | Rate | Synth destination |
|------|------|-------------------|
| SOURCE | ×2 | TIMBRE (morph + detune law) |
| SIZE | ×½ | filter cutoff (+ FILT trim) |
| PITCH | ×1 | pitch (master lane) |
| MOTION | ×¾ | pan fan + drift amount |
| LEVEL | ×³⁄₂ | level |

The parallel FX row (`FXT_GRIT_INT`, `FXT_FLUX_TIME`, `FXT_FX_MIX`,
`FXT_REV_SEND`, `FXT_FLUX_FB`) is likewise 5 of 5. Nothing in the docs records
a freed slot; the removed reverb DEPTH and shimmer were reverb parameters, not
lane targets.

But no free slot is needed. The FX row already proves a lane can drive a second
destination with its own base and depth. COLOR becomes a **third destination**
of one existing lane.

## Design

### 1. MOTION drives it, alongside pan and drift

MOTION (×¾) is the sender. Two reasons: it runs polyrhythmically against the
melody, so the density pattern repeats on a different cycle than the notes and
walks; and it is the thinnest lane — its only synth destination is stereo pan
fan plus drift amount, despite `lane_id.h` still calling it "SHAPE / MOTION"
(the SHAPE half never arrived in the synth).

Pan and drift **stay**. MOTION now moves three things in lockstep: width, drift
and chord density. Dense becomes wide as a side effect — accepted deliberately
as one "fullness" gesture rather than paying for a separate depth control.

### 2. Bipolar additive with a zero-gate: the knob is the centre

In `Part`:

```
gate      = clampf(_color / kColorGate, 0, 1)
m         = _active[LANE_MOTION] ? _mod.lane_output(LANE_MOTION) * _depth * kColorMod * gate : 0
color_eff = clampf(_color + m, 0, 1)
```

- `_color` is the COLOR knob, stored in `Part` (new member).
- `_depth` is the existing MOD macro.
- `kColorMod` is an ear-tunable constant in `part.h`, starting at **0.2**,
  in the style of `kLevelFloor`.
- `kColorGate` ≈ **0.01**: the swing fades in over the first 1% of knob
  travel, so "barely open" already modulates at full depth while `COLOR = 0`
  stays structurally silent (the swing is multiplied by 0, not special-cased).
- `lane_output` is bipolar −1…+1 for texture lanes (range 1 → `apply_range`
  returns the raw value).

**Two invariants fall out structurally, not by care:**

| Condition | Result | Why it matters |
|-----------|--------|----------------|
| `COLOR = 0` | `gate = 0` → `color_eff = 0` — always one note | The chord layer's bit-identity guarantee survives untouched |
| `MOD = 0` | `m = 0` → `color_eff = COLOR` | Today's behaviour exactly; modulation is opt-in through a knob that already exists |

Every saved patch and every existing render that leaves COLOR at 0 is
unaffected, by construction rather than by tuning.

**Why additive, and why 0.2.** The intent is *subtle*: here and there a
neighbouring chord, not a density collapse. The ChordBuilder's zones are 0.25
wide (edges 0.125 / 0.375 / 0.625, ninth-flip 0.85), so a bipolar swing of
±0.2 crosses at most one zone edge in each direction — density pendles ±1
note around the knob position, and near the top the fifth↔ninth flip gives a
different chord *colour* at unchanged density. An additive swing (rather than
multiplicative) keeps that reach constant across the knob range; with a
multiplicative law a barely-open knob could never rise into chord territory,
which is explicitly wanted: at `COLOR` just past zero the positive peaks reach
the two-note zone — mostly single tones, occasionally a dyad. The negative
half simply clamps at 0 there.

### 3. Where it applies

`Part::process` computes `color_eff` and pushes it into the ChordBuilder via
`_chord.set_color()` immediately before the existing `_chord.apply(...)` call —
**per sample**, on the same cadence `apply` already runs at; the added cost is a
multiply, an add and a clamp. Both behaviours then come for free from code that
already exists:

- **STEP** — `_chord.build()` on a PITCH-lane fire samples whatever the color
  is at that instant, so each stab gets its own density. This is the feature.
- **FLOW** — the chord layer's live path with zone hysteresis keeps running, so
  a moving color reads as a surface that blooms and collapses on MOTION's
  clock.

`Part::set_color` stops writing straight into the ChordBuilder; it stores the
knob value. `trigger_manual()` needs no change — it builds from the color last
pushed by `process`.

CHOKE-suppressed fires keep their existing rule (they do not advance
voice-leading memory).

### 4. No new surface

No panel control, no scenario action, no parameter id, no `.vcv` compatibility
concern. Disabling works through MOTION's target-active flag, like every other
destination.

## Consequence to accept

`host/render/scenarios/chord_bloom.json` **will** sound different. It sweeps
COLOR up to 0.95 and never calls `set_depth`, so it runs at the boot `_depth =
1.0` with MOTION active — its chords will now breathe instead of holding a flat
density. This is a certainty, not a risk. The other three chord-layer baseline
scenarios sit at COLOR 0 and stay byte-identical; the bit-identity gate should
be re-run on those three and `chord_bloom`'s reference re-cut.

## Testing

- `COLOR = 0`, `MOD = 1`, MOTION active → the engine receives `n == 1` on every
  trigger across a full cycle (bit-identity invariant).
- `MOD = 0` → the value reaching the ChordBuilder equals the knob exactly, for
  several knob positions (today's-behaviour invariant).
- `COLOR = 0.005`, `MOD = 1`, MOTION active → at least one trigger across a
  full MOTION cycle receives `n >= 2` (the barely-open knob reaches upward
  into chords — the gate works and the swing is additive).
- `COLOR = 0.5`, `MOD = 1` → observed chord sizes over one MOTION cycle vary
  (min < max). A knob position near a zone edge, because at ±0.2 swing an
  edge-distant position may legitimately never cross one. Assert the spread,
  not specific sizes — zone edges carry hysteresis.
- MOTION target inactive → no modulation regardless of MOD.
- Determinism: two identically seeded renders stay byte-identical.

## Out of scope

- No new depth control for the COLOR destination (rejected: costs a knob per
  deck to decouple density from width).
- No selectable sender — MOTION is wired in. A per-pad target row for COLOR was
  considered and dropped as premature.
- No change to pan/drift, to the ChordBuilder's zone or voice-leading logic, or
  to any parameter id.
- Hardware panel mapping is untouched; MOTION already exists as a pad slot.

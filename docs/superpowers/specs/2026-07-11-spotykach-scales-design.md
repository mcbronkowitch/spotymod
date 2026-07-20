# Spotykach — Scale Quantization Design

Date: 2026-07-11
Status: approved (brainstorm 2026-07-11; UX review pass same day — inspection
via ALT-hold, relative scale stepping, change slew, V/Oct summing, persistence)
Extends: `2026-07-10-spotykach-modulation-first-synth-design.md` (which already
promises "quantized to semitones/scale" and "TUNE knobs set each part's root;
scale quantization is internal" without specifying either).

## Problem

The PITCH target feeds engines as a raw 0..1 value, so generated melodies are
uniformly random in pitch. We want melodies to sit in a musical scale by
default (Dorian), while keeping chromatic and fully free (unquantized) modes
for pitch-drift experiments.

## Musical model

**One global scale** shared by both parts, **plus a per-part quantize mode**:

| Mode  | Behavior |
|-------|----------|
| SCALE | quantize to the global scale (default for both parts) |
| CHROM | quantize to 12 semitones — drift steps audibly in semitones |
| FREE  | passthrough, exactly today's continuous behavior |

Global scale list — 6 scales ordered dark → bright, so the selection knob
sweep is a brightness axis:

| # | Scale | Semitones | Character |
|---|-------|-----------|-----------|
| 1 | Minor pentatonic | 0 3 5 7 10 | dark, forgiving |
| 2 | Aeolian (natural minor) | 0 2 3 5 7 8 10 | dark, full |
| 3 | **Dorian (default)** | 0 2 3 5 7 9 10 | melancholy with light |
| 4 | Major pentatonic | 0 2 4 7 9 | open, bright |
| 5 | Lydian | 0 2 4 6 7 9 11 | floating bright |
| 6 | Whole tone | 0 2 4 6 8 10 | weightless, centerless |

Boot default: scale = Dorian, both parts in SCALE mode.

**Root:** the per-part TUNE knob remains the root (as in the parent spec).
In SCALE/CHROM the root snaps to semitones; in FREE, TUNE stays continuous
as today. Both parts can play the same scale on different roots — or part B
drifts freely against part A's Dorian melody.

## Interface (hardware gestures — recorded now, wired in the UI milestone)

> **[Partially superseded by the M6 shell spec, 2026-07-12]** — the
> *model* below (one global scale + per-part mode, relative stepping,
> V/Oct summing, persistence) is unchanged and remains binding.
> Re-homed by M6: **ALT + PITCH-pad tap** collides with M6's targets-first
> map (that pad is GRIT; ALT+GRIT = GRIT on-off) — the per-part mode moves
> to a 3-zone SHAPE knob inside the PITCH pad's edit layer. **ALT-hold
> free inspection** yields to M4's COUPLE bar on the rings; quantize
> inspection folds into the PITCH pad's hold-to-inspect. **ALT + TUNE
> relative stepping stays** as specified here (inactive while an edit
> layer is latched). See the M6 spec's supersession table.

- **Hold ALT = free inspection.** The moment ALT is held — before any tap or
  knob move — both rings passively show their side's quantize state: scale
  raster with root bright (SCALE), all 12 lit (CHROM), smooth circular sweep
  (FREE). Inspecting never changes a value, matching the parent spec's
  hold-to-inspect rule. Only a subsequent tap or knob move acts.
- **Hold ALT + turn TUNE** = select the global scale, **relative stepping**:
  each ~sixth of knob travel steps one entry up/down the 6-scale list
  (clockwise = brighter). Relative motion avoids the two-knobs-one-value
  position conflict — either side's TUNE works and neither position is ever
  "wrong". On ALT release, TUNE returns to root duty **with pickup** (same
  catch-up mechanic as pad edits — no value jump).
- **ALT + PITCH-pad tap** = cycle that part's mode: SCALE → CHROM → FREE.
  The gesture is unclaimed (ALT+tap is only bound to SEQ and PLAY so far) and
  follows the existing release-based pad rules. Because ALT-hold already
  shows the current mode, the first tap is never a blind change.
- **Ring feedback on change:** stepping the scale updates the 12-slot octave
  raster live; a mode tap flashes the new mode's signature. On ALT release
  the rings return to the normal lane display.

## Architecture

**New module `engine/pitch/quantizer.h`** — small, near-stateless:

```cpp
enum class QuantMode { SCALE, CHROM, FREE };

class Quantizer {
public:
    void set_scale(uint16_t mask12);   // bitmask of the 12 semitones
    void set_mode(QuantMode m);
    void set_root(int semis);          // already snapped by the caller
    float process(float norm);         // 0..1 in, 0..1 out
};
```

- **Pitch contract:** normalized 0..1 = **36 semitones (3 octaves)**. This is
  what `TestToneEngine` already does (`110·8^p` → 110–880 Hz); the contract
  is made explicit and binds all future engines and the CV out.
- **Placement:** `Part::target_value(LANE_PITCH)` runs through the quantizer
  as the last stage — after base+depth and after lane smoothing. Consequence:
  SMOOTH glides step through scale notes as a glissando, EVOLVE walks the
  scale instead of wandering freely. `pitch_cv()` therefore yields the
  quantized value for engine, CV out, and the future sequencer capture — one
  source of truth for all three paths.
- **Hysteresis:** ±~15 cents at raster boundaries, otherwise a slowly
  drifting value flutters between two neighbor notes — ugly especially in
  the CHROM drift case. FREE is a pure passthrough with no computation.
- **Scale/mode changes apply immediately**, softened by a short internal
  slew (~30–50 ms) inside the quantizer for the change moment only — the
  characteristic step glissando stays, the hard click on a sounding drone
  goes. Steady-state quantization remains instant steps.
- **Capture reinterpretation (intended feature):** the sequencer capture
  stores raw lane values; quantization happens at the output. Capturing a
  melody and then stepping the scale re-voices the same melody in the new
  scale — a performance move, and the strongest argument for the central
  quantizer placement.
- **V/Oct path:** the quantizer is a callable stage, not welded to the
  target. When voices arrive (later milestone), they apply it to the **sum**
  of PITCH target + external V/Oct CV at latch time — per the parent spec's
  "V/Oct CV + modulator value, quantized". Quantizing only the internal
  target would let external CV bypass the scale.
- Other lanes (TIMBRE/LEVEL/…) are untouched; the quantizer applies to the
  PITCH slot only. The SuperModulator stays free of musical semantics.
- **Persistence:** global scale, per-part mode, and snapped root are saved
  in the inherited preset storage alongside the existing part state. A setup
  must wake up in the scale it went to sleep in; the boot default (Dorian,
  both parts SCALE) applies only to a blank state.

## Host, scenarios & tests

- **New scenario actions:** `set_scale` (global, index or name),
  `set_quant_mode` (per part), `set_root` (per part, semitones).
- **Demo scenario `dorian_vs_drift.json`:** part A plays the fixed 16-step
  melody quantized Dorian; part B starts in SCALE and switches mid-scene to
  FREE + EVOLVE — the drift experiment, renderable as WAV.
- **Existing scene:** `melody_then_drift.json` gets one added line
  (`set_quant_mode: free`) so it keeps sounding identical. Deliberate call:
  **Dorian is the boot default, in the host too** — no implicit FREE.
- **Tests:** quantizer unit tests (scale mapping, hysteresis behavior,
  change-slew settles within ~50 ms, FREE = bit-identical passthrough, root
  shift) plus a Part integration test asserting `pitch_cv()` lands only on
  allowed scale degrees in SCALE mode and the existing LOOP determinism
  guarantee is untouched.

## Out of scope

- Hardware UI wiring (ALT+TUNE / ALT+PITCH-pad) — comes with the UI
  milestone; this spec fixes the gestures so the UI work has a contract.
- Per-part scales (rejected: one global scale keeps the two parts
  harmonically coherent; FREE/CHROM per part covers the divergent cases).
- Larger scale lists / user-defined scales (rejected for now: no display,
  every scale must be blind-navigable).

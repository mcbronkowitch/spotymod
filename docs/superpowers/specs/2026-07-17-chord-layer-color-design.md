# CHORD layer — the COLOR knob

**Date:** 2026-07-17
**Status:** approved (brainstorm with Bastian, 2026-07-17)

## Problem

The instrument plays one note per trigger. Chords were on the wish list, but
every obvious packaging is wrong for this instrument:

- A **chord engine** would duplicate the whole harmonic question per sound
  source — the M5 sampler would need its own chord logic all over again.
- A **chord mode** (chords-only deck vs. melody deck, or a combi mode) adds an
  armed state you have to enter and leave. The capture sequencer taught us
  what that costs: great on paper, no fun live.
- A **Qu-Bit-style quality/inversion knob** (step through maj/min/7/9 chord
  types manually) fights the quantizer: explicit qualities can clash with the
  other part's scale-quantized melody, and nobody steps inversions in time
  with a random walk.

## Design

### 1. Placement — pitch layer, not engine

Chords are pitch material, not timbre. The chord builder lives in `Part`,
after the quantizer, before the engine trigger. Every engine behind
`IPartEngine` inherits it — the M2 synth today, the M5 sampler for free
tomorrow. This is why the milestone (M4.10) lands **before** M5.

### 2. COLOR — one knob, no mode

One new per-part knob, **COLOR**, 0..1:

| COLOR | result |
|-------|--------|
| 0     | one note — exactly today's behavior |
| ~0.25 | + fifth / octave below (power chord: fat, quality-neutral) |
| ~0.5  | close triad |
| ~0.75 | + seventh, voicing starts to open |
| 1.0   | 4 notes, wide spread, ninth as color tone |

The sweep is musically monotone: more is always denser/richer, never merely
different. At 0 the chord layer vanishes, so chord-vs-melody is a knob
position per part, not a deck assignment. It passes the capture test:
audible at every position, no armed state, no latching — turning it up is a
performance gesture (the arrangement blooms), not a menu item.

**COLOR is live, not latched (performance-review amendment).** In STEP the
chord is built at trigger time (stabs are short; the next trigger is near).
In FLOW, latching would make the knob dead until the next chord change —
at low DENS that is seconds of nothing, the capture mistake in a new coat.
So in FLOW COLOR acts **continuously on the sounding surface**: voices fade
in/out click-free as the knob crosses zones — turning up blooms the drone
into a pad, turning down collapses it to a single note. Zone boundaries
carry hysteresis (as in the quantizer) so a knob parked on an edge never
flutters.

**Loudness compensation.** One note vs. four is a +6..12 dB jump; without
compensation COLOR is secretly a volume knob that pumps the compressor. The
builder scales per-note gain ~1/sqrt(n) (equal-power) so COLOR changes
density and color, not level. Exact curve is ear-tunable.

Panel: COLOR joins the per-part macro row in the VCV panel. The hardware
panel cannot absorb a new knob as-is; resolving that is explicitly deferred
to the upcoming reduction/macro round (per the standing hardware-reducibility
constraint).

### 3. Chord construction — diatonic stacking

Chord tones are built by taking every **second scale note** above the root
(root, skip one, take one, …) using the active quantizer mask. The scale is
the harmony teacher:

- Dorian on the tonic → minor; on the IV → major; on the degree above the
  fifth → diminished. Quality is emergent, never selected.
- Pentatonic scales yield partly quartal stacks — the open, modern voicing
  character comes for free.
- Because both parts draw from the same scale, chords and the other part's
  melody are guaranteed to harmonize. No quality control exists or is needed.

CHROM mode stacks over the chromatic mask (yielding minor-third stacks); in
FREE mode the root stays unquantized as today, and the stack intervals are
built from the last active scale mask on top of that free root — COLOR stays
useful during drift experiments.

### 4. Change logic — nothing new

- **Root** = the PITCH lane's quantized output, latched per trigger (today's
  path, unchanged).
- **Harmonic rhythm** = DENS (trigger probability), as for single notes.
- **Progression behavior** = MELO. The entropy sequencer already freezes the
  pitch walk into a loop at negative MELO — which means *a frozen, looping
  chord progression* already exists as a side effect. The "dedicated harmony
  generator with freezable progressions" idea is delivered by zero new code.

### 5. Voicing — automatic voice-leading, no inversion control

On each root change the builder picks the chord lay (inversion/octave
placement) that minimizes total semitone movement of the individual voices
from the previous chord — common tones stay put, chords glide into each
other. COLOR sets the allowed spread; voice-leading works inside it. All
chord tones fold into the 36-semitone pitch contract (0..1 normalized).
The builder is deterministic: same root sequence + same COLOR = same voicings.

### 6. Engine interface

`IPartEngine` gains:

```cpp
virtual void trigger_chord(const float* pitches_norm, int n);
```

with a default implementation that loops `trigger()` — the test tone and any
future engine that doesn't care stay untouched. `Part` calls `trigger_chord`
always (n = 1 at COLOR = 0).

**SynthEngine** overrides it:

- **STEP:** chord stabs on the AD envelopes, with a few milliseconds of
  deterministic, seeded micro-offset per chord tone (ear-tunable constant, no
  control) — four voices firing on the same sample sound like a home organ;
  this is humanization, NOT the cut strum feature.
- **FLOW:** the whole chord becomes the sustaining surface — all chord voices
  decay to the sustain level and hold; their pitches follow the (re-voiced)
  chord targets, and COLOR adds/removes surface voices live (click-free
  fades, see §2). The next trigger crossfades to the new chord surface
  exactly as the single sustaining voice hands over today, just polyphonic.
  The drone promise (auto-trigger on entering FLOW) fires a chord at the
  current COLOR.
- **CHOKE:** releases the entire surface, click-free, re-arms on release —
  the existing `set_hold` contract, unchanged in meaning.

**Known feel (accepted, so the play test isn't surprised):**

- STEP at COLOR max: a 4-note stab uses all 4 voices, so each new chord cuts
  the previous tails entirely. Accepted — most of the sweep lives at 2–3
  notes with a spare voice, and the pad magic lives in FLOW anyway.
- A chord surface counts as a held note for CHOKE: with choke priority set, a
  standing pad on the priority part blocks the other part permanently. This
  is the existing, intended drone semantics — chords just make the case more
  likely.

### 7. Testing

- Unit tests (doctest): degree qualities per scale (Dorian i = minor,
  IV = major, vi° = diminished; pentatonic quartal stacks), COLOR sweep
  monotonicity (note count and spread never decrease), zone-boundary
  hysteresis (a COLOR value parked on an edge never flutters), voice-leading
  optimality (movement cost ≤ any other lay of the same chord), 36-semi
  fold, gain compensation (RMS roughly level across the sweep on a static
  chord), determinism across runs (incl. the seeded stab micro-offsets),
  `trigger_chord` default = n × `trigger`.
- FLOW live-response test: with no new trigger, moving COLOR changes the
  number of sustaining voices, click-free (no sample-to-sample jump above
  threshold).
- Render scenarios: STEP stabs and FLOW pad crossfades at several COLOR
  positions, plus a COLOR sweep over a standing FLOW pad; `mods.csv`
  inspected for chord pitch sets.

## Explicitly out of scope (YAGNI)

- Explicit chord-quality selection (the scale is the quality axis).
- Manual inversion stepping (voice-leading owns the lay).
- Strum / arpeggio spread of the chord trigger (the STEP micro-offset is
  humanization on a fixed few-ms scale, not a playable spread).
- Combi split (chords + independent melody inside one part) — two parts
  already cover it; revisit only if both parts are otherwise occupied.
- Hardware panel placement of COLOR — deferred to the reduction/macro round.

# Spotykach Per-Step Sample & Hold — Melody Generation Fix

> **SUPERSEDED (2026-07-12)** by
> `2026-07-12-spotykach-entropy-sequencer-design.md`. Listening tests showed
> pure per-step randomness yields unusable melodies ~95% of the time; the
> entropy sequencer replaces it with a looping step buffer plus a bipolar
> ENTROPY control (erode / loop / grow). Per-step-always-random survives only
> as the entropy = +1 extreme. Do not implement this spec.

A narrow correction to the modulation lane's random source so that STEP mode at
the sample&hold end of SHAPE produces a genuinely **random note per step** — a
melody — instead of a single note repeated for the whole cycle. This is a
spec-conformance fix to the M1 engine, discovered while listening to M3 capture
renders; it is not a new feature and adds no parameters.

Fork: `github.com/mcbronkowitch/spotymod`, local at
`c:\Users\bernd\Documents\AI\Spotykach`.

## Goal

Make the PITCH lane (and every lane, uniformly) able to generate melodic
sequences of random pitches under STEP mode. Today the two ends of the SHAPE
knob give only: (a) waveform **contours** (sine/triangle/ramp/pulse) that, when
stepped, walk the scale up and down like a cheap arpeggiator; and (b) at the
sample&hold end, a value that is redrawn **once per cycle**, so every step in a
cycle is the *same* note. Neither is a melody. After this fix, STEP + high SHAPE
yields an independent random note on each fired step, and PROBABILITY thins it
to taste.

## Root cause

The master spec (`2026-07-10-spotykach-modulation-first-synth-design.md`, lines
110–119) defines SHAPE as a morph `sine → triangle → ramp → pulse →
sample&hold random`, and STEP mode as "output quantized to clock subdivisions
(sample & hold) — **waves become sequences**." The intended reading: S&H in STEP
mode is a **random sequence** (a fresh random value per step).

The implementation redraws the S&H value only on cycle wrap:

- `engine/mod/lane.cpp:83` — `_sh_cycle = _rng.next_bipolar();` runs inside the
  `if (wrapped)` block, i.e. once per cycle.
- `engine/mod/lane.cpp:58-63` — `_compute_raw()` feeds `_sh_cycle` into
  `shape_value(ph, shape, _sh_cycle)` (`engine/mod/waveforms.h:16-28`).
- `engine/mod/lane.cpp:65-73` — `_on_boundary()` latches `_target =
  _compute_raw()` on each fired step, but `_sh_cycle` has not changed within the
  cycle, so every step in a cycle reads the same S&H value.

So at SHAPE→1 in STEP mode, a cycle of N steps produces N identical notes. That
is the bug.

## The change

One behavior: **in STEP mode, the sample&hold value redraws on each *fired*
step; in FLOW mode it stays one value per cycle (unchanged).**

- Redraw only on a **fired** step (inside `_on_boundary()` when the dice pass),
  so a thinned/held step keeps holding its previous pitch — preserving the
  "suppressed trigger = hold" drone rule and the M3 capture freeze semantics.
- FLOW mode is untouched: it is a continuous output, and one held random value
  per cycle is the correct continuous S&H behavior.
- The change lives in `ModLane` (the shared generator), so it applies to all
  five lanes uniformly. The PITCH lane is where it matters musically; a
  random-per-step SIZE/MOTION/LEVEL in STEP mode is equally valid and desirable.
- No new parameter, no PITCH special-casing, no mode flag. The randomness is
  reached exactly as before — by turning SHAPE toward its S&H end — and blends
  in progressively across the pulse→S&H segment (`shape` ≈ 0.75→1.0).

### Behavior after the fix

| SHAPE region | FLOW | STEP |
|---|---|---|
| sine/triangle/ramp/pulse (0..~0.75) | continuous contour LFO | stepped contour = deliberate arpeggio/sequence |
| sample&hold (~0.75..1) | one random value held per cycle (unchanged) | **random note per fired step = melody (fixed)** |

"Fewer notes / more pauses" comes from PROBABILITY (fewer steps fire) as the
master spec intends; with a plucky voice envelope a non-retriggered step decays
to silence, reading as a rest. No rest/gate-off behavior is added (an explicit
Option-A decision from brainstorming).

## Determinism & ripple

- **Still fully deterministic** (RNG-seeded, no time/global state). The
  scenario → bit-identical-WAV invariant continues to hold; the *bytes* of any
  scenario that reaches the S&H region in STEP mode change, because the note
  sequence is now genuinely per-step.
- **Pinned demo WAVs re-render.** At minimum `demo_step_melody.json` (SHAPE
  automated to 0.9 at t=8 s) now plays a random melody rather than a repeated
  note; regenerate any committed/expected renders that reach the S&H region.
- **M3 capture is unaffected in logic.** Capture freezes whatever the lane
  plays; a per-step random melody is exactly the kind of phrase capture exists
  to freeze. The M3 capture tests configure `shape = 0.75` (the pulse/S&H
  boundary, zero random weight) and so are unmoved.
- **EVOLVE is structurally independent** — it walks `_ev_phase`/`_ev_shape`/
  `_ev_rate`, not the S&H value — so its behavior is unchanged, though a STEP+S&H
  lane under EVOLVE will now wander a random melody rather than a single note.

## Module changes

All in `namespace spky`; no heap, no allocation in the audio path, no libDaisy.

| Module | Change |
|---|---|
| `engine/mod/lane.cpp` | In `_on_boundary()`, when the dice pass in STEP mode, redraw the S&H value before latching `_target` (a fresh `_rng.next_bipolar()` per fired step). Keep the per-cycle redraw path for FLOW. The exact placement (reuse `_sh_cycle` vs. a dedicated `_sh_step` field, and whether to drop the now-redundant wrap redraw in STEP mode) is an implementation detail for the plan, provided the observable behavior above holds and the RNG stream stays deterministic. |
| `engine/mod/lane.h` | Only if a dedicated hold field is added; otherwise unchanged. |

No change to `SuperModulator`, `Part`, `Instrument`, `CaptureLoop`, or the host.

## Testing

doctest, desktop, as established:

- **STEP + S&H is a random sequence:** a lane at `shape = 1.0`, STEP, probability
  1 → the per-step `target()` values across one cycle are **not all equal** (at
  least K distinct values over N steps), and the sequence is deterministic for a
  fixed seed (two identically-seeded lanes match exactly).
- **FLOW + S&H unchanged:** a lane at `shape = 1.0`, FLOW → the S&H value is held
  constant across a cycle and redraws once per wrap (guards against regressing
  the continuous case).
- **Held/thinned step still holds:** STEP + S&H + probability < 1 → a suppressed
  step does not change `target()` (no fresh random on a missed step).
- **Determinism invariant:** identical scenario → bit-identical WAV (existing
  invariant, re-confirmed on a scenario that reaches the S&H region).
- Update any existing test that asserts per-cycle S&H equality across STEP-mode
  steps (e.g. in `tests/test_step.cpp` / `tests/test_lane.cpp` if present).

## Master-spec touch-up

Add a clarifying half-sentence to the master spec's SHAPE / STEP description
(lines ~111 / ~118-119) that S&H under STEP redraws **per step** (a random
sequence), so the intended behavior is unambiguous. No other spec changes.

## Demos

Re-render the two captured-melody demos with proper melodic settings so they
sound like melodies:

- `capture_pentatonic.json` / `capture_duet.json`: STEP mode, SHAPE toward the
  S&H end for random notes, lower PROBABILITY for space, plucky voice envelopes
  so gaps read as rests; the slow voice is no longer a triangle/FLOW pitch
  sweep.

## Non-goals (YAGNI)

- No rest / gate-off mode (thinned steps still hold; brainstorming Option A).
- No new parameters, no separate "melody" mode, no per-lane special-casing.
- No change to the contour shapes or the SHAPE morph curve.

## Acceptance criteria

- A lane at `shape = 1.0`, STEP, probability 1 produces a per-step sequence of
  distinct random pitches (not one repeated value), deterministic per seed.
- FLOW + S&H still holds one value per cycle.
- A thinned STEP + S&H step holds its previous pitch (no new random on a miss).
- `engine/` still compiles with no libDaisy include; all new unit tests pass;
  existing tests pass or are updated to the corrected behavior.
- Bit-determinism invariant holds; pinned demo renders regenerated.
- The re-rendered `capture_pentatonic` / `capture_duet` play random melodies
  with rests, not arpeggios or repeated notes.

# Spotykach Entropy Sequencer — Looping Melodies with a Bipolar Entropy Control

Replaces the per-step sample&hold fix
(`2026-07-12-spotykach-per-step-sample-hold-design.md`, now superseded): pure
per-step randomness produced unusable "random number generator" melodies ~95%
of the time — no repetition, unmusical leaps, no arc. This design gives every
lane a **looping step buffer** (Turing-Machine principle) and one **bipolar
ENTROPY parameter** that replaces EVOLVE: center = the melody loops, positive =
it mutates and grows, negative = it erodes step by step toward a single note.

Fork: `github.com/mcbronkowitch/spotymod`, local at
`c:\Users\bernd\Documents\AI\Spotykach`.

## Goal

STEP mode at the S&H end of SHAPE should play a **melody**: recognizable
(it repeats), musical (it moves in lines, anchored around the root), and
performable (one control moves it between "one note", "locked loop" and
"living variation"). The control is a **process**, not a state: holding it
off-center changes the melody over time; returning to center freezes whatever
it has become. This also finally honors the master spec's LOOP contract
("deterministic, every cycle identical"), which both the old per-cycle S&H
redraw (FLOW) and the superseded per-step redraw (STEP) violated.

## Decisions (from brainstorming, 2026-07-12)

- The failures to fix are **all three**: no repetition, unmusical jumps, no
  tension arc.
- Control model: **process (entropy knob)**, not a deterministic
  complexity-position. Not reversible by design — turning back to center keeps
  the *current* state, not the original melody.
- Dice rules: **random walk + tonic gravity combined** — mutations draw small
  intervals from the old value, weighted toward 0 (= root / base value).
- Hardware access: **panel switch 2 becomes ERODE / LOOP / GROW** (three
  detents on a continuous engine parameter). Continuous ENTROPY stays fully
  automatable via scenario/CV.

## Architecture

### 1. Loop buffer

Each `ModLane` holds a fixed step buffer `float _seq[kSeqSlots]`
(`kSeqSlots = 32`, static, no heap), indexed by the step-in-cycle. At the S&H
end of SHAPE the lane no longer draws a fresh random value; it **reads the
buffer**:

- **STEP mode:** `_seq[step]` — the sequence loops by construction.
- **FLOW mode:** `_seq[0]` — one value per cycle, now loop-stable across
  cycles instead of redrawn per wrap (`_sh_cycle` and the per-wrap redraw at
  `engine/mod/lane.cpp:121` are removed).

The buffer is pre-filled at `init()` from the lane's seeded RNG: **a melody
exists from the first cycle**, deterministic per seed. Cycles with more steps
than `kSeqSlots` wrap the index (`step % kSeqSlots`).

### 2. ENTROPY replaces EVOLVE

`set_evolve(float 0..1)` becomes `set_entropy(float -1..+1)`. One parameter,
one sign convention, uniform across all five lanes and all SHAPE zones:

- **0 — LOOP.** Nothing mutates: the buffer repeats exactly, and the EVOLVE
  random walk (`_ev_phase/_ev_shape/_ev_rate`) is frozen where it stands.
- **> 0 — GROW.** On each **fired** step, with probability proportional to
  entropy, that slot mutates: `new = old + walk`, where `walk` draws small
  intervals often and leaps rarely, biased mildly toward 0 (tonic gravity) so
  melodies stay anchored. At +1 virtually every fired step mutates — the old
  "fresh random per step" becomes the extreme end of the range, not the only
  behavior. The existing EVOLVE walk on phase/shape/rate runs with strength =
  entropy (in the contour zone of SHAPE this remains the audible effect).
- **< 0 — ERODE.** Same mutation dice (probability ∝ |entropy|), but the
  mutation pulls the slot value a fixed fraction **toward 0**. The melody
  erodes cycle by cycle to the root until a single note remains. The EVOLVE
  walk decays back toward neutral (a per-part "settle", same spirit as the
  global TAP-hold settle).

Exact walk weights, gravity strength, erosion fraction and the mutation-
probability curve are tuned by ear on renders; the spec fixes the behavior,
not the constants.

### 3. Preserved rules

- **Mutation only on fired steps.** A PROBABILITY-suppressed step holds both
  its output *and* its buffer slot — the "suppressed trigger = hold" rule and
  M3 capture freeze semantics are untouched.
- **Deterministic.** All draws go through the lane's seeded `_rng`; identical
  scenario → bit-identical WAV continues to hold.
- **No music theory in the lane.** "Root" is simply value 0 (the base value of
  any target); scale quantization stays downstream in the synth engine. The
  same machinery gives non-PITCH lanes looping/eroding/growing patterns of
  SIZE, MOTION, LEVEL — uniform, no PITCH special-casing.
- **Capture unchanged.** Capture freezes whatever the lane plays; a loop
  buffer under entropy is exactly the kind of phrase worth freezing. Captured
  loops still never mutate.
- The SHAPE morph and the pulse→S&H blend (shape ≈ 0.75→1.0) are unchanged;
  the buffer value replaces `_sh_cycle` as the S&H operand of
  `shape_value()`.

### 4. Hardware mapping

Panel switch 2 (per side) is remapped:

| Switch 2 | Old              | New                       |
|----------|------------------|---------------------------|
| Left     | LOOP             | **ERODE** (entropy ≈ −0.4) |
| Middle   | EVOLVE subtle    | **LOOP** (entropy 0)      |
| Right    | EVOLVE strong    | **GROW** (entropy ≈ +0.4) |

Middle = frozen matches the ON-OFF-ON hardware (off = nothing changes). The
detent values are tuning constants. The semantics read uniformly across all
SHAPE zones: left = calms down, middle = freezes, right = lives. Continuous
entropy remains reachable via scenario automation (and later CV) without any
redesign.

## Module changes

All in `namespace spky`; no heap, no allocation in the audio path, no libDaisy.

| Module | Change |
|---|---|
| `engine/mod/lane.h` | `float _seq[kSeqSlots]` buffer; `set_entropy(float)` replaces `set_evolve(float)`; `_entropy` field replaces `_evolve`; `_sh_cycle` removed; private mutation helper(s). |
| `engine/mod/lane.cpp` | `init()` pre-fills `_seq` from the RNG. `_compute_raw()` reads `_seq[slot]` instead of `_sh_cycle`. `_on_boundary()` runs the mutation dice before latching on fired steps. Cycle-wrap: remove the `_sh_cycle` redraw; gate the EVOLVE walk on `entropy > 0`, decay it toward 0 when `entropy < 0`. |
| `engine/mod/supermod.*` / `engine/instrument.*` | Plumb `set_entropy` through in place of `set_evolve` (bipolar range). |
| `host/render` | Scenario key `evolve` (0..1) becomes `entropy` (−1..+1); update bundled scenarios. |
| firmware shell (M6) | Switch-2 mapping table updated to ERODE/LOOP/GROW — noted here, lands with M6. |

No change to `CaptureLoop`, `waveforms.h` (signature keeps taking the S&H
operand), or the voice/engine layer.

## Testing

doctest, desktop, as established:

- **Loop invariant:** STEP + S&H, entropy 0, probability 1 → cycle N and
  cycle N+1 produce identical step sequences; FLOW + S&H holds one identical
  value across cycles.
- **Melody from the start:** freshly init'd lane at shape 1.0, STEP → the
  first cycle's steps are not all equal (buffer pre-filled), deterministic per
  seed (two identically seeded lanes match exactly).
- **Grow:** moderate positive entropy → over M cycles some slots change while
  others persist; per-mutation deltas are bounded (walk, not uniform redraw).
  At entropy +1 nearly every fired step differs from the stored value.
- **Erode:** from a random buffer, entropy −1 → all slot values converge to 0
  within K cycles (monotonically decreasing |value| per mutated slot).
- **Hold rule:** probability < 1 → a suppressed step changes neither
  `target()` nor its buffer slot.
- **EVOLVE walk gating:** entropy 0 freezes `_ev_*`; negative entropy decays
  them toward 0.
- **Determinism invariant:** identical scenario → bit-identical WAV,
  re-confirmed on a scenario exercising positive and negative entropy.
- Update/replace the superseded per-step S&H tests (per-step-always-random is
  now only the entropy = +1 extreme).

## Master-spec touch-ups

In `2026-07-10-spotykach-modulation-first-synth-design.md`:

- **LOOP / EVOLVE paragraph** → **ENTROPY**: bipolar, erode/loop/grow process
  semantics; S&H zone = loop buffer with mutation, contour zone = EVOLVE walk
  with settle.
- **Panel switches table:** switch 2 = ERODE / LOOP / GROW.
- **SHAPE / STEP description:** S&H under STEP plays the looping step buffer
  (a melody), mutated by ENTROPY — not a per-cycle or per-step free random.

## Demos

Re-render melody demos to showcase the arc: start at LOOP (repeating phrase),
automate entropy positive (melody grows/varies), then negative (erodes toward
one note). `capture_pentatonic.json` / `capture_duet.json` re-rendered with
looping melodies + PROBABILITY rests; `demo_step_melody.json` becomes the
entropy showcase. Pinned WAVs that reach the S&H region regenerate.

## Non-goals (YAGNI)

- No deterministic "complexity position" mode (process model chosen; turning
  back to center is intentionally not an undo).
- No "new melody" gesture — a fresh phrase is played in by briefly cranking
  entropy up; reseeding stays an init/scenario concern.
- No scale awareness inside `ModLane`; no per-lane special-casing.
- No editable mutation constants as user parameters (tuned by ear, fixed).
- No rest/gate-off mode (unchanged from the superseded spec).

## Acceptance criteria

- Entropy 0: a STEP + S&H lane loops its melody exactly, cycle after cycle;
  FLOW + S&H holds a stable per-cycle value.
- Positive entropy audibly *varies* the melody over cycles (walk-sized
  changes, tonally anchored), up to near-full randomness at +1.
- Negative entropy audibly *simplifies* the melody over cycles down to a
  single repeated root note at sustained −1.
- Suppressed steps still hold (note and slot); capture behavior unchanged.
- `engine/` compiles with no libDaisy include; all new unit tests pass;
  superseded tests updated.
- Bit-determinism invariant holds; pinned demo renders regenerated; the
  re-rendered demos play looping, evolving melodies — not arpeggios, repeated
  notes, or white-noise note salad.

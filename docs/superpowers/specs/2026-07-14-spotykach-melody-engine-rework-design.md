# Spotykach Melody Engine Rework — Marbles-style MELODY Knob, Switchable Phrase Principles, Gate Layer

Supersedes the capture sequencer
(`2026-07-12-spotykach-capture-sequencer-design.md`) and the per-step
sample&hold fix (`2026-07-12-spotykach-per-step-sample-hold-design.md`), and
reworks the negative half of the entropy sequencer
(`2026-07-12-spotykach-entropy-sequencer-design.md`): the GROW side and the
looping step buffer stay; ERODE, the PROBABILITY knob and the whole capture
system are removed.

The VCV Rack test showed the generative melodies were unusable. Two root
causes: (1) the step buffer is seeded from **independent** random values per
slot (`_seq[i] = _rng.next_bipolar()`) — a quantized note salad even when it
loops cleanly; the entropy spec named "moves in lines, anchored around the
root" as the *goal* but the code never produced it. (2) The only melodic
controls were a probability knob (per-step dice) and an entropy knob whose
negative side merely eroded to one note — neither ever produces a genuinely
*new, structured* phrase.

Fork: `github.com/mcbronkowitch/spotymod`, local at
`c:\Users\bernd\Documents\AI\Spotykach`. Engine + desktop host in this
milestone; hardware wiring lands with M6.

## Goal

One per-part knob — Mutable Marbles in spirit — that a player can hold to move
the melody between three states and release to freeze wherever it landed:

- **Centre — LOOP:** the phrase repeats exactly.
- **Right of centre — GROW:** free variation of the *existing* melody; the
  further right, the more it drifts.
- **Left of centre — RENEW:** increasingly *new* melodies — rhythmically new,
  not eroded — built by a real melodic generator ("our method"), not random.

Melodies must sound composed, not diced: contour lines instead of independent
random notes, and **motivic repetition** (the thing that makes a phrase
memorable). The generator's *principle* is switchable, so the character of a
fresh phrase is a performable choice.

## Decisions (from brainstorming, 2026-07-14)

- **Remove** the capture sequencer (record/replay of a melody) and the
  **PROBABILITY** knob entirely. Rhythm now lives in the melody itself.
- **Rhythm is part of the melody:** each step carries a **gate** (note / rest)
  alongside its pitch. "Rhythmically new" is literal — RENEW regenerates the
  gate pattern too.
- **One continuous process knob**, symmetric *at the interface* — one bipolar
  control, centre-detent, release-to-freeze — but the two sides are **different
  processes at different granularities**, not inverse operations. GROW varies
  the *existing* pitches per slot within the cycle; RENEW replaces whole units
  per cycle wrap. Neither is a return to the original (same "process, not state"
  model as the entropy spec — releasing keeps the current phrase; RENEW is not
  an undo of GROW). "Mirror" describes the knob feel, not the mechanism.
- **"Our method" = motivic phrase construction:** a fresh phrase is a
  gravity-anchored contour walk assembled from short **motifs** that repeat and
  vary — never 32 independent draws. The same generator seeds the melody at
  init *and* supplies RENEW.
- **Principle is switchable** (default **Two motifs**). v1 catalogue of five:
  Two motifs, One motif + variation, Hierarchical (cell→motif→phrase),
  Question & answer, Ostinato.
- **Change granularity differs by side:** GROW works **per slot** (small pitch
  walk from the old value — the existing GROW, pitch only); RENEW works **per
  renewal unit** (regenerate a whole motif — or a Q&A pair — via the current
  principle, its repetitions follow) so structure survives regeneration.
- **Live DENSITY control** thins/fills the gate pattern deterministically
  (musical priority, no dice), non-destructively over the generated pattern.
- **Scope by lane:** only the **PITCH** lane gets the rich system (five
  principles, motif structure, gates, density). The other four lanes keep the
  same knob as a plain **LOOP / GROW / RENEW** process on their step buffer
  (RENEW = a fresh contour walk, no motif form, no gates). One paradigm across
  all lanes; only PITCH is "rich".

## Naming

- Engine parameter `set_entropy(float −1..+1)` is renamed **`set_variation`**
  (uniform across all five lanes): negative = RENEW, 0 = LOOP, positive = GROW.
  The ERODE meaning of the negative half is gone.
- The PITCH-side hardware knob is labelled **MELODY** on the faceplate overlay.

## Architecture

All in `namespace spky`; no heap, no allocation in the audio path, no
libDaisy; every draw goes through the lane's seeded `_rng` so the
bit-determinism invariant holds.

### 1. Melody data model (PITCH lane)

The active phrase length is the steps-per-cycle count `_steps` (STEP mode);
the buffer capacity stays `kSeqSlots = 32`. Each slot now carries two values:

- `float _seq[kSeqSlots]` — pitch (bipolar, quantized downstream; unchanged).
- `bool  _gate[kSeqSlots]` — note (true) / rest (false). **New.**

Plus a compact **phrase layout** (POD, caller-owned, no heap) so RENEW can
regenerate a unit and refresh its repetitions, and so the loop, RENEW and
DENSITY share one description of the phrase:

- `uint8_t _motif_id[kSeqSlots]` — which motif each slot belongs to.
- `PhraseLayout _layout` — `{ uint8_t motif_len, tail_len, inst_count,
  motif_count; }`. All instances of a motif are **exactly `motif_len` slots**,
  so slot-copy repetition is exact; a trailing `tail_len` (`0..motif_len−1`)
  slots form a separate tail motif with its own id.

**Phrase length & motif sizing.** The generator works on
`n = min(_steps, kSeqSlots)` slots (the 32-slot buffer is the hard cap; `_steps`
above 32 wraps into it via `_sh_slot` — there the metric grid (§4) refers to
**buffer** position, not bar position: the first `_steps − 32` slots sound twice
per cycle, the second time off their nominal weights. Accepted: above the buffer
cap is an edge range, not the played sweet spot). From a principle-specific target length
`Lt` it derives `inst_count k = max(1, round(n / Lt))`, `motif_len L = n / k`
(integer), `tail_len r = n − k·L`. The phrase is `k` equal instances of length
`L` plus an optional `r`-slot tail. The principle's arrangement (A A B A,
cell→motif, …) is rolled over the **actual** `k` and **degrades gracefully** when
`k` is small (k=2 Two-motif → A B). Prime/awkward `_steps` therefore always yield
a valid layout (e.g. steps 7, Lt 4 → k 2, L 3, tail 1); the invariant
`k·L + r == n` always holds.

On the four non-PITCH lanes `_gate` is always true and the motif map is unused;
those lanes fill `_seq` with a plain contour walk.

### 2. The MELODY knob — `set_variation(float −1..+1)`

Replaces `set_entropy`. One sign convention, uniform across lanes:

- **0 — LOOP.** Nothing mutates; the buffer repeats exactly and the contour
  walk (`_ev_phase/_ev_shape/_ev_rate`) is frozen where it stands. (Unchanged
  from entropy 0.)
- **> 0 — GROW.** On each **effectively-gated (§4) & fired** step, with probability ∝
  variation² (squared for fine control near LOOP), the slot's **pitch** takes a
  small walk from its old value — cubed draw (small intervals common, leaps
  rare), width opening with variation, mild tonic gravity toward 0. Gates are
  **untouched**: the groove stays put; only pitch drifts. This is the current
  GROW mutation (`_mutate_slot` positive branch) restricted to pitch. The
  per-cycle contour walk on phase/shape/rate runs with strength = variation, as
  today.
- **< 0 — RENEW.** On each cycle wrap, **every renewal unit rolls its own
  dice**: with probability ∝ variation² the unit is **regenerated** (pitch +
  gate) with the current principle, writing all of the unit's slots in place.
  (Per-unit dice, not one pick per wrap — only that delivers "a new phrase per
  cycle" at −1, and just left of centre it trickles without re-hitting the
  same unit.) A renewal unit is
  principle-defined: for Two-motif / One-motif / Ostinato it is a single motif
  (its sibling repetitions are rewritten with it, so they stay identical); for
  **Question & answer** it is the question+answer pair regenerated **together**
  (so the answer still resolves to the root); for **Hierarchical** it is a motif
  whose cell lineage is regenerated inside it (nesting preserved). Regeneration
  reads absolute slot positions from `_layout`/`_motif_id`, so rests land on the
  phrase's real metric weights (§4) — consistent with init and DENSITY, not on
  motif-relative positions. At sustained −1 nearly every unit is replaced each
  cycle → a new phrase per cycle; just left of centre, a unit changes now and
  then. The arrangement (which motif sits where) is kept; a full-phrase
  regeneration (new arrangement + all motifs) happens only on an explicit "new
  phrase" gesture (§5) or at init. The per-cycle contour walk
  (`_ev_phase/_ev_shape/_ev_rate`) **decays toward neutral** per wrap under
  RENEW, exactly as the old ERODE side did — fresh phrases are presented
  un-warped (RENEW's job is fresh structure, not accumulated drift).

Non-PITCH lanes: GROW as above (they have no gates, every step fires); RENEW
regenerates the whole `_seq` as one fresh contour walk (no motif structure, no
gates).

**Live step-count changes (PITCH).** `_steps` comes from a knob and can change
at any time, invalidating the derived layout. On the PITCH lane, `_regen_pending`
is set **only when the effective length `n = min(_steps, kSeqSlots)` actually
changes** — repeated `set_step` calls with the same count (knob jitter) and any
move within `_steps ≥ 32` (e.g. 33 → 40, n stays 32) must not trigger a regen.
The full phrase is then regenerated (via
`generate_phrase`, current principle) at the **next cycle wrap** — not
mid-cycle, so the running bar stays intact and the change point is
deterministic. Deliberate consequence: changing the step count **discards the
current (possibly GROW-drifted) phrase** — release-to-freeze promises survival
across the MELODY knob, not across a length change; a new length needs a new
layout, and a fresh phrase beats a corrupted one. The non-PITCH lanes need no
regeneration: their contour walk is
length-agnostic and simply re-windows over `min(_steps, kSeqSlots)`.

**GROW × RENEW interaction (two deliberate, audible behaviours).** These follow
from the per-slot / per-unit split and are intended, not accidents:

1. **RENEW re-imposes unison on GROW-diverged repetitions.** GROW mutates each
   fired slot independently, so the sibling instances of one motif (e.g. the
   three *A*s of an *A A B A*) gradually diverge — this is the wanted variation.
   RENEW rewrites *all* siblings of a unit from a single regenerated motif, so
   they snap back to identical. Crossing centre from GROW into RENEW therefore
   audibly resets a drifted repetition to a clean unison. This is desired
   (RENEW's job is fresh *structure*, which means coherent repetition); it is
   not smoothed or cross-faded.
2. **GROW never touches rest-slot pitch.** GROW mutates only
   **effectively-gated & fired** slots (§2, `> 0`; §4), so a slot that is
   currently a rest — generated *or* density-masked — keeps its pitch. When
   DENSITY (§4) later fills that gate back in, the note
   sounds at its original generated value, not a grown one. Desired: GROW drifts
   only what is *sounding*; DENSITY reveals the phrase's structural pitches
   unchanged. (Full renewal of a rest slot's pitch happens on RENEW or
   `new_phrase`, not GROW.)

### 3. Phrase generator — `PhraseGen` (PITCH lane)

Deterministic, allocation-free, no virtual dispatch (embedded-friendly): a
`Principle` enum plus free functions operating on caller-owned buffers.

```
enum class Principle { TwoMotif, OneMotif, Hierarchical, CallResponse, Ostinato };

struct PhraseLayout { uint8_t motif_len, tail_len, inst_count, motif_count; };

// Fills pitch[0..n), gate[0..n), motif_id[0..n) for n = min(steps, kSeqSlots),
// derives the motif sizing (§1) and writes it into `out`. Deterministic per rng.
void generate_phrase (Principle, Rng&, int steps,
                      float* pitch, bool* gate, uint8_t* motif_id,
                      PhraseLayout& out);

// Regenerates renewal unit `unit` in place across ALL its slots (sibling
// repetitions, and for CallResponse its paired answer). Reads the whole phrase
// for neighbour/metric context via `layout` + `motif_id`; uses absolute-position
// metric weights, so gates stay consistent with generate_phrase. `unit` in
// [0, layout.motif_count).
void regenerate_unit (Principle, Rng&, const PhraseLayout& layout,
                      const uint8_t* motif_id, int unit,
                      float* pitch, bool* gate);
```

Shared building block — **contour walk**: a motif's pitches are a bounded
random walk (cubed step draw, tonic gravity), *not* independent draws, so a
motif is a line. Shared **gate rule**: each principle places rests on weak
metric positions with a principle-specific density character; every slot's
metric weight (down-beat strongest, then binary subdivision) is derived from
its position for the density mask (§4).

The five principles:

1. **Two motifs (default).** Generate motifs A and B (each a contour walk with
   a rhythm), arrange them in a rolled form — AABA / ABAB / AAB — for
   question-answer phrasing with strong repetition.
2. **One motif + variation.** One motif, repeated as A A' A'' with light
   per-repetition pitch variation. Calm, meditative, maximally recognizable;
   closest to today's engine.
3. **Hierarchical.** A 2–4-slot cell repeats into a motif, motifs into the
   phrase — the richest nested repetition.
4. **Question & answer.** Motif A ends *away* from the root (open); the answer
   phrase resolves *to* the root. A sung, dialogic arc.
5. **Ostinato.** A strong, repetitive gate pattern with minimal pitch movement
   — groove/hypnotic; leans on the new gate layer rather than melodic contour.

### 4. Gate layer + DENSITY

**Scope: STEP mode only.** The gate layer, DENSITY and the motif structure are
defined **only in STEP mode** (they index `_seq`/`_gate` by `_cur_step`). In
FLOW mode the PITCH lane is a single continuous slot as today (`_sh_slot()==0`):
no gates, no motif map, no DENSITY, no rests — `generate_phrase` is not run,
`_gate` is unused, and the lane produces its continuous shaped value every
sample. FLOW's loop invariant (§Testing) is unchanged.

**`_frozen` semantics change (deliberate).** Today `_frozen` is set by the
per-boundary PROBABILITY dice. With PROBABILITY removed:

- **STEP:** `_frozen = !effective_gate(slot)` at a step boundary — a rest
  holds the previous `_target` and dims the LED, exactly the
  visual/behavioural role the old dice-freeze had. The trigger is gated by the
  effective gate (below), never by a dice roll.
- **FLOW:** there is no per-step gate, so **FLOW no longer freezes at all** —
  removing PROBABILITY removes FLOW's only freeze source. This is an intended
  behaviour change: a FLOW lane now always runs its continuous value.

- **Effective gate — one definition, used everywhere.**
  `effective_gate(slot) = _gate[slot] && density_pass(slot)`. Fire/onset, the
  `_target` latch, `_frozen`, the LED and GROW eligibility (§2) all read the
  **effective** gate: a density-masked step behaves exactly like a generated
  rest while masked — no onset, no latch (the CV never moves without a note),
  `_frozen` true, LED dimmed, no GROW mutation. DENSITY still never edits
  `_gate` (below), so unmasking restores the phrase exactly.
- **Gate semantics.** An effectively-gated step triggers a note (sets
  `_fired`, latches the pitch as today). A rest produces **no onset**
  — a real rest. Pitch continuity: on a rest the lane holds its previous
  `_target` (for CV), it simply does not fire. This replaces the old
  "PROBABILITY-suppressed = hold" path.
- **DENSITY (`set_density(float 0..1)`, live, PITCH lane).** A non-destructive
  mask over the generated gate pattern: DENSITY sets a threshold on slot metric
  weight — weak (off-beat) gates drop out first as density falls, fill back in
  as it rises. Deterministic and metric-ordered (no dice, no salad). DENSITY 1
  = the full generated pattern; low = only strong-beat notes remain. It never
  edits `_gate`; it gates the trigger at playback, so turning it back restores
  the phrase exactly.

### 5. New-phrase gesture

A one-shot: regenerate the **whole** phrase (fresh arrangement + all motifs)
with the current principle, then loop it. Distinct from RENEW's continuous
motif churn — it is the "audition a new melody, then leave the knob at LOOP"
workflow, and finally gives the explicit new-melody trigger the entropy spec
deferred. Engine hook: `new_phrase()` on the PITCH lane (and its plumbing
through SuperModulator / Instrument).

**Timing.** `new_phrase()` sets the same pending-regen flag as a step-count
change (§2); the regeneration runs at the **next cycle wrap** — the running bar
finishes intact and the change point is deterministic (accepted trade-off: at
slow rates the audition lags up to one cycle). Multiple taps before the wrap
coalesce into one regen, as does a coinciding pending step-change regen — one
flag, one regeneration. In FLOW the flag simply stays pending
(`generate_phrase` never runs in FLOW, §4); the fresh phrase materialises at
the first STEP-mode cycle wrap.

### 6. Hardware mapping (concept; wiring in M6)

Freed by the removals: the **CTRL_POS** pot (was PROBABILITY), the **SEQ pad**
(was capture), and **switch 2** (was ERODE/LOOP/GROW — the knob owns that axis
now).

| Element (per side) | Function |
|---|---|
| CTRL_POS pot | **MELODY** — bipolar, centre-detent: RENEW ← LOOP → GROW |
| SEQ pad · tap | **Cycle principle** — LED ring shows the active principle |
| SEQ pad · hold + CTRL_POS | **DENSITY** — continuous, soft-takeover, ring shows density |
| ALT + SEQ pad | **New phrase now** (§5) |
| Switch 2 | free (reserve) |

The SEQ cycle-button carries all five principles where a 3-position switch
could only show three; tap (fires on release, short) = principle, hold =
density edit — the firmware's existing release-based tap/hold model, no
collision. The ring's capture step-pattern display (with playhead) is removed
with capture; the ring gains a brief principle-indication overlay on SEQ tap.

**Both pot roles get soft-takeover:** DENSITY on SEQ-hold (as above) *and*
MELODY on SEQ release — after a density edit the pot physically sits at the
density position, so MELODY re-engages only once the pot crosses its pre-hold
value. Otherwise releasing SEQ would slam variation to wherever the density
edit left the knob (e.g. a sudden RENEW at −0.8).

## Module changes

| Module | Change |
|---|---|
| `engine/mod/lane.h` | Add `bool _gate[kSeqSlots]`, `uint8_t _motif_id[kSeqSlots]`, a `PhraseLayout _layout`, a `_regen_pending` flag, a `Principle _principle` and a `_melodic` flag (set only on the PITCH lane). `set_entropy` → `set_variation`; add `set_density`, `set_principle`, `new_phrase`. Remove all capture fields (`_capture_loop`, `_replay`, `_play_slot`, `_rec_*`) and `set_probability`/`_prob`; drop the `_replaying()` guard in `kick()`. |
| `engine/mod/lane.cpp` | `init()` seeds the melody via `PhraseGen::generate_phrase` (PITCH, filling `_layout`) or a contour walk (others), not per-slot `next_bipolar`. `_on_boundary`: fire gated by the **effective gate** (§4), no probability dice; GROW mutates pitch only (effectively-gated & fired slots). Cycle-wrap: RENEW rolls **per-unit dice** (each unit regenerated with probability ∝ variation² via `regenerate_unit`; non-PITCH: whole walk), and the `_ev_*` walk decays toward neutral at negative variation. `set_step` on the PITCH lane sets `_regen_pending` **only when `min(_steps, kSeqSlots)` changes**; `new_phrase()` sets the same flag; a pending regen runs at the next STEP-mode cycle wrap. Remove `_record_slot`/`_replay_step`/`_replaying` and the erode branch of `_mutate_slot`. `process()` applies the DENSITY mask to the trigger. |
| `engine/mod/phrase_gen.h` | **New.** `Principle` enum + `PhraseLayout` + `generate_phrase` / `regenerate_unit`; contour-walk + absolute metric-weight helpers, motif-sizing (`k`/`L`/tail) derivation. Header-only, deterministic, no heap. |
| `engine/mod/capture.h` | **Removed.** |
| `engine/mod/super_modulator.*` | Drop `probability`/capture plumbing; rename entropy→variation; forward `set_density`/`set_principle`/`new_phrase` to the PITCH lane. |
| `engine/instrument.*` | Public API: remove `set_probability`, `capture_now`, `set_replay`, `replaying`, `loop_valid`; rename `set_entropy`→`set_variation`; add `set_density`, `set_principle`, `new_phrase`. |
| `host/render` | Remove `probability`, `capture_now`, `set_replay` scenario actions and `a_cap`/`b_cap` CSV columns; `entropy`→`variation`; add `density`, `principle`, `new_phrase` actions; add a gate/rest CSV column for the PITCH lane. Update bundled scenarios. |
| firmware shell (M6) | Control map above; capture gesture + ring step-pattern display removed. |

No change to `waveforms.h` (still takes the S&H operand), the quantizer, or the
voice/engine layer.

## Testing

doctest, desktop, as established:

- **Loop invariant:** variation 0 → cycle N and N+1 identical (pitch *and*
  gate); FLOW holds one stable value.
- **Melody from the start, structured:** a freshly-init'd PITCH lane at
  shape 1.0, STEP, Two-motif principle → the phrase shows motivic repetition
  (the motif map has repeats and repeated slots match), not 32 independent
  values; deterministic per seed.
- **GROW (pitch only):** moderate positive variation → some pitch slots change,
  bounded per-mutation deltas, **gates unchanged**; at +1 nearly every fired
  step's pitch differs.
- **RENEW (unit level):** negative variation → whole renewal units are replaced
  over cycles while the arrangement/motif map stays coherent; sibling
  repetitions of a regenerated motif remain identical to each other (structure
  preserved, not slot-wise salad). Per-unit dice: at sustained −1 **every** unit
  is replaced on every wrap (a new phrase per cycle); the `_ev_*` walk decays
  toward neutral over cycles. For **Question & answer**, after a unit regen
  the answer still resolves toward 0 (the pair regenerates together); for
  **Hierarchical**, the cell identity within the regenerated motif is preserved.
- **Layout across step counts:** `generate_phrase` at steps ∈ {5, 7, 11, 13, 32}
  yields a valid layout — `inst_count·motif_len + tail_len == min(steps, 32)`,
  every instance of an id the same length — deterministic per seed.
- **Live step change:** changing `_steps` on the PITCH lane regenerates the
  phrase at the **next cycle wrap** (not mid-cycle), deterministically; the
  non-PITCH lanes keep looping their walk. No regen when the effective length
  is unchanged: repeated `set_step` with the same count, and moves within
  `_steps ≥ 32` (e.g. 33 → 40), leave the phrase untouched. Regenerated gates
  use absolute metric
  positions (a regenerated unit's rests match what `generate_phrase` would place
  at those slots).
- **Gate/rest (STEP):** a rest slot produces no `_fired`/onset and holds pitch;
  `frozen()` is true on a rest boundary and false on a gated one. (This changes
  the RNG draw sequence vs. the old probability path, so pinned WAVs are
  re-rendered — the invariant is *behavioural* per gate, not bit-identical to
  pre-rework renders.)
- **FLOW after PROBABILITY removal:** a FLOW PITCH lane never freezes — `frozen()`
  stays false across many cycles at any variation; no gate/motif/DENSITY state is
  touched. Pairs with the FLOW loop invariant.
- **GROW × RENEW interaction:** (1) GROW at high variation diverges the sibling
  instances of a motif (their slots stop matching); a subsequent RENEW of that
  unit makes all its instances identical again (unison reset). (2) A slot held as
  a rest keeps its generated pitch across GROW cycles; raising DENSITY to fire it
  yields the original generated value, not a grown one. Both deterministic per
  seed.
- **DENSITY:** deterministic and metric-ordered — lowering density drops
  weak-beat gates first and is exactly reversible; never mutates `_gate`. A
  density-masked step behaves as a rest end-to-end: no onset, `frozen()` true,
  `_target` held, and **no GROW mutation** while masked (effective gate, §4).
- **Principles:** each of the five produces its signature (e.g. Ostinato =
  repetitive gate + near-static pitch; Question&answer = answer resolves toward
  0; One-motif = high repetition), deterministic per seed.
- **new_phrase:** replaces the whole phrase deterministically **at the next
  cycle wrap** and then loops; taps before the wrap coalesce into one regen; in
  FLOW the flag stays pending until the first STEP-mode wrap.
- **Non-PITCH lanes:** RENEW regenerates a contour walk with all gates on and
  no motif structure; GROW as today.
- **Determinism invariant:** identical scenario → bit-identical WAV, on a
  scenario exercising GROW, RENEW, density and a principle switch.
- Remove capture and per-step-probability tests; update entropy tests (GROW
  kept, ERODE gone, negative half is RENEW).

## Master-spec touch-ups

In `2026-07-10-spotykach-modulation-first-synth-design.md`:

- **Remove the PROBABILITY knob** from the macro surface and the lane signal
  chain (`[wavetable]→[probability]→…` becomes `[wavetable]→[gate]→…` on PITCH);
  CTRL_POS becomes **MELODY**.
- **Remove the entire Capture-sequencer section**; ALT+SEQ becomes the
  principle/new-phrase gestures; the ring's step-pattern+playhead display is
  removed.
- **ENTROPY paragraph → MELODY/VARIATION:** bipolar RENEW / LOOP / GROW; PITCH
  gets the phrase generator + gate layer + density; other lanes get the plain
  process.
- **Panel switches table:** switch 2 = free/reserve (was ERODE/LOOP/GROW).
- **LED section:** ring shows PITCH post-gate/post-density (frozen/rest dims as
  before); add the principle-overlay note; drop the capture display line.

## Demos

Re-render the melody demos to show the arc: init (structured looping phrase),
sweep MELODY right (pitch grows/varies, groove intact), sweep left (new motifs
appear, rhythm changes), tap **new phrase** a few times to audition, then leave
at LOOP. One demo per principle showing its signature. Add a DENSITY sweep
demo (full phrase → strong-beats-only → back). Retire `capture_*.json`;
`demo_step_melody.json` becomes the principle/variation showcase.

## Non-goals (YAGNI)

- No editable generator constants as user parameters (walk widths, gravity,
  gate densities are tuned by ear, fixed). Motif length and instance count are
  **derived** from `_steps` (§1), not user-set.
- No scale/music-theory logic inside `ModLane` or `PhraseGen` — "root" is value
  0; scale quantization stays downstream in the synth engine.
- No motif structure, gates or principles on the non-PITCH lanes.
- No capture/record/replay (removed) and no return-to-original undo (process
  model: RENEW is not reversible; new_phrase is a fresh phrase, not the old
  one).
- No probability/dice-based thinning (density is deterministic and
  metric-ordered).
- No per-motif independent RNG streams — one lane RNG, as today.

## Acceptance criteria

- Variation 0: a STEP PITCH lane loops its phrase exactly, pitch and gate,
  cycle after cycle.
- Positive variation audibly *varies the existing melody's pitch* over cycles
  (bounded walk, tonally anchored) with the rhythm held; up to near-random
  pitch at +1.
- Negative variation audibly introduces *new, structured* phrases over cycles —
  new motifs and new rhythm, coherent (motivic), not eroded and not salad;
  sustained −1 ≈ a new phrase per cycle.
- Each of the five principles produces a recognizably different phrase
  character; **Two motifs** is the boot default.
- DENSITY thins/fills the gate pattern smoothly, deterministically, reversibly.
- `new_phrase` yields a fresh, structured phrase that then loops.
- Capture and the PROBABILITY knob are gone from engine, host and API;
  `engine/` compiles with no libDaisy include; all new tests pass; superseded
  tests removed/updated.
- Bit-determinism invariant holds; pinned demo renders regenerated and play
  looping, evolving, *musical* melodies — not arpeggios, repeated notes, or
  note salad.

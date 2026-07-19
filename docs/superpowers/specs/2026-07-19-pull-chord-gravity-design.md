# PULL — chord gravity between the decks

- **Status:** designed, not implemented
- **Date:** 2026-07-19
- **Branch:** main

## Context

Since the chord layer (spec 2026-07-17 chord-layer), each part carries a
`ChordBuilder` behind its COLOR knob: at every trigger it latches up to four
chord-tone intervals around the part's melody root, and `apply()` re-voices
them per sample. Both parts already share the global scale and per-part root
via their `Quantizer`s.

What the instrument cannot do yet: make one deck's *melody* aware of the other
deck's *harmony*. Deck A can stack a chord, but deck B picks its notes from the
full scale mask (5–7 pitch classes) with no pull toward what A is sounding.
The musical ask: a chord-melody mode — one deck plays chords, the other plays
notes drawn toward (at full strength: exclusively from) that chord's tones.

## Decision

A new bipolar center knob, **PULL**, appended LAST in `gen_panel.py` (existing
.vcv patches keep their param ids — the CHOKE/FILT/TIDE convention). It sits in
the center duo column beside CHOKE/DRIFT.

- **Sign = direction, CHOKE convention:** negative (left) = A leads, B's notes
  are pulled onto A's chord; positive mirrored. Center = off.
- **Magnitude = probability:** |PULL| is the per-note chance of being pulled.
  Full deflection = every note is a chord tone. A small dead zone (~±0.03)
  around center makes 12 o'clock reliably off.
- **Off is structural:** at PULL 0 the follower's quantizer path is untouched —
  the same bit-identity guarantee style as CHOKE zone 0 and COLOR 0.

### The pull is a quantizer mask override

The follower's `Quantizer` gains a second, **absolute** pitch-class mask (the
scale mask stays root-relative; the gravity mask is checked mod 12 with no root
shift). When a note is bound, the quantizer quantizes to the gravity mask
instead of the scale mask.

- **Per-note decision at fire time.** On each PITCH-lane fire of the follower,
  a deterministically seeded PRNG (same family as the lane seeds — no global
  RNG, fully reproducible) draws once: below |PULL| the note is bound, else
  free. The decision latches until the next fire.
- **Bound notes follow the chord live.** Quantization already runs continuously
  at the control tick, and mask changes go through `on_change()` → the existing
  40 ms slew. A sustained bound note therefore *glides* onto the nearest tone
  of the new chord when the leader re-triggers — no special machinery.
- **Any octave.** The chord is a pitch-class set; the follower keeps its own
  register and arpeggiates across the lay. (Chosen over "exactly the leader's
  voicing", which would glue B into A's register.)
- **FREE mode is not exempt.** A bound note gets quantized to the chord mask
  even when the follower's quantizer mode is Free; free-rolled notes stay
  unquantized. Full PULL therefore harmonizes even unquantized material —
  that is the point of gravity.
- **COLOR 0 = unison follow, no special case.** The leader's "chord" is then a
  single pitch class, so full PULL makes the follower double the leader's line
  in octaves. Emergent from the mask size, not coded.

### Wiring

1. **Leader side (`Part`):** a getter `chord_pc_mask()` — the pitch classes of
   the currently sounding chord as a 12-bit mask, derived from the same values
   the control tick already pushes to the engine via `set_chord()` (absolute
   semis mod 12). Read-only; no new state.
2. **Instrument:** at the existing control raster (where `Center::update`
   runs), read PULL, pick leader/follower from the sign, push
   `set_gravity(mask, p)` to the follower part. Parts still do not know each
   other — all cross-part knowledge stays in `Instrument`, as with CHOKE.
3. **Follower side (`Part` + `Quantizer`):** `Quantizer::set_gravity_mask(
   abs_pc_mask, on)`. The part draws the PRNG on PITCH-lane fire and switches
   the override on/off for that note. The follower's own COLOR stays
   orthogonal: it stacks its chord on the (pulled) root using its scale mask,
   as today.

### Edge cases

- **Leader choked/silent:** the chord intervals stay latched; gravity keeps
  pulling toward the last chord — the harmony still "stands in the room".
- **Scale/root changes:** the mask is re-derived at the control tick; nothing
  can go stale.
- **PULL swept through center live:** direction changes act on *new* fires;
  sounding bound notes of the old follower are released at the zero crossing
  (override off, slewing back into the scale).
- **STEP sustain / gates:** untouched. Gravity changes pitch quantization
  only, never timing.

## Host / panel

- `Ctl("PULL", SMKNOB, …)` in `gen_panel.py`, appended LAST; center duo column.
- VCV param + `expose.h` entry; init patch gets PULL = 0 (center). The
  `init.vcvm` snapshot needs a refresh afterwards.

## Testing

Offline renders in the existing `tests/` style — sanity checks, no
bit-exactness gates:

1. PULL 0 → the follower quantizer is never touched (structural bypass).
2. PULL full → every follower note's pitch class is in the leader chord,
   checked over a long render.
3. PULL half → bound-note ratio statistically ~50 % (deterministic seed makes
   the count exactly reproducible).
4. Chord change under a sounding bound note → pitch settles onto a tone of the
   new mask within ~40 ms.
5. Leader COLOR 0 + full PULL → follower plays only the leader's pitch class
   (unison/octaves).

## Design dialogue (2026-07-19)

Decisions made in brainstorming, in order: bipolar direction knob over fixed
A→B or auto-by-COLOR; bound notes follow chord changes live (over latch-at-
trigger); COLOR 0 degrades to unison follow (over a gravity-only-above-dyad
special case); pitch-class set over exact leader voicing; mask-override
mechanism over trigger-time snap (kills live-follow) and over folding into
COUPLE (no direction, chains chance to the timing lock; saved a pot but lost
the chosen bipolarity). Knob name PULL over GRAV/HARM — the panel's verb
family (MORPH, DRIFT, TIDE, CHOKE).

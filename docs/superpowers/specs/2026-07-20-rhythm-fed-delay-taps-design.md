# Rhythm-fed delay taps — the other bank's groove places the echoes

**Date:** 2026-07-20
**Status:** Approved (design review with user)
**Supersedes:** `2026-07-18-dust-grain-cloud-design.md` §3 zone S (rev 7, beat
repeat) and zones F/R. See "What is cut and why" below.

## Context

Two attempts to make DUST rhythmic have failed the listening gate.

Rev 6 spawned grains at offsets measured relative to the *moving* write head,
which holds a constant age and therefore reproduces a delay tap regardless of
scheduling. Rev 7 fixed that with an absolute per-beat anchor, and the user's
verdict was:

> es ist immer noch nicht spielbar, halt wie eine erweiterung des delays — die
> macht es aber nicht besser eher schlechter

The diagnosis that both revisions missed:

**Evenly spaced taps *are* a delay.** Zone S played 2, 4, 8 or 16 equal slots
per beat. That is a short delay by construction, however precisely it sits on
the grid. Rhythm requires *unequal* spacing. Rev 7 made the spacing more exact
when it needed to make it less regular.

This spec therefore does not tune zone S a third time. It replaces the grain
cloud with a small multi-tap read bank whose spacings are taken from the
rhythm the *other* bank is already generating, and makes non-uniformity a
property the design guarantees rather than one it hopes for.

### The two facts that make it cheap

1. **The rhythm lives only in the PITCH lane.** SOURCE, SIZE, MOTION and LEVEL
   have all gates true (`lane.cpp:25`); only PITCH is `_melodic`
   (`super_modulator.cpp:14`) and consults groove, rests and DENSE depth
   (`lane.cpp:162-165`). The four texture lanes are ratio-locked metronomes,
   not patterns. The material worth reading is `Part::mod()`'s PITCH lane.
2. **The multi-tap primitive already exists.** `TapeTap` (`dust.h:82-98`) is a
   read-only stereo view over the echo buffer: one AND, one load, at an
   arbitrary offset behind the write head. It is the only part of the grain
   cloud this design keeps.

### The simplification that removes a subsystem

`DeLine::Write` *decrements* the write pointer (`flux.h:45-48`). A constant
offset behind a backwards-moving head is therefore exactly 1× forward playback
of material of that age — continuously, with no retrigger.

This is the same fact that killed rev 6, used the other way round: there it was
the bug, here it is the mechanism. A tap needs no beat anchor, no scheduling
and no per-beat rebirth. `_anchor`, `_beat_pending`, `Flux::sync_beat`, the
`PartFx::sync_beat` hop and the forwarding block in `instrument.cpp:71-76` are
all removed. `Center::beat_edge()` and `Center::beat_samples()` lose their only
consumer and are removed with their tests.

## Decision

**Each part's delay grows three read taps. Their offsets are the sample
distances back to the last three onsets of the *other* bank's PITCH lane. DUST
sets how many taps sound; ROT spreads them spectrally.**

## Design

### Components

**New: `engine/fx/taps.h` / `taps.cpp`.**

- `struct TapeTap` — moved verbatim from `dust.h:82-98`. `flux.cpp` already
  constructs one per sample; only the include changes.
- `struct RhythmView { int32_t gap[3]; bool valid; }` — plain data, the sample
  distances between the last four onsets of a lane, most recent first.
- `void derive_offsets(const RhythmView&, int32_t tape_len, int32_t out[3])` —
  a **free function with no state**. This is where the rule lives, and it is
  unit-testable without constructing a `SuperModulator`, a `Flux` or an audio
  block.
- `class TapBank` — owns three taps' filter state, gains, and crossfade state.
  Replaces `DustCloud`.

**Deleted: `engine/fx/dust.h` / `dust.cpp`** — grain pool, scheduler, anchor,
Hann table, `kInvSqrt` normalisation, birth probabilities, the reverse/octave
machinery, and the zone-R writeback stub (`dust.cpp:359`) that was never
finished.

**Changed: `engine/mod/lane.h` / `lane.cpp`** — the onset-gap ring (below).

**Changed: `engine/instrument.cpp`** — cross-feed at the control tick.

**Changed: `engine/fx/flux.h` / `flux.cpp`** — owns a `TapBank`, not a
`DustCloud`; `sync_beat` removed.

### The onset-gap ring

`ModLane` gains:

```cpp
int32_t _since_onset = 0;      // samples since the last onset
int32_t _gap[3] = { 0, 0, 0 }; // most recent first
int     _onsets = 0;           // saturating count, for validity
```

`_since_onset` increments once per sample in `process()` and by
`kTickInterval` in `tick()`. In `_start_note()` (`lane.cpp:184`) — the point
that already decides whether a note actually begins — the ring shifts,
`_gap[0] = _since_onset`, `_since_onset = 0`, `_onsets` saturates at 4.

**Three gaps require four onsets.** The first onset after `init()` or RST
writes a `_gap[0]` measured from an arbitrary starting point, not from a
previous onset, so it is not a rhythm. `valid` is therefore `_onsets >= 4`,
not `>= 3`, and the ring is reset (`_since_onset = 0`, `_onsets = 0`) by
`SuperModulator::reset_phases()` so RST re-fills it from scratch rather than
carrying a gap that straddles the reset.

**Gaps are measured in samples, not in steps.** This is a deliberate departure
from the first draft of this design, which derived a step period from
`_phase_inc * _steps`. Sample gaps are simpler and strictly more correct:

- FLOW mode has no step grid at all (only cycle wraps, `lane.cpp:293`), so a
  step-based measure has no meaning there. Sample gaps treat STEP and FLOW
  identically with no special case.
- Groove, rests, DENSE depth, freeze and note ties are already accounted for,
  because only a real onset writes the ring.
- It cannot drift from the rate chain (division, TIDE, COUPLE, EVOLVE), because
  it does not model the rate chain — it measures the result.

`RhythmView` is exposed read-only from `ModLane`, forwarded by
`SuperModulator` for `LANE_PITCH`.

Cost: one add per sample on the one lane that already runs per-sample.

### The offset rule

Offsets are **cumulative**, so the taps sit on the previous three onsets:

```
out[0] = gap[0]
out[1] = gap[0] + gap[1]
out[2] = gap[0] + gap[1] + gap[2]
```

**The uniformity guard.** The gaps count as uniform when every one of them lies
within ±2 % of their mean; then the resulting offsets are evenly spaced — a
delay. In that case the gaps
are replaced by `{ g, 0.75·g, 1.5·g }`, where `g` is `gap[0]`, giving
cumulative offsets `g · { 1, 1.75, 3.25 }`. The 3/4 and 3/2 factors are the
MOTION and LEVEL lane ratios (`super_modulator.cpp:8`) — the polyrhythm the
instrument already runs. The result is a limp, not a grid.

The ±2 % tolerance is a fraction, not an absolute count: at 30 samples a
one-sample jitter must not read as non-uniform, and at 30000 samples a
50-sample drift must not read as uniform.

**Validity.** If `!valid` (fewer than three onsets recorded — after `init()`,
after RST, or under a very sparse pattern), all taps are muted. Silence is the
correct output for "no rhythm known yet"; a fallback pattern would invent one.

**Tape overflow.** An offset exceeding `tape_len - 2` **mutes that tap**; it is
not clamped. Clamping would put two taps at the same position, turning a
missing echo into a doubled one. Slow patterns therefore lose the far taps
progressively rather than collapsing.

### Cross-feed

At the existing `_ctrl_ctr == 0` block in `instrument.cpp` — the same route
`sync_beat` used:

```cpp
constexpr int32_t kTapeLen = (int32_t)Flux::kMaxSamples;
int32_t off_a[3], off_b[3];
derive_offsets(_parts[PART_B].mod().rhythm(), kTapeLen, off_a);
derive_offsets(_parts[PART_A].mod().rhythm(), kTapeLen, off_b);
_parts[PART_A].fx().set_tap_offsets(off_a);
_parts[PART_B].fx().set_tap_offsets(off_b);
```

A bank hears the other bank's groove and never its own. `Instrument` is the
only scope where both parts are visible; no `Part` receives a pointer to its
sibling. Both directions are computed unconditionally — the cost is a handful
of integer operations at 500 Hz.

### The audio path

Per sample, per live tap, per channel: `TapeTap::read(right, offset)` — one AND
and one load — then the tap's one-pole chain, then the pan gains into the
stereo sum.

**Filter spread**, one-pole per tap per channel, cutoffs driven by ROT:

| Tap | Chain | ROT = 0 | ROT = 1 |
|---|---|---|---|
| 0 | LP | 18 kHz | 400 Hz |
| 1 | HP then LP | 20 Hz / 18 kHz | 300 Hz / 2.5 kHz |
| 2 | HP | 20 Hz | 1.5 kHz |

Cutoffs interpolate geometrically (constant ratio per unit ROT), so the sweep
is even by ear rather than by hertz. At ROT = 0 every filter is effectively
open and the bank is deliberately a plain multi-tap delay; ROT is the control
that walks it away from that.

**Pan spread**, fixed equal-power constants: tap 0 centre, tap 1 left, tap 2
right. The echo path is effectively mono, so this alone separates the taps from
it spatially.

**Crossfade on re-latch.** A tap holds its current offset and, when a new one
arrives that differs by more than `kRelatchMin = 64` samples, fades over 4 ms
to the new position using the existing SoftSwitch Hann table (`fx_util.h:13`,
`:72`), reading both positions for the duration. The threshold prevents a
gliding tempo from crossfading continuously; 64 samples is 1.3 ms, below which
a jump is inaudible against the tape's own band-limit.

**No writeback.** Taps read only. Feedback and bloom remain the echo's job.
This is a non-goal, not an omission.

**Level.** Fixed per-tap gain by live tap count — three taps at most, known in
advance. None of the grain cloud's active-count normalisation is needed.

### Panel mapping

| Knob | Meaning |
|---|---|
| DUST | 0 = off; then 1, 2, 3 taps with rising gain |
| ROT | filter spread, 0 = open (delay-like) → 1 = maximally separated |

No new control. Both knobs already exist and both lose their previous meaning
(zone selection) with the cloud. This satisfies the hardware constraint: the
panel must stay reducible to the real instrument.

`DUST == 0` returns `Flux::process` to the pre-DUST path, which must stay
bit-exact — as it is today (`flux.cpp:113-117`).

## Testing & acceptance

1. **The property test, and the reason this spec exists.** Over every gap
   triple drawn from a musically reachable grid — each gap swept from 240 to
   96000 samples (5 ms to 2 s at 48 kHz) in 32 geometric steps, all three
   independently, 32³ = 32768 combinations — `derive_offsets` must never
   produce evenly spaced offsets, judged by the same ±2 % test the guard uses.
   Runs in milliseconds. This is the test that would have caught zone S, and it
   makes non-uniformity a proven property rather than an intention.
2. **Onset ring:** a lane driven through a known pattern with rests reports the
   expected sample gaps; a frozen (rest) step writes no onset; a tie does not
   write a second onset. Covered in both STEP and FLOW mode.
3. **Validity:** fewer than three onsets → all taps muted. After RST the ring
   invalidates and re-fills.
4. **Tape overflow:** an offset beyond `tape_len - 2` mutes its tap; the nearer
   taps keep sounding; no two taps share a position.
5. **Crossfade:** an offset change produces no sample-to-sample discontinuity
   above a fixed threshold; a change below `kRelatchMin` triggers no fade.
6. **`DUST == 0` is bit-exact** with the FX path that has no tap bank, on a
   double render.
7. **ROT = 0 leaves the taps effectively unfiltered**; ROT = 1 shows measured
   band separation between the three taps.
8. **Determinism:** double render of at least two scenarios byte-identical.
9. Every new test is mutation-tested. A test that passes against a deliberately
   broken implementation is not a test.

**Hard gate:** no existing PITCH, groove, CHOKE or Center test may be edited.
The sole exception is the `beat_edge` / `beat_samples` tests, which are deleted
together with the feature they cover. If a melodic-path test needs touching,
the design is violated — stop and reassess.

**Measured cost:** the hardware bench is re-run by the user. The estimate is
roughly half the DUST-path cost, but the structural gain is that the cost
becomes *constant* — no birth process, no variable grain count, no
data-dependent worst case. On an instrument already at 117 % / 123 % of block
budget, a predictable ceiling is worth more than a lower average. Per project
convention, an estimate is not trusted until it is measured.

### The listening gate — agreed before implementation

This is the third attempt. Two gestures decide it:

- **H1:** with DUST up, land a rhythmic figure on purpose, without looking at
  the knob.
- **H2:** change MELO or DENSE on bank B and hear bank A's delay rhythm follow.
  This is the real test of the idea — if the coupling is not audible, the
  cross-feed is decoration and the construction does not carry.

**If H1 and H2 both fail, DUST and ROT are cut outright. There is no fourth
attempt.** After two failed revisions, a criterion fixed in advance is worth
more than any amount of tuning afterwards.

## What is cut and why

- **Zone S (beat repeat, rev 7)** — evenly spaced by construction, therefore a
  delay. Cut, not tuned.
- **Zone F (free scatter)** and **zone R (rot)** — not condemned by listening,
  but they are what makes DUST a three-meaning zone knob, and their grain pool
  is the cost this rework exists to reclaim. Cut so that DUST has exactly one
  meaning.
- **The beat plumbing** (`Center::beat_edge`, `beat_samples`, `Flux::sync_beat`,
  `PartFx::sync_beat`, the `instrument.cpp` forwarding) — no consumer remains.
  Removed with its tests; re-adding it is cheap if a later feature needs it.

## Non-goals

- **`FXT_FLUX_TIME`**, the dead modulation slot: computed, cached and smoothed
  (`part_fx.cpp:28`) but never applied, with a test pinning that it must *not*
  move the echo (`test_part_fx.cpp:112`). Named here so it is not forgotten;
  deliberately not touched.
- Writeback / self-feedback from the taps.
- Any new panel control.
- Interpolated (fractional) tap offsets — reads stay integer, as `TapeTap`
  already is.
- Per-tap resonance or a full SVF per tap. Rejected on cost: six `daisysp::Svf`
  instances exceed the eight grains being removed, and the budget has no room.

# Rhythm-fed delay taps — the other bank's groove places the echoes

**Date:** 2026-07-20 (rev 2, same day)
**Status:** Approved (design review rev 2 with user)
**Supersedes:** `2026-07-18-dust-grain-cloud-design.md` §3 zone S (rev 7, beat
repeat) and zones F/R. See "What is cut and why" below.

**Rev 2 changes** (critical review 2026-07-20, all user-decided):

1. **Two taps, not three.** The echo head is already a third voice; the third
   tap bought rhythmic complexity the CPU budget cannot carry.
2. **Mono reads.** Each tap reads one tape channel, panned; the stereo image
   was coming from the fixed pan spread anyway. Halves the SDRAM cost.
3. **Dip, not crossfade.** A re-latch fades out at the old offset, jumps,
   fades in — never two read positions at once. The worst case is constant.
4. **Offsets latch once per source-lane cycle**, not per onset. Groove is a
   repeating irregular pattern; per-onset re-latching repeats nothing.
5. **DUST is a continuous morph**, not a stepped tap count. Intermediate
   positions are an accent hierarchy, not dead zones.
6. **The onset hook moves to `_on_boundary()`.** Rev 1 hooked `_start_note()`,
   which never runs in FLOW mode — the ring would never fill there.
7. **The deletion list is completed** — rev 1 missed the EchoDelay erosion
   remnants, the head takeover, the host tooltips, the seed chain, and the
   dust tests/benches.

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

Rev 2 adds the second half of that diagnosis: **a groove is an unequal spacing
that repeats.** Rev 1 guaranteed the inequality but re-derived the tap
positions on every onset of the source lane, so even a static, looping source
pattern rotated a different offset set into the taps every few hundred
milliseconds. The echo figure would never have stood still long enough to be
heard as a figure — the same "not playable" failure by another route. Both
properties are now guaranteed: unequal by the guard, repeating by the
cycle latch.

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

**Each part's delay grows two read taps. Their offsets are the sample
distances back to the last two onsets of the *other* bank's PITCH lane,
latched once per that lane's cycle. DUST fades the taps in continuously; ROT
spreads them spectrally.**

## Design

### Components

**New: `engine/fx/taps.h` / `taps.cpp`.**

- `struct TapeTap` — moved verbatim from `dust.h:82-98`. `flux.cpp` already
  constructs one per sample; only the include changes.
- `struct RhythmView { int32_t gap[2]; bool valid; }` — plain data, the sample
  distances between the last three onsets of a lane, most recent first,
  **as latched at the lane's last cycle wrap**.
- `void derive_offsets(const RhythmView&, int32_t tape_len, int32_t out[2])` —
  a **free function with no state**. This is where the rule lives, and it is
  unit-testable without constructing a `SuperModulator`, a `Flux` or an audio
  block.
- `class TapBank` — owns two taps' filter state, gains, and dip state.
  Replaces `DustCloud`. No RNG: the bank is deterministic, so the whole seed
  chain (`Flux::init`'s `seed`, `PartFx`'s `dust_seed`, the derivation in
  `part.cpp:28`) goes with the cloud.

**Deleted: `engine/fx/dust.h` / `dust.cpp`** — grain pool, scheduler, anchor,
Hann table, `kInvSqrt` normalisation, birth probabilities, the reverse/octave
machinery, the head takeover, and the zone-R writeback stub (`dust.cpp:359`)
that was never finished.

**Changed: `engine/mod/lane.h` / `lane.cpp`** — the onset-gap ring (below).

**Changed: `engine/instrument.cpp`** — cross-feed at the control tick.

**Changed: `engine/fx/flux.h` / `flux.cpp`** — owns a `TapBank`, not a
`DustCloud`; `sync_beat` removed; `EchoDelay` loses the zone-R erosion
remnants (below).

**Changed: `host/vcv/src/Spotymod.cpp`** — `DustQuantity`/`RotQuantity`
tooltips rewritten (tap morph / filter spread); the `fx/dust.h` include and
the `dust_tuning` zone constants they read are gone.

### The onset-gap ring

`ModLane` gains:

```cpp
int32_t _since_onset = 0;        // samples since the last onset
int32_t _gap[2] = { 0, 0 };      // most recent first (live ring)
int     _onsets = 0;             // saturating count, for validity
RhythmView _rhythm = {};         // the latched snapshot, updated at wrap
```

`_since_onset` increments once per sample in `process()` and by
`kTickInterval` in `tick()`.

**The hook is the `gated` branch of `_on_boundary()`** (`lane.cpp:173-181`) —
the one point that fires in both STEP and FLOW mode, after groove, rests and
DENSE have had their say. There the ring shifts, `_gap[0] = _since_onset`,
`_since_onset = 0`, `_onsets` saturates at 3. Rev 1 hooked `_start_note()`
instead, which is guarded by `_melodic && _step_mode` (`lane.cpp:175`) and
therefore never runs in FLOW — the ring would never have filled there.

**On ties, honestly:** at lane level every gated boundary starts a note; a
"tie" is `_note_hold` reaching the next note — an envelope-length fact, not a
skipped onset. Every gated boundary therefore writes the ring. (In FLOW the
onsets are the cycle wraps, one per cycle — necessarily uniform, so FLOW
sources land on the uniformity guard's limp. Correct, and cheaper than a
special case.)

**Two gaps require three onsets.** The first onset after `init()` or RST
writes a `_gap[0]` measured from an arbitrary starting point, not from a
previous onset, so it is not a rhythm. Validity is therefore `_onsets >= 3`,
and the ring is reset (`_since_onset = 0`, `_onsets = 0`) by
`SuperModulator::reset_phases()` so RST re-fills it from scratch rather than
carrying a gap that straddles the reset.

**The cycle latch.** `_rhythm` (the view consumers see) is copied from the
live ring in `_wrap_events()` — once per cycle, at the pattern boundary, in
both STEP and FLOW. Between wraps the ring keeps recording but nothing
downstream moves. Consequences, all intended:

- A static, looping source pattern latches the same gaps at every wrap; the
  derived offsets land within the re-latch threshold; the taps stand still and
  the echo figure **repeats** — the property rev 1 lacked.
- A MELO/DENSE/STEPS change on the source bank takes effect at that lane's
  next wrap: the coupling (H2) arrives quantised to the pattern boundary,
  which is a musical latency, not a defect.
- After RST the taps stay muted until the first wrap at which the ring is
  valid. Silence is still the correct output for "no rhythm known yet".
  (An immediate latch-on-valid hybrid was considered and rejected: one rule,
  one latch point.)

**Gaps are measured in samples, not in steps.** Sample gaps treat STEP and
FLOW identically with no special case (FLOW has no step grid, only cycle
wraps, `lane.cpp:293`). Groove, rests, DENSE depth and freeze are already
accounted for, because only a gated boundary writes the ring. And it cannot
drift from the rate chain (division, TIDE, COUPLE, EVOLVE), because it does
not model the rate chain — it measures the result.

`RhythmView` is exposed read-only from `ModLane`, forwarded by
`SuperModulator` for `LANE_PITCH`.

Cost: one add per sample on the one lane that already runs per-sample, plus a
two-int copy per cycle.

### The offset rule

Offsets are **cumulative**, so the taps sit on the previous two onsets:

```
out[0] = gap[0]
out[1] = gap[0] + gap[1]
```

**The uniformity guard.** The gaps count as uniform when both lie within ±2 %
of their mean; then the taps plus the dry signal at t = 0 are evenly spaced —
a delay. In that case the gaps are replaced by `{ g, 0.75·g }`, where `g` is
`gap[0]`, giving cumulative offsets `g · { 1, 1.75 }`. The 3/4 factor is the
MOTION lane ratio (`super_modulator.cpp:8`) — a polyrhythm the instrument
already runs. The result is a limp, not a grid.

The ±2 % tolerance is a fraction, not an absolute count: at 30 samples a
one-sample jitter must not read as non-uniform, and at 30000 samples a
50-sample drift must not read as uniform.

**Validity.** If `!valid` (fewer than two gaps recorded — after `init()`,
after RST, or under a very sparse pattern), both taps are muted. Silence is
the correct output for "no rhythm known yet"; a fallback pattern would invent
one.

**Tape overflow.** An offset exceeding `tape_len - 2` **mutes that tap**; it
is not clamped. Clamping would put two taps at the same position, turning a
missing echo into a doubled one. Slow patterns therefore lose the far tap
before the near one.

### Cross-feed

At the existing `_ctrl_ctr == 0` block in `instrument.cpp` — the same route
`sync_beat` used:

```cpp
constexpr int32_t kTapeLen = (int32_t)Flux::kMaxSamples;
int32_t off_a[2], off_b[2];
derive_offsets(_parts[PART_B].mod().rhythm(), kTapeLen, off_a);
derive_offsets(_parts[PART_A].mod().rhythm(), kTapeLen, off_b);
_parts[PART_A].fx().set_tap_offsets(off_a);
_parts[PART_B].fx().set_tap_offsets(off_b);
```

A bank hears the other bank's groove and never its own. `Instrument` is the
only scope where both parts are visible; no `Part` receives a pointer to its
sibling. Both directions are computed unconditionally — the cost is a handful
of integer operations at 500 Hz, reading a view that only changes once per
source-lane cycle.

### The audio path

Per sample, per audible tap: **one mono read** — `TapeTap::read(ch, offset)`,
one AND and one load — then the tap's one-pole, then fixed equal-power pan
gains into the stereo sum. The echo path is effectively mono, so which channel
a tap reads is a decorrelation nicety, not information loss:

| Tap | Reads | Panned | Filter | ROT = 0 | ROT = 1 |
|---|---|---|---|---|---|
| 0 | L | left | LP | 18 kHz | 400 Hz |
| 1 | R | right | HP | 20 Hz | 1.5 kHz |

Cutoffs interpolate geometrically (constant ratio per unit ROT), so the sweep
is even by ear rather than by hertz. The interpolation (a `powf` per tap) runs
**only when ROT changes**, guarded exactly like `set_dust`/`set_rot` already
guard `_remap()` (`flux.h:237-243`) — never per tick. At ROT = 0 both filters
are effectively open and the bank is deliberately a plain two-tap delay; ROT
is the control that walks it away from that.

**Dip on re-latch.** A tap holds its current offset. When a new one arrives
that differs by more than `kRelatchMin = 64` samples, the tap fades to zero
over 2 ms at the old offset, jumps, and fades back in over 2 ms at the new
one — a dip, using the existing SoftSwitch Hann table (`fx_util.h:13`, `:72`).
**At no point does a tap read two positions.** The rev 1 crossfade doubled the
reads of a whole bank whenever the source pattern changed — a data-dependent
worst case, which is the exact disease this design is meant to cure. A 2 ms
dip in a delay tail is inaudible; a CPU spike on the hardware is not. The
threshold prevents a gliding tempo from dipping continuously; 64 samples is
1.3 ms, below which a jump is inaudible against the tape's own band-limit.

**DUST is a morph.** Tap 0's gain ramps 0 → full over the knob's first half,
tap 1's over the second half: `gain_i = clamp(2·DUST − i, 0, 1) · kTapGain`.
Intermediate positions are an accent hierarchy (tap 0 full, tap 1 half =
strong/weak) — the groove dimension the stepped mapping lacked — and the knob
sweeps click-free. Gains are slewed at control rate. **A tap at gain 0 skips
its read entirely**, so CPU follows the knob down to zero. `kTapGain` is one
taste constant for the play-test, sized so full DUST sits at parity with a
direct tape read (the grain cloud's window/pan losses do not exist here — the
7.27 dB makeup dies with them).

**No writeback.** Taps read only. Feedback and bloom remain the echo's job.
This is a non-goal, not an omission.

### Panel mapping

| Knob | Meaning |
|---|---|
| DUST | 0 = off; continuous morph — tap 0 fades in over the first half, tap 1 over the second |
| ROT | filter spread, 0 = open (delay-like) → 1 = maximally separated |

No new control. Both knobs already exist and both lose their previous meaning
(zone selection) with the cloud. This satisfies the hardware constraint: the
panel must stay reducible to the real instrument. The `Spotymod.cpp` tooltips
follow the new meanings.

`DUST == 0` returns `Flux::process` to the pre-DUST path, which must stay
bit-exact — as it is today (`flux.cpp:113-117`).

## Testing & acceptance

1. **The property test, and the reason this spec exists.** Over every gap pair
   drawn from a musically reachable grid — each gap swept from 240 to 96000
   samples (5 ms to 2 s at 48 kHz) in 32 geometric steps, both independently,
   32² = 1024 combinations — `derive_offsets` must never produce offsets that
   are evenly spaced **counting the dry signal at t = 0** (i.e. the resulting
   spacings `{out[0], out[1] − out[0]}` never pass the same ±2 % test the
   guard uses). This is the test that would have caught zone S, and it makes
   non-uniformity a proven property rather than an intention.
2. **Onset ring:** a lane driven through a known pattern with rests reports
   the expected sample gaps; a rest step writes no onset; every gated boundary
   writes one (ties are envelope-level, not ring-level). Covered in both STEP
   and FLOW mode — in FLOW the wraps are the onsets.
3. **Cycle latch:** a static pattern yields identical `rhythm()` snapshots at
   consecutive wraps (taps stand still); a DENSE change mid-cycle changes
   nothing until the next wrap, then everything at once.
4. **Validity:** fewer than two recorded gaps → both taps muted. After RST the
   ring invalidates and re-fills; taps stay muted until the first valid wrap.
5. **Tape overflow:** an offset beyond `tape_len - 2` mutes its tap; the near
   tap keeps sounding; no two taps share a position.
6. **Dip:** an offset change produces no discontinuity above a fixed
   threshold, and at no sample does the bank read more positions than live
   taps; a change below `kRelatchMin` triggers no dip.
7. **`DUST == 0` is bit-exact** with the FX path that has no tap bank, on a
   double render.
8. **Morph:** gain ramps match the mapping; a tap at gain 0 performs no read
   (observable via a read-count telemetry accessor, same idiom as the cloud's
   `active_grains()`).
9. **ROT = 0 leaves the taps effectively unfiltered**; ROT = 1 shows measured
   band separation between the two taps.
10. **Determinism:** double render of at least two scenarios byte-identical.
11. Every new test is mutation-tested. A test that passes against a
    deliberately broken implementation is not a test.

**Hard gate:** no existing PITCH, groove, CHOKE or Center test may be edited.
The sole exception is tests that die with their feature: the
`beat_edge`/`beat_samples` cases in `test_center.cpp`, and the freeze/erosion/
takeover cases in `test_flux.cpp`. If a melodic-path test needs touching, the
design is violated — stop and reassess.

**Measured cost.** Reference points from `docs/bench/2026-07-19`:
`instrument_worst` at 92.1 % avg / 97.8 % max, `dust_8_opt` (eight streaming
grain reads + window + pan, one part) at ~6.2 %, i.e. roughly 0.6–0.8 % per
streaming SDRAM read. This design's worst case is **two mono reads + two
one-poles per part, constant**: an estimated 1.5–2 % per part, ~3–4 % for both
— versus rev 1's 7–8 % steady with data-dependent spikes toward 12 %. The
hardware bench is re-run by the user; per project convention, an estimate is
not trusted until it is measured. The bench keeps a tap-shaped workload:
`workloads_dust.cpp` is replaced by tap workloads, and the `grain_read_sdram`
anchor gives way to a `tap_read_sdram` (streaming, not scattered — the old
anchor prices an access pattern that no longer exists).

### The listening gate — agreed before implementation

This is the third attempt. Two gestures decide it:

- **H1:** with DUST up, land a rhythmic figure on purpose, without looking at
  the knob.
- **H2:** change MELO or DENSE on bank B and hear bank A's delay rhythm follow
  at the next pattern boundary. This is the real test of the idea — if the
  coupling is not audible, the cross-feed is decoration and the construction
  does not carry.

**If H1 and H2 both fail, DUST and ROT are cut outright. There is no fourth
attempt.** After two failed revisions, a criterion fixed in advance is worth
more than any amount of tuning afterwards.

## What is cut and why

Everything below exists only on `dust-explore`, so deletion is history-safe;
the panel knobs themselves (`7ec6382`, `48cd556`) stay.

- **Zone S (beat repeat, rev 7)** — evenly spaced by construction, therefore a
  delay. Cut, not tuned.
- **Zone F (free scatter)** and **zone R (rot)** — not condemned by listening,
  but they are what makes DUST a three-meaning zone knob, and their grain pool
  is the cost this rework exists to reclaim. Cut so that DUST has exactly one
  meaning.
- **The beat plumbing** (`Center::beat_edge`, `beat_samples`, `Flux::sync_beat`,
  `PartFx::sync_beat`, the `instrument.cpp` forwarding) — no consumer remains.
  Removed with its tests; re-adding it is cheap if a later feature needs it.
- **The EchoDelay erosion remnants** — `set_freeze`/`set_wear`/`WriteBlend`/
  `Advance` and `Process()`'s `wb` parameter (`flux.h:63-74`, `:139-143`,
  `:156-176`). Zone-R groundwork; no production caller, only `test_flux.cpp`
  exercises it. Removed with those tests.
- **The head takeover** — `head_gain()`, `kTakeoverKnee`, and the takeover
  cases in `test_flux.cpp`. The taps sit beside the echo, never in its place.
- **`dust_tuning` and the host's zone tooltips** — `Spotymod.cpp:50-68` reads
  `kZoneSEnd`/`kZoneFEnd` for `RotQuantity`; both quantities get new text.
- **The seed chain** — `Flux::init(seed)`, `PartFx`'s `dust_seed`,
  `part.cpp:28`. The tap bank has no randomness.
- **`tests/test_dust.cpp`** entirely; **`bench/workloads_dust.cpp`** replaced
  by tap workloads; the `grain_read_sdram` anchor replaced by
  `tap_read_sdram`.

## Non-goals

- **`FXT_FLUX_TIME`**, the dead modulation slot: computed, cached and smoothed
  (`part_fx.cpp:28`) but never applied, with a test pinning that it must *not*
  move the echo (`test_part_fx.cpp:112`). Named here so it is not forgotten;
  deliberately not touched.
- Writeback / self-feedback from the taps.
- Any new panel control.
- Interpolated (fractional) tap offsets — reads stay integer, as `TapeTap`
  already is.
- A third tap. Rejected in rev 2 on cost: the echo head is already a third
  voice, and two streaming reads per part is the budget's comfort zone.
- Per-tap resonance or a full SVF per tap. Rejected on cost.
- An immediate latch when the ring first becomes valid. One latch point (the
  cycle wrap) is one rule; the RST-to-first-wrap silence is acceptable.

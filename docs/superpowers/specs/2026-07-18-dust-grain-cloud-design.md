# DUST / ROT — grain cloud, tape rot + erosion freeze on the FLUX echo

**Date:** 2026-07-18 (rev 2 — performance/experimental redesign, same day)
**Status:** design approved, ready for implementation plan
**Scope:** a per-part granular read/write stage inside the FLUX block, a character axis (ROT), and a tape FREEZE with erosion. No new audio buffers. No change to GRIT, COMP, reverb, or the master chain.

## Problem

Clouds-style granular texture is wanted — smearing, freeze/stutter, random scatter — but a Clouds port would be redundant (diffusion/reverb already exists as ROOM) and too heavy for the remaining Daisy budget. And a polite read-only grain cloud is not enough either (rev 1 verdict): this must be a **performance effect played live like the delay**, and a **sound design tool** — like the reverb, dialable from plain delay all the way into territory standard tools don't reach.

The insight that makes it nearly free: **the FLUX tape already holds 5 s of per-part stereo material**. Grains are extra read heads — and, at the experimental end, extra *write* heads — on that existing tape. Zero extra sample memory; echo and grains share one sonic identity ("the tape").

Requirements (user decisions):
- DUST = 0 ⇒ **bit-exact plain delay**, always reachable.
- Two axes: **how much** grain activity (DUST) vs. **which character** (ROT).
- Character range: tempo-synced stutter → free scatter → reverse grains → self-eating writeback.
- FREEZE with two personalities: preserving looper (low ROT) or **eroding loop that decomposes itself** (high ROT).
- No pitch-shifted grains (reverse is time-reversal, not transposition).
- No FX-target lane; live play = knobs + freeze gate.

## Existing infrastructure this reuses

- `engine/fx/flux.h` — `EchoDelay` over `DeLine` rings (240 000 samples = 5 s per channel per part). The write pointer decrements once per sample; a read at constant offset behind it **is** 1× forward playback delayed by that offset. `Flux` already receives BPM and knows `_delay_time`.
- `engine/mod/rng.h` — deterministic xorshift32 `Rng` (bit-reproducible, statistically testable).
- `engine/util/fast_sin.h` — for the raised-cosine (Hann) grain window.
- `engine/fx/part_fx.*`, `engine/instrument.*` — per-part FX plumbing and setter forwarding.
- `host/vcv/res/gen_panel.py` — panel generator; params appended at end of `PARAMS` for patch compatibility (FILT/TIDE/FLUXRATE precedent).

## Design

### 1. Grain mechanics — read heads on the tape

A **forward grain** never moves relative to the write head: a constant integer offset behind it is exactly 1×-speed forward playback (same mechanism as the echo head). A **reverse grain** increments its offset by +2 per sample: it recedes through the material at 1× — true time-reversal, no resampling, no pitch change of content playback speed. Both cost 1 integer array read × Hann window × 2 pan multiplies per sample.

Per grain state: offset (+ per-sample offset delta 0 or +2), age, length, equal-power pan gains, source channel (reads L **or** R tape, random — free stereo decorrelation), reverse flag.

New `engine/fx/dust.h/.cpp`: `DustCloud`, fixed pool of **8 grains** per part (plain structs, no heap). Own `Rng`, fixed distinct per-part seeds ⇒ bit-identical desktop/firmware, testable. Reverse grains clamp so the offset never overruns the tape length within a grain's lifetime.

### 2. DUST knob — how much

`d ∈ 0..1`, per part:

- **d = 0:** dust path skipped entirely — **bit-exact** with today's FLUX.
- Density: mean grain birth rate rises exponentially (sparse splinters → full 8-voice overlap).
- Grain length range rises with d (≈ 25–100 ms sparse → 80–400 ms dense).
- Level: grain sum normalized by `1/sqrt(expected overlap)`.
- **Head takeover:** above d ≈ 0.7 the echo read head fades (equal-power) to zero at d = 1 — the cloud eats the delay. Feedback keeps recirculating underneath (the tape stays alive; only the *audible* head swaps).

### 3. ROT knob — which character

`r ∈ 0..1`, per part, three zones with continuous morphs (all breakpoints are tuning constants in one header block, finalized in the deferred play-test):

**Zone S — synced stutter (r = 0 … ~0.33).**
Grain births lock to a grid derived from the delay itself: **birth interval = FLUX delay time / 4** (clamped to a sane minimum) — stutter bursts always subdivide the echo, automatic dub polyrhythm with zero extra controls. DUST sets the probability that a grid slot fires (and density stacks bursts). Spray is tight (near the head) → repeat/stutter character. As r rises through the zone, timing jitter grows from 0 to fully random. Like FLUX itself this is duration-synced, not phase-locked to the sequencer (consistent with the existing delay behavior).

**Zone F — free scatter (r ≈ 0.33 … 0.66).**
The classic cloud: free-running stochastic scheduler, spray widens across the zone from ~150 ms up to the full 5 s. Read-only, forward grains. (This is rev 1's whole design, now one zone of three.)

**Zone R — rot (r ≈ 0.66 … 1).**
Two things ramp together:
- **Reverse probability** 0 → ~70 %: splinters increasingly play backwards.
- **Writeback gain** 0 → max: the grain sum is written back onto the tape (see §4) — the cloud smears itself over generations; the delay mutates into a self-eating organism. tanh-bounded, cannot run away.

### 4. Writeback + FREEZE with erosion

`EchoDelay` grows a freeze flag and a writeback input. Per sample, the store becomes:

| state | tape write |
|---|---|
| normal, no writeback | `echo_out * fb + in` (unchanged) |
| normal + writeback (zone R) | `echo_out * fb + in + tanh(grains * wb_gain)` |
| **frozen**, low ROT | *no store* — pointer still advances (preserving 5 s looper) |
| **frozen**, zone R (**erosion**) | `line[ptr] = tanh(line[ptr] * wear + tanh(grains * wb_gain))`, `wear` slightly < 1 |

- **Freeze (any ROT):** input and feedback stop being written; the pointer keeps advancing, so echo head and grain taps travel through the frozen material on their own. The echo head becomes a **5 s looper** (RATE only phases it), grains scatter the still image. Dry input keeps passing around FLUX as always.
- **Erosion (freeze + zone R):** the frozen loop does not preserve — **it decomposes**. Every pass, grains burn themselves into the loop while `wear` slowly abrades what was there. Progressive, irreversible self-granulation: a tape loop wearing out, playable in real time via the ROT knob (pull ROT down mid-erosion to keep what remains). This is the signature feature — no standard tool does this.
- Unfreeze resumes normal writing; the old/new seam is honest tape aesthetic (write-in crossfade only if the play-test hears a click).
- Boundedness: every writeback passes `tanh`, erosion additionally decays by `wear` — recirculating energy is intrinsically bounded (tested).

### 5. Signal flow (in `Flux::process`)

```
send = sw.process()
gLR  = dust.process()                   // ReadTap() on both channel tapes, fwd + rev grains
wb   = tanh(grain_sum * wb_gain)        // 0 outside zone R (and skipped when d == 0)
e    = echo.Process(in * send, wb)      // store per the §4 table
out += (e * head_gain(d) + gLR) * _mix_lin
```

- Grain sum joins **before** `_mix_lin`: the FLUX (MIX) knob remains the single wet control for everything off the tape.
- Grain *reads* never pass band-pass/tanh (the cloud is rawer/brighter than the echo — its own voice); only the *writeback* is tanh-bounded.
- `d == 0` skips the dust path; FLUX off (SoftSwitch idle) skips the whole block — both bit-exact, both free. FREEZE requires FLUX on (it is a tape state).

### 6. Control surface + modulation policy

Per part (mirrored A/B like the FLUX cluster):

- **`DUST_A/B`** — SMKNOB (amount).
- **`ROT_A/B`** — SMKNOB (character: sync → free → rot).
- **`FRZ_A/B`** — LATCH pad (widget family of ENG/GRIT pads).
- **`FRZGATE_A/B`** — gate input in the jack strip: latch **OR** gate high = frozen ⇒ a sequencer can stutter-freeze rhythmically; held gate + high ROT = performed erosion.

**Patch compatibility (hard requirement):** all new params/inputs appended at the **end** of their enums in `gen_panel.py` with explicit coordinates — never added to the `part_controls()` template (would shift part-B/SHARED ids and break existing `.vcv` patches).

**Sequencing vs. M5 sampler (agreed 2026-07-18):** DUST ships **before** the M5 sampler. Both specs append params at the end of `PARAMS`, so release order fixes the ids — DUST/ROT/FRZ first, then M5's REC. The two must not be developed in parallel branches touching `gen_panel.py`.

**No FX-target lane** (user decision): DUST/ROT are hand-played; rhythm comes from the sync zone and the freeze gate. The 5-lane == pad-slot structure stays untouched.

**Hardware reducibility note:** worst case +4 small knobs, +2 pads, +2 jacks across both parts. Reduction ladder if the real panel runs out of space: (1) drop gate jacks, (2) one shared FRZ button, (3) ROT becomes a shared (both-parts) knob, (4) last resort: ROT fixed mid-travel and DUST alone — the engine API is per-part and per-axis regardless, so the panel can merge without engine changes.

### 7. Engine API + host plumbing

- `Flux::set_dust(float)`, `Flux::set_rot(float)`, `Flux::set_freeze(bool)`; `Flux` owns the `DustCloud`, passes tap access + delay time (sync grid) + collects writeback.
- `PartFx` / `Instrument` forwarding in the `set_flux_mix` pattern: `set_dust(p, v)`, `set_rot(p, v)`, `set_freeze(p, on)`.
- `host/render/scenario.cpp`: actions `set_dust`, `set_rot`, `set_freeze` + demo scenarios (`dust_stutter.json` — sync zone; `dust_erosion.json` — freeze + zone R).
- `host/vcv/src/Spotymod.cpp`: knobs/pad/gate → setters; DUST tooltip in percent; ROT tooltip naming the zone ("SYNC / FREE / ROT"); FRZ on/off.

### 8. CPU + memory budget

- **Memory:** ~8 grain structs × 2 parts (< 1 KB). **No new audio buffers.**
- **CPU:** worst case 16 grains × (1 read + window + 2 mults); reverse costs one extra add. Writeback adds one tanh per part-sample *only in zone R*; erosion one more *only while frozen*. No transcendentals otherwise in the hot path (window via LUT/`fast_sin`). Estimated ≪ 1 % on the H750 @ 480 MHz.
- DUST = 0 and no freeze: two branch checks.

## Testing

New `tests/test_dust.cpp` (+ small additions to `tests/test_flux.cpp`):

- **Bypass bit-exactness:** d = 0, freeze off ⇒ output identical to pre-DUST FLUX, sample for sample (any ROT).
- **Determinism:** same seed/settings/input twice ⇒ bit-identical output.
- **Sync grid:** in zone S with jitter 0, measured birth intervals == delay_time/4 within tolerance; jitter grows monotonically across the zone.
- **Reverse grains:** a known transient in the tape appears time-reversed in a reverse grain's output.
- **Preserving freeze:** freeze + low ROT ⇒ buffer contents unchanged after N loud input samples, output non-silent, echo output repeats with 5 s period; dry input still passes.
- **Erosion:** freeze + r = 1 ⇒ buffer contents measurably change per pass, RMS remains bounded, and with DUST at 0 the loop only wears (decays) without new grain content.
- **Writeback boundedness:** full-scale input, d = 1, r = 1, sustained ⇒ tape and output stay within headroom (no runaway).
- **Click-free grains:** max sample-to-sample delta of the grain sum bounded across many births/deaths (both directions).
- **Scheduler statistics:** free-zone birth rate for a given d within tolerance of the mapped rate (style of `test_rng.cpp`).

## Relation to the M5 sampler (texture deck)

Designed the same day: `2026-07-18-sampler-texture-deck-design.md` is a
granular *engine* over its own 42 s buffer. The two do not overlap — the
canonical distinction:

> **The sampler cloud is harmonic** — pitch-quantized, chord-locked, it
> plays *in the scale*. **DUST is inharmonic** — no pitch shift, it
> scatters *time on the tape*.

FREEZE (5 s, performative, eroding) and sampler record (42 s, deliberate,
persistent) are likewise two different gestures, not duplicates. DUST is
implemented first (see sequencing note in §6) and establishes the grain
idiom — pool, `fast_sin` window, equal-power pan, `1/sqrt(overlap)`
normalization, seeded-Rng statistics tests — which the sampler then
reuses (window/pan helpers extracted to `engine/util/` if that is cleaner
once DUST lands). The read mechanics stay separate by design: DUST reads
integer offsets behind a moving write head; the sampler does interpolated,
pitch-scaled reads on a static buffer.

## Out of scope / deferred

- Rack play-test and listening pass (project convention) — final tuning of all zone breakpoints, curves, `wear`, `wb_gain`, takeover knee, normalization.
- Unfreeze write-in crossfade — only if the play-test hears a click.
- Pitch-shifted grains, half/double-speed heads, write-head skip chaos (GRIT overlap) — deliberately not in v1.
- Firmware (M6) wiring — engine code is target-agnostic like the rest of the FX layer.

## Files touched (anticipated)

- `engine/fx/dust.h`, `engine/fx/dust.cpp` — **new**: `DustCloud` (pool, zones, scheduler, window, reverse, writeback sum).
- `engine/fx/flux.h`, `engine/fx/flux.cpp` — `ReadTap`, freeze/erosion store, writeback input, dust integration, head takeover.
- `engine/fx/part_fx.h/.cpp`, `engine/instrument.h/.cpp` — setter forwarding.
- `host/vcv/res/gen_panel.py` + regenerated `generated_panel.hpp` — `DUST_A/B`, `ROT_A/B`, `FRZ_A/B`, `FRZGATE_A/B` appended.
- `host/vcv/src/Spotymod.cpp` — param config, zone tooltip, gate OR latch logic.
- `host/render/scenario.cpp` + `host/render/scenarios/dust_stutter.json`, `dust_erosion.json`.
- `tests/test_dust.cpp` (new), `tests/test_flux.cpp`.

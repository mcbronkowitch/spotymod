# DUST / ROT — grain cloud, tape rot + erosion freeze on the FLUX echo

**Date:** 2026-07-18 (rev 3 — control surface reduced to what the hardware will
actually have, same day); **rev 4 2026-07-19 — CPU budget corrected, §8**
**Status:** design approved, **implementation BLOCKED on measurement.** The
plan (`docs/superpowers/plans/2026-07-18-dust-grain-cloud.md`) is written and
must not be executed until the `dust` bench family has run on hardware: §8's
original "≪ 1 %" understated the cost by roughly 30× and the instrument has
2.3 points of margin. See §8.
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
- No FX-target lane; live play = knobs + freeze pad.

## Existing infrastructure this reuses

- `engine/fx/flux.h` — `EchoDelay` over `DeLine` rings (240 000 samples = 5 s per channel per part). The write pointer decrements once per sample; a read at constant offset behind it **is** 1× forward playback delayed by that offset. `Flux` already receives BPM and knows `_delay_time`.
- `engine/mod/rng.h` — deterministic xorshift32 `Rng` (bit-reproducible, statistically testable).
- `engine/fx/fx_util.h` — `hann_value_at()`, the existing 192-entry table. It is `sin²(π/2·x)`, so folding a grain's age about its midpoint yields **exactly** `sin²(π·age)` — a true Hann grain window with no new table and no `fast_sin` call. (`engine/util/fast_sin.h` is still used, but for the equal-power pan, matching the `Voice::update_control` idiom.)
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

- **`DUST_A/B`** — SMKNOB (amount). Appended; sits in slot 3 of the FX box's bottom row, which widens from two knobs to four (`GRIT COMP DUST ROT`, pitch 8.833 mm against a 3.0 mm knob radius).
- **`ROT_A/B`** — SMKNOB (character: sync → free → rot). Appended; slot 4 of the same row.
- **`FRZ_A/B`** — LATCH pad. **Reuses the `TRIGGER_A/B` slot in place** in `part_controls()`: the manual trigger was unused, and taking over its template position is what keeps `PART_STRIDE` at 23. Kind changes from momentary (`SMBTN`) to latch.

**No gate inputs (decision 2026-07-18).** The spec's earlier `FRZGATE_A/B` is
dropped: the real hardware will not have those jacks, so putting them on the
VCV panel would violate the standing reducibility constraint — this is
reduction ladder step (1) below, taken up front rather than held in reserve.
Consequence, accepted: **no sequencer-gated stutter-freeze in v1.** FREEZE is
hand-played. The engine API stays per-part and level-driven (`set_freeze(p,
on)`), so if the gates ever return, only `gen_panel.py` and one host line
change.

**Patch compatibility (hard requirement):** all new params appended at the **end** of their enums in `gen_panel.py` with explicit coordinates — never added to the `part_controls()` template (would shift part-B/SHARED ids and break existing `.vcv` patches). The one template edit, `TRIGGER_A/B` → `FRZ_A/B`, is a rename at a fixed index: the count is unchanged, so `PART_STRIDE` stays 23 and every id holds. Residue: a patch saved with TRIGGER held down would load with FREEZE engaged — momentary buttons are effectively never saved at 1, so this is accepted rather than mitigated.

**Sequencing vs. M5 sampler (agreed 2026-07-18):** DUST ships **before** the M5 sampler. Both specs append params at the end of `PARAMS`, so release order fixes the ids — DUST/ROT/FRZ first, then M5's REC. The two must not be developed in parallel branches touching `gen_panel.py`.

**No FX-target lane** (user decision): DUST/ROT are hand-played; rhythm comes from the sync zone alone. The 5-lane == pad-slot structure stays untouched.

**Hardware reducibility note:** net cost after the reductions above is **+4 small knobs and 0 new pads** across both parts (the FRZ pads reuse TRIGGER's slot, and the gate jacks are gone). Reduction ladder, with step (1) already taken: ~~(1) drop gate jacks~~ **done**, (2) one shared FRZ button, (3) ROT becomes a shared (both-parts) knob, (4) last resort: ROT fixed mid-travel and DUST alone — the engine API is per-part and per-axis regardless, so the panel can merge without engine changes.

### 7. Engine API + host plumbing

- `Flux::set_dust(float)`, `Flux::set_rot(float)`, `Flux::set_freeze(bool)`; `Flux` owns the `DustCloud`, passes tap access + delay time (sync grid) + collects writeback.
- `PartFx` / `Instrument` forwarding in the `set_flux_mix` pattern: `set_dust(p, v)`, `set_rot(p, v)`, `set_freeze(p, on)`.
- `host/render/scenario.cpp`: actions `set_dust`, `set_rot`, `set_freeze` + demo scenarios (`dust_stutter.json` — sync zone; `dust_erosion.json` — freeze + zone R).
- `host/vcv/src/Spotymod.cpp`: knobs/pad → setters; DUST tooltip in percent; ROT tooltip naming the zone ("SYNC / FREE / ROT"); FRZ read as a level (the latch state *is* the freeze state), and the old manual-trigger wiring removed.

### 8. CPU + memory budget

> **Rev 4 correction (2026-07-19). The estimate below this box was wrong by
> roughly 30×, and the feature is blocked on measurement.** The original text
> read "estimated ≪ 1 %". It counted arithmetic, and the arithmetic was right —
> what it missed is that the grain `read` is not a read. See the analysis that
> follows. **Do not execute the implementation plan until the `dust` bench
> family has run on hardware.**

- **Memory:** ~8 grain structs × 2 parts (< 1 KB). **No new audio buffers.** —
  this part of the original estimate stands and is the design's real strength.
- **CPU:** worst case 16 grains × (1 read + window + 2 mults); reverse costs one
  extra add. Writeback adds one `fast_tanh` per part-sample *only in zone R*;
  erosion one more *only while frozen*. No transcendentals otherwise in the hot
  path — the window is a table lookup (`hann_value_at`), and pan gains are
  computed once at grain birth, not per sample.
- DUST = 0 and no freeze: two branch checks. (Unchanged and free.)

**Why the operation count is the wrong unit here.** FLUX's tape is 262 144
floats per channel per part — 1 MB. It does not fit SRAM at any size, so every
grain read is a *scattered* access into SDRAM. The bench already prices exactly
this pattern, and has since before this spec was written:

| row | avg % | max % | anchored max |
|---|---:|---:|---:|
| `grain_read_sram` | 3.04 | 3.09 | — |
| `grain_read_sdram` | 16.07 | 16.84 | 17.43 |

**The region factor is 5.3×**, and that row measures *eight* grains. This spec
wants sixteen. Scaling it — with the caveat that the row reads interpolated
stereo where DUST reads one integer sample, which drops loads but not cache
lines, so the miss count is what carries over — puts the cloud at roughly
**24–35 points of the 960 000-cycle block budget**. Against "≪ 1 %".

For scale: the instrument currently sits at **97.69 % anchored max with 2.3
points of margin** (`docs/bench/2026-07-19-6e38090.md`). The entire remaining
ranked cut list — a global voice cap 8→5 (~13 points, itself a ceiling), the
`Svf` single-pass rework (~2–4), the `PartFx` rev-send `fast_sin` (~1–2) — comes
to about 19 points and **does not pay for this feature**. DUST as specified
would land the instrument near 125 %.

This is the same failure mode as the mod-plane design spec, which estimated
4–6 % of budget for a plane that measured 33 %, and as `util/fast_sin.h`'s
still-uncorrected "~10–15 cycles" comment against an empirical 50–65. Counting
operations predicts nothing on this chip when the operand is in SDRAM.

**The lever, and why the feature is probably still viable.** Cost is linear in
cache *misses*, not in grains. Grains confined to the most recent slice of tape
read the region the write head and echo head are already touching — that stays
resident, and the 5.3× region factor should largely collapse toward the
`grain_read_sram` figure. Zone F's "spray widens across the zone from ~150 ms up
to the full 5 s" (§3) is the expensive clause, and it is **a tuning constant,
not a structural requirement**. Bounding the spray buys back most of the cost
for a narrowing of one zone's reach.

That is a hypothesis, not a result. It is what the bench family exists to
settle.

**Required before implementation: the `dust` bench family**
(`bench/workloads_dust.cpp`, seven rows, added 2026-07-19). It runs the real
access shape — integer offsets, one channel per grain, Hann window, equal-power
pan, grain respawn — rather than a proxy:

| row | question it answers |
|---|---|
| `dust_16_full` | the headline worst case: 16 grains, full 5 s spray |
| `dust_16_win05` | does a 0.5 s spray window collapse the SDRAM factor? |
| `dust_16_win01` | the tight end — zone S reads near the head anyway |
| `dust_8_full` | grain count isolated from window width; also prices DUST's integer read against `grain_read_sdram`'s interpolated stereo read at equal grain count |
| `dust_16_rev` | reverse grains walk memory backwards — different prefetch behaviour, and zone R makes ~70 % of grains reverse |
| `dust_16_wb` | zone-R writeback: grain sum → `fast_tanh` → store |
| `dust_16_erode` | freeze + erosion: read-modify-write at the pointer |

**Decision rule.** If `dust_16_win05` lands in single digits, the feature ships
with a bounded spray and §3 zone F is retuned accordingly. If even the windowed
rows are expensive, the grain pool must shrink (cost is linear in it) or DUST
waits for headroom that does not currently exist. Either way the number comes
from the probe, not from this document.

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
- **`FRZGATE_A/B` gate inputs** — cut from v1 (decision 2026-07-18, see §6): the hardware will not have the jacks. With them go sequencer-gated stutter-freeze and performed erosion via a held gate. Reinstating them needs a `JACK_GROUPS` entry, the enum added to the hardcoded `IN` tuple in `jack_at()`, and one `pushParams` line — the jack strip has no room at today's 23.0 mm box width, so it would also need the strip constants retuned.
- Unfreeze write-in crossfade — only if the play-test hears a click.
- Pitch-shifted grains, half/double-speed heads, write-head skip chaos (GRIT overlap) — deliberately not in v1.
- Firmware (M6) wiring — engine code is target-agnostic like the rest of the FX layer.

## Files touched (anticipated)

- `engine/fx/dust.h`, `engine/fx/dust.cpp` — **new**: `DustCloud` (pool, zones, scheduler, window, reverse, writeback sum).
- `engine/fx/flux.h`, `engine/fx/flux.cpp` — `ReadTap`, freeze/erosion store, writeback input, dust integration, head takeover.
- `engine/fx/part_fx.h/.cpp`, `engine/instrument.h/.cpp` — setter forwarding.
- `host/vcv/res/gen_panel.py` + regenerated `Spotymod.svg` / `generated_panel.hpp` — `FX_BOT` widened to four slots, `TRIGGER_A/B` renamed to `FRZ_A/B` in the template, `DUST_A/B` + `ROT_A/B` appended.
- `host/vcv/res/test_panel.py` — `PARAM_ORDER`, `LOWER_A`, plus a guard that `PART_STRIDE` is still 23 and the new params are appended rather than templated.
- `host/vcv/src/Spotymod.cpp` — param config, DUST/ROT tooltips, FRZ latch read, manual-trigger wiring removed.
- `host/render/scenario.cpp` + `host/render/scenarios/dust_stutter.json`, `dust_erosion.json`.
- `tests/test_dust.cpp` (new), `tests/test_flux.cpp`.

# DUST / ROT — grain cloud, tape rot + erosion freeze on the FLUX echo

**Date:** 2026-07-18 (rev 3 — control surface reduced to what the hardware will
actually have, same day); **rev 6 2026-07-19 — CPU budget MEASURED + optimized, §8**
**Status:** design approved, **implementation BLOCKED on budget** — but the
number is now known and the shape of the fix with it. Optimized cost is **11.1
points at 16 grains, 6.2 at 8** against **~2.1 points of margin**. DUST ships
when headroom exists (the global voice cap 8→5 is the candidate, ~13 points as
a ceiling), and it ships at 8 grains unless a re-measure says otherwise. The
per-grain optimizations are specified in §8 and must go into `DustCloud` when
the plan is eventually executed. See §8.
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

- `engine/fx/flux.h` — `EchoDelay` over `DeLine` rings (`kMaxSamples` = **262 144 samples ≈ 5.46 s** per channel per part; this spec was written against the older 240 000 / 5.0 s figure, raised to a power of two by `7e9f924` so the ring index wraps by AND mask — the "5 s" quoted elsewhere in this document is approximate, and code must use the symbol). The write pointer decrements once per sample; a read at constant offset behind it **is** 1× forward playback delayed by that offset. `Flux` already receives BPM and knows `_delay_time`.
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
- Level: grain sum normalized by **`1/sqrt(active grain count)`** — the count
  actually sounding, smoothed, **not** the expected overlap. *(Corrected
  2026-07-19, measured.)* The original `1/sqrt(expected overlap)` fails at the
  top of the knob: at DUST = 1 the tuning offers ≈8.4 grains to a pool of 8, and
  a loss system at full utilisation drops ~26 % of arrivals (Erlang-B). Dividing
  by a load the pool cannot deliver made the cloud **quieter** at maximum DUST —
  energy ∝ carried/offered = 0.74, measured 0.68 — which would have read as a
  dead range at the top of the sweep. Normalising against what is actually
  sounding holds the level flat across the knob, which is what this
  normalisation was always for: **DUST changes texture and density, not
  loudness.**
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

> **Rev 6 (2026-07-19) — measured, then optimized and re-measured.** The
> original estimate here read "≪ 1 %". The specified 16-grain cloud measures
> **17.2 % of block budget** naive and **11.1 % after an optimization pass**
> (13.3 % with erosion); at 8 grains, **6.2 %**. The rev-4 correction that caught
> the original error guessed 24–35 points on a cache-miss argument and was itself
> wrong — **on the number and, more importantly, on the mechanism**: grain reads
> are nearly free, and the bounded-spray fix rev 4 proposed buys about one point.
> The cost is per-grain arithmetic, and a third of it came off. Findings below;
> the live decision is now the **pool size**, not the spray.

- **Memory:** ~8 grain structs × 2 parts (< 1 KB). **No new audio buffers.** —
  this part of the original estimate stands and is the design's real strength.
- **CPU:** worst case 16 grains × (1 read + window + 2 mults); reverse costs one
  extra add. Writeback adds one `fast_tanh` per part-sample *only in zone R*;
  erosion one more *only while frozen*. No transcendentals otherwise in the hot
  path — the window is a table lookup (`hann_value_at`), and pan gains are
  computed once at grain birth, not per sample.
- DUST = 0 and no freeze: two branch checks. (Unchanged and free.)

**MEASURED 2026-07-19** (`docs/bench/2026-07-19-12f05ce.md`, `dust` family,
seven rows). The estimate this box replaced said "≪ 1 %". The correction that
replaced *it* said 24–35 points, on a cache-miss argument. **Both were wrong,
and the second was wrong about the mechanism, which matters more than the
number.**

| row | avg % | max % |
|---|---:|---:|
| `dust_16_full` | 17.19 | **18.54** |
| `dust_16_win05` | 16.77 | 17.43 |
| `dust_16_win01` | 16.86 | 17.65 |
| `dust_8_full` | 8.94 | 9.31 |
| `dust_16_rev` | 17.12 | 17.91 |
| `dust_16_wb` | 18.67 | 19.95 |
| `dust_16_erode` | 19.91 | 21.48 |

**Finding 1 — the spray window buys nothing. Hypothesis refuted.** The widest
row measured, `dust_16_full`, sprays 240 000 of the tape's 262 144 samples —
**91.5 % of the tape** (`Flux::kMaxSamples`, ~5.46 s @ 48 kHz), not the "full
5 s" this row and its bench constant were labelled at the time; both were a
bare literal that undercounted the real buffer, fixed 2026-07-19 alongside
`fx/dust.cpp`'s equivalent `tape_max_s` mistake. That row costs 18.54 %, a
0.5 s window 17.43 %, a 0.1 s window 17.65 % — the narrowest is *worse* than
the middle one, so the whole spread is noise around roughly one point. A 0.1 s
window is 19 KB and fits the 32 KB D-cache outright; if misses drove the cost,
that row would have collapsed. It did not. **§3 zone F keeps its full tape
reach (~5.46 s) — there was never anything to buy by giving it up, and the
91.5 %-vs-100 % gap in what was actually measured does not change that: the
narrowest row here already refutes the hypothesis on its own.**

**Finding 2 — grain reads are nearly free, and `grain_read_sdram` was a bad
proxy.** `dust_8_full` (9.31 %) costs **45 % less** than `grain_read_sdram`
(16.82 %) at the same grain count, while doing strictly more arithmetic. The
reason is the access shape: that proxy re-randomizes its index every sample, so
it is 8 genuinely random accesses per sample. **A DUST grain is not a random
access — it is a read head.** Born at a random offset, it then walks linearly
(offset += 0 forward, += 2 reverse), so consecutive samples read adjacent
addresses. Sixteen grains are sixteen sequential streams, which is the
prefetcher's best case; only the *birth* is a jump, amortized over 1 200–19 200
samples of grain life. The 5.3× SDRAM region factor never applied here.

**Finding 3 — the cost is arithmetic, and it is linear in grain count.** 8→16
grains scales 1.99× with no cliff anywhere. That works out to **~116 cycles per
grain per sample**, which is far more than "1 read + window + 2 mults" should
cost. The bench loop is the explanation and is honest about it: it computes
`age / length` as a **float division per grain per sample**, then calls
out-of-line `hann_value_at`, which does its own multiply, float→int cast, bounds
compare and lerp. **So these rows are an upper bound on a naive implementation,
not a floor** — a phase accumulator stepped by a reciprocal computed at birth,
plus an inlined window read, should take a real bite out of 116 cycles. How
much is unmeasured, and this spec has now been wrong twice by reasoning instead
of measuring, so: unmeasured means unknown.

**Finding 4 — writeback and erosion are cheap, reverse is free.** Zone-R
writeback adds 1.4 points over plain, erosion 2.9. Reverse grains cost nothing
measurable (17.91 vs 18.54, inside the noise) — the +2 stride is as
prefetch-friendly as +1.

**Finding 5 — the optimization pass returns 35 %, measured**
(`docs/bench/2026-07-19-dccb8a3.md`, three `*_opt` rows measured in the *same
run* as the naive ones, which held their checksums exactly — the layout did not
shift, so the comparison is clean):

| row | avg % | max % | vs naive (avg) |
|---|---:|---:|---:|
| `dust_16_opt` | **11.13** | 13.35 | −35.3 % |
| `dust_8_opt` | 6.16 | 6.93 | −31.0 % |
| `dust_16_opt_erode` | 13.29 | 15.02 | −33.3 % |

Three costs were removed without changing what a grain is: the per-sample
`age / length` division became a window index stepped by an increment fixed at
birth; `hann_value_at`'s out-of-line call — whose `hann_curve()` is a
**function-local static, so every call paid a thread-safe-init guard check** —
became a table pointer hoisted out of the block loop; and the per-sample
`(g_write + offset) & mask` plus L/R select became an absolute read index
stepped by `(delta − 1)` with the tape pointer resolved at birth.

**107.6 → 69.6 cycles per grain per sample.** Decomposed, the optimized cloud is
**62 cycles per grain per sample marginal plus 1.2 points fixed** (the tape
store stream and loop overhead, now a visible share because the grains got
cheap), and **erosion adds 2.16 points as a per-sample, not per-grain, cost**.
That decomposition is what lets the pool size be priced without another run.

**Where that leaves the feature.** The instrument sits at **97.83–97.98 %
anchored max** with ~2.1 points of margin (same run). Against that:

| configuration | cost | verdict |
|---|---:|---|
| 16 grains, optimized | 11.1 avg / 13.4 max | does not fit, even with the voice cap |
| 16 grains + erosion | 13.3 / 15.0 | does not fit |
| **8 grains, optimized** | **6.2 / 6.9** | fits *only* with headroom bought elsewhere |
| 8 grains + erosion | ~8.3 (projected) | as above |

The global voice cap 8→5 is worth ~13 points as a ceiling — and per the FLUX
precedent (5.80 predicted solo, 3.56 returned in context) the in-context return
could be nearer 8. **8 grains plus the voice cap fits with room to spare;
16 grains plus the voice cap lands at the edge with no margin at all.** So the
pool constant in §1 is the live design decision, and it is a musical one: 4
grains per part instead of 8 halves the maximum overlap density, which is
exactly what zone F's "full 8-voice overlap" (§2) trades on.

What survived contact with the probe is the design's premise: **no new audio
buffers, < 1 KB of state, and grain reads that cost almost nothing.**

**The `dust` bench family** (`bench/workloads_dust.cpp`) stays in the tree as
the measuring instrument: `dust_16_opt` is the headline, `dust_8_opt` prices the
pool decision, `dust_16_opt_erode` the freeze path. The naive rows stay as the
baseline the optimization is measured against, and the two window rows have done
their job and can go if the table gets crowded.

**A measurement note for whoever runs this next:** `dust_16_full`'s *avg* held
to within 200 cycles across the 12f05ce and dccb8a3 builds (165 100 → 165 252)
while its *max* moved 6 % (178 053 → 188 764). For these component rows compare
**avg across runs and max within a run** — the max carries the cross-build
icache layout shift this bench has seen before. `instrument_worst`'s max stays
the gate; that is a different, much larger row.

**Finding 6 — folding DUST into the GRIT selector is worth 3.2 points, and
that is not enough.** The proposal (2026-07-19): make GRIT a three-way choice
per part — Drive / Reduce / **Dust** — instead of adding DUST as a fourth FX
block. Mutually exclusive means the worst case pays `max(GRIT, DUST)` rather
than `GRIT + DUST`, so GRIT's in-context cost *is* the saving. That number was
unmeasured, and the two available proxies disagreed by 57 %, so
**`inst_worst_nogrit`** now measures it directly (`abl` family, GRIT off on both
parts, everything else at worst case):

| | avg | max |
|---|---:|---:|
| `instrument_worst` | 882 312 | 934 199 |
| `inst_worst_nogrit` | 851 234 | 900 102 |
| **GRIT, both parts** | **3.24 %** | **3.55 %** |

*Proxy post-mortem, because this repo keeps paying for proxies:*
`grit_drive_solo × 2` predicted 3.06 — **off by −5 %, essentially right**.
`fx_grit − fx_none` predicted 4.81 — **off by +48 %**, and the mechanism is
visible in `part_fx.cpp:29`: the FX block is guarded by
`if (_grit.engaged() || _flux.engaged())`, so `fx_none` skips the *entire*
chain — smoothed target reads, the FLUX call, the dry/wet crossfade — not just
Grit. That delta was never GRIT alone. Note this cuts against the FLUX
precedent, where the solo row *over*-predicted: there is no general law that
solo rows over- or under-state. Each block has to be checked.

With the measured figure, against `instrument_worst`'s 97.31 % offline max:

| merged configuration | arithmetic | result |
|---|---|---:|
| 8 grains | 97.3 − 3.6 + 7.0 | 100.7 % ✗ |
| 8 grains + erosion | 97.3 − 3.6 + 9.1 | 102.9 % ✗ |
| 16 grains | 97.3 − 3.6 + 13.3 | 107.1 % ✗ |

**The merge alone does not land it — it misses by about one point at 8 grains.**
An earlier estimate in this conversation said it would land at 99.3 %, using the
4.81 proxy; that estimate was wrong for exactly the reason the post-mortem
above gives. What closes the remaining gap is the `Svf` single-pass rework
(~2–4 points, and musically free — `Voice::process` reads only `_filt.Low()`
while DaisySP's `Svf` is double-sampled and computes five outputs):
**merge + 8 grains + Svf ≈ 96.7–98.7 %**, which fits without spending a single
voice. With erosion engaged it is 98.9–100.9 % — borderline, and the case to
re-measure rather than argue.

The merge's non-CPU consequences belong in §6 if it is adopted: GRIT's existing
intensity knob can carry the DUST amount, so the panel cost falls from +4 small
knobs to +2 (only ROT per part) — reduction-ladder step (3), taken for free. The
musical price is that GRIT and DUST become exclusive, and they sit at *different*
points in the chain: GRIT is saturation **before** the tape, DUST reads **from**
it. Zone R's self-eating tape is exactly where drive in front would be
idiomatic, and the merge forecloses it. That is a taste decision, not a budget
one, and it is not settled here.

**Decision rule, settled.** DUST is unblocked when headroom exists for it —
today it does not. When it does, it ships at **8 grains** unless a re-measure
shows the margin can carry 16. The pool constant is the knob; cost is
near-linear in it, which is now proven rather than argued. The cheapest known
route to the headroom is **GRIT-selector merge (3.2) + `Svf` single-pass
(2–4)**, which together cost no polyphony; the global voice cap 8→5 (~13 as a
ceiling) is the fallback if that route falls short.

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

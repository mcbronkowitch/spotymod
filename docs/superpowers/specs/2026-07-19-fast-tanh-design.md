# Fast tanh — the echo loop and the master limiter off libm

- **Status:** implemented and measured at `87f3538` — see `## Outcome (2026-07-19)`
- **Date:** 2026-07-19
- **Branch:** `cpu-hunt`
- **Baseline:** `94468af` (`docs/bench/2026-07-19-94468af.md`)
- **Predecessor:** `2026-07-19-mod-plane-control-rate-design.md`

## Context

At `94468af` the full instrument's worst case measures **97.60 % (avg) / 104.06 %
(max)** of the 960 000-cycle block budget, anchored inside a real audio callback.
The average is under budget for the first time; the max is not, and the max is
the gate — a worst case that fits on average still drops blocks. **Roughly 4
points remain.**

`std::tanh` is the last libm call left in the per-sample audio path. It has two
call sites:

- `engine/fx/flux.h:119` — `EchoDelay::Process`, once per sample per channel,
  two parts stereo = 384 calls per block.
- `engine/fx/limiter.h:60` — `Limiter::shape`, on both channels whenever the
  signal exceeds the knee.

The roadmap's ranked cut list puts the `EchoDelay` site at a ceiling of ≈8 points
and the limiter at ≈3. **Both figures are under caveat:** they were derived from
`abl` rows measured at `9be5df9`/`c7f6a73`, and the `94468af` run showed that
attribution has drifted (`part_glue_flow` halved a second time after the raster
tick landed, cost the ablation had booked to the glue). The estimates are the
reason to try this cut, not a promise of its size.

This mirrors the `wave_sine` → `fast_sin` cut from
`2026-07-18-mod-plane-optimization-design.md`: the audio path has used the cheap
`fast_sin` polynomial since M2 while these two sites still called libm.

## Decision

Add `engine/util/fast_tanh.h` — a **clamped Padé [5/4] rational approximation** —
and use it at both call sites.

```
              x · (945 + x²·(105 + x²))
fast_tanh(x) = ─────────────────────────    for |x| < 3.646739
              945 + x²·(420 + 15·x²)

             = ±1                            for |x| ≥ 3.646739
```

### Why this approximation

Candidates were measured numerically over [−8, 8] before choosing:

| approximation | max abs error | max\|f\| | ops | est. cycles (M7) |
|---|---:|---:|---|---:|
| `x/√(1+x²)` | 0.0737 | 0.9923 | 1 sqrt, 1 div | ~28 |
| Padé [3/2], clamped at 3 | 0.0235 | 1.0000 | 1 div, 3 mul, 2 add | ~22 |
| **Padé [5/4], clamped at 3.646739** | **0.0014** | **1.0000** | 1 div, 5 mul, 3 add | ~30 |
| `libm tanhf` (current) | — | <1 | — | ~200 |

[5/4] is **17× more accurate than the usual [3/2] for ~8 extra cycles**. Across
384 calls per block that is ~0.32 points — noise against the ~7 points the cut is
expected to return. The accuracy is bought for a specific reason: at `set_feedback`
1.2 the echo self-oscillates, and in a feedback loop a curve error compounds over
repeats. A 2.4 % deviation ([3/2]) would move the limit cycle audibly; 0.14 %
([5/4]) does not.

`x/√(1+x²)` is rejected on both axes — three times the error of [3/2] at higher
cost — and its asymptote never reaches 1.0, which would pull the limiter's
ceiling below full scale.

### The three properties that are contract, not cosmetics

1. **`|fast_tanh(x)| ≤ 1.0`, for every finite input.** This is what bounds the
   echo feedback loop at coefficient 1.2 and what defines the master limiter's
   ceiling. The unclamped Padé does not have it (it runs to `x/15` for large x
   and would let the loop diverge) — but the threshold alone does not
   guarantee it either: `3.646739f` sits 4.1e-7 above the true root
   (3.6467385950…), so a 9.3e-6-wide band of floats just below the threshold
   evaluates the raw rational form up to 1.19e-7 above 1.0, found by
   exhaustive float32 enumeration, not by the sweep below (its 8e-5 step is
   8.6× wider than the band, so it steps straight over it — the earlier
   "Verified: max|f| = 1.0000000000 over the sweep" claim was a grid artifact,
   not a measurement). The bound is now enforced on the return value itself
   (two compares), so it holds regardless of the threshold's rounding.
2. **Monotonic** over the whole range. A non-monotonic saturator folds the
   transfer curve and produces artifacts in the loop that read as a bug.
3. **`f'(0) = 1` exactly.** `Limiter::shape` is C1-continuous at the knee only
   because `tanh'(0) = 1`; any approximation that misses this puts a slope
   discontinuity at the knee, audible as the DRIVE curve "catching".

### Accepted deviation from `tanh`

The clamped form **reaches** ±1 at |x| ≥ 3.646739, where true `tanh` only
approaches it. In the echo at extreme feedback this is a marginally harder cap on
the limit cycle. At 0.14 % curve error and a clamp point the signal only reaches
with the bloom fully open, this is expected to sit below the audibility
threshold — but it is the specific thing to listen for in the listening pass.

At the clamp point the raw [5/4] slope is ~0.005, so the join is very nearly C1;
the residual kink is three orders of magnitude below the signal it acts on.

## Design

### `engine/util/fast_tanh.h`

Header-only, `namespace spky`, no libm call. One shared implementation so desktop
renders and firmware output stay bit-identical — the same rule `fast_sin.h`
states for itself.

```cpp
inline float fast_tanh(float x) {
    const float ax = x < 0.f ? -x : x;
    if (ax >= 3.646739f) return x < 0.f ? -1.f : 1.f;
    const float x2 = x * x;
    return x * (945.f + x2 * (105.f + x2))
             / (945.f + x2 * (420.f + 15.f * x2));
}
```

### Call site 1 — `engine/fx/flux.h:119`

`out = std::tanh(out)` → `out = fast_tanh(out)`. Topology unchanged; the comment
("tape-warm limiter: transparent near unity, bounded self-oscillation above it")
still holds and gains a note that the bound is now a hard clamp.

### Call site 2 — `engine/fx/limiter.h:60`

`std::tanh(...)` → `fast_tanh(...)`. **The bit-exact paths are untouched** — the
transparent early return (`limiter.h:42`) and the below-knee identity
(`limiter.h:59`) both sit *before* the call. Drive 0 below the knee stays
byte-identical to `94468af`.

### What stays bit-identical, what changes

- **Bit-identical:** the limiter's transparent path (drive 0, below knee); every
  part of the engine that does not run FLUX or a driven master.
- **Changes deliberately:** the echo's saturation curve and the driven limiter's
  warm curve. Consistent with the project's standing position that byte-identity
  is not a gate (`renders/` references are re-cut, not defended).

## Testing & acceptance

New `tests/test_fast_tanh.cpp`, modelled on `tests/test_fast_sin.cpp`, plus its
entry in the explicit source list in `CMakeLists.txt` (there is no glob).

1. **Accuracy** — max abs deviation from `std::tanh` < 2e-3 over a dense sweep of
   [−8, 8].
2. **Boundedness** — `|fast_tanh(x)| <= 1.0f` across the sweep *and* at extremes
   (±1e6, ±1e30). This is the stability contract; it is asserted as `<=`, not an
   epsilon comparison.
3. **Monotonicity** — non-decreasing across the whole sweep.
4. **Odd symmetry** — `fast_tanh(-x) == -fast_tanh(x)` exactly.
5. **Origin behaviour** — `fast_tanh(0) == 0.f` exactly; central-difference
   derivative at 0 within 1e-4 of 1.0.

Existing tests that must stay green — these, not the unit tests, are the real
safety net:

- `tests/test_flux.cpp:114` — "feedback at max blooms but stays bounded"
  (fb = 1.2, self-oscillating). If the clamp were wrong this diverges.
- `tests/test_flux.cpp:148` — "feedback below unity decays to silence".
- `tests/test_limiter.cpp` — the bit-exactness cases for the transparent path.

Full suite expected green (333 cases at `94468af`, plus the new file's).

### Re-pinning

`host/render/scenarios/ctrl_identity.sha256` must be re-pinned, using the same
procedure as `2026-07-19-mod-plane-control-rate.md` Step 1: render, hash,
overwrite in the existing file's format, then re-render and confirm the pin
matches.

### Hardware acceptance

Order matters — `bench/run.py` names result files by HEAD hash, so the tree must
be clean before measuring:

1. Suite green.
2. Commit.
3. `python bench/run.py`.
4. Read `instrument_worst` **anchored max** against 100 %.

The run sweeps the `abl` family too, so it also re-baselines the drifted
attribution flagged in the roadmap at no extra cost.

**Outcome branches:**

- **Under 100 % anchored max** — the 2×4 budget question flips for the first
  time. Record it; the listening pass then gates the merge to `main`.
- **100–102 %** — next candidate is `PartFx` rev-send `std::sin` → `fast_sin`
  (≈1–2 points, same technique, same listening pass).
- **Materially above** — the ~8-point estimate was wrong. Go back to measurement
  (re-derive the `abl` attribution) rather than spending another guess.

## Non-goals

- **`engine/center/center.cpp:234`** (`_weather = std::tanh(_ou)`) — control rate,
  not per sample. The saving is negligible and it would change DRIFT behaviour for
  nothing.
- **Restructuring the echo topology** — the nonlinearity stays inside the loop,
  where it is the feature.
- **`renders/` refresh and the VCV plugin rebuild** — the user's listening pass,
  bundled with the one already pending from the mod-plane cut.
- **Chasing the `[3/2]` variant's 0.32 points** — considered and rejected above.

## Outcome (2026-07-19)

**The shipped figure is `6e38090`: anchored max 97.69 %.** The gate cleared —
under 100 % for the first time — but the number below the fold is not the one
this section originally recorded, and the difference is worth reading before the
tables. The cut was first measured at `87f3538` at **95.77 %**; a subsequent
correctness fix to `fast_tanh` (see *The bound fix and what it cost*) gave back
1.9 of those points. Both measurements are real; `87f3538` is simply not what
ships.

The analysis that follows is written against the `87f3538` run, because that is
the run that isolates what the *cut* bought. The bound fix is a separate,
later change and is accounted for separately.

**Measured at `87f3538`** on a clean tree (`docs/bench/2026-07-19-87f3538.md`),
against the `94468af` baseline.

| `instrument_worst` | `94468af` | `87f3538` | delta |
|---|---:|---:|---:|
| offline avg | 96.59 % | 90.67 % | −5.92 pts |
| offline max | 103.63 % | 95.61 % | −8.02 pts |
| anchored avg | 97.59 % | 90.91 % | −6.68 pts |
| **anchored max** | **103.89 %** | **95.77 %** | **−8.12 pts** |

Avg and max are separate claims and both moved, but only the second one is the
verdict. The average had already been under budget at `94468af`; what this cut
bought is the max, and it is the max that decides whether a worst-case block
gets dropped. The second capture repeat of this run read 95.85 % anchored max,
a same-binary, same-layout repeat seconds apart, so that specific comparison
resolves to about ±0.1. That is not the band that governs whether the next
build still fits, though: this same document's re-baselined `abl` family
below puts this run pair's cross-build noise floor on solo rows at ~2 %
(`oliverb_solo_sram` moved −2.1 % with no reverb code change, in-context
reverb swung ~10 %) — at ~2 % of the 95.77 reading that is ~1.9 points,
roughly 2× the noise against the 4.2-point margin this run showed, not forty.
(Against the shipped `6e38090` margin of 2.3 points the cross-build band is
larger than the margin itself — see *The bound fix and what it cost*.)

One bookkeeping note on the baseline: this spec's Context section quotes the
`94468af` anchored pair as 97.60 / 104.06, while the committed bench report
for that same commit records 97.59 / 103.89. Both are true — they
are the two capture repeats of one run, and the earlier prose took its numbers
from the second. The table above uses the committed report. Against the 104.06
reading the saving is −8.29 pts instead of −8.12; nothing about the verdict
depends on which one is used.

### Predicted vs realized

**The prediction was too optimistic, and both call sites underdelivered.** The
ranked cut list put `EchoDelay` at ≈8 points and the limiter at ≈3, a combined
≈11. The cut returned **8.12**, about three quarters of that. Isolating the two
sites in the same run shows where the shortfall sits:

| site | predicted | realized | evidence |
|---|---:|---:|---|
| `EchoDelay::Process` | ≈8 pts | **5.80 pts** | FLUX solo over the FX-none shell, 65 953 → 38 099 cycles/part, ×2 parts |
| `Limiter::shape` | ≈3 pts | **1.53 pts** | driven-limiter tax (`limiter_driven − limiter_clean`), 27 732 → 13 020 cycles |
| sum of the two | ≈11 pts | 7.33 pts | — |
| `instrument_worst` anchored max | — | **8.12 pts** | ground truth, not a sum |

Some of the gap was structural and foreseeable: those were *ceilings*, derived
by booking the entire measured cost of a call site to `tanh`, and `fast_tanh` is
not free — roughly 30 cycles against libm's ~200, so about 15 % of the cost was
always going to survive. That accounts for a point or so. It does not account
for the limiter's realized saving being **half** its prediction: the ≈3-point
figure was the whole driven-limiter tax, and most of what remains after the swap
is gain-riding arithmetic that has nothing to do with `tanh`. The estimate
attributed to the nonlinearity work that was never the nonlinearity's.

The correct way to record this is that the ablation was **right about rank order
and wrong about magnitude** — `EchoDelay` really was the bigger of the two, but
the ratio between them did not hold: realized ~3.8× (5.80 : 1.53) against a
predicted ~2.7× (8 : 3), about 42 % wider than the ceiling implied, and both
absolute figures were high besides. The cut cleared the gate because only ~4
points stood between the baseline and budget, not because the estimate was
good.

Two secondary rows show the kernel-level effect plainly, unmixed with the rest
of the instrument: `echo_short_sram` fell 21 154 → 8 752 cycles (−58.6 %) and
`echo_short_sdram` 23 012 → 10 607 (−53.9 %). The echo kernel itself more than
halved; it is the instrument around it that dilutes that into 5.8 points.

The in-context FLUX delta (`instrument_worst − inst_worst_noflux`) fell 133 824
→ 99 657 cycles, a saving of only 3.56 points against the 5.80 the solo rows
predict for two parts. That direction is consistent with the previously measured
negative `coupling_flux`: FLUX in context costs less than FLUX alone, so cutting
it also returns less. It is a caution for the next estimate — a solo-row saving
is an upper bound on what the composed instrument will hand back.

### What the re-baselined `abl` family says about the drift

The roadmap flagged the ablation attribution as untrustworthy after
`part_glue_flow` halved a second time at `94468af` (19.86 % → 9.97 %), post-dating
the Part-glue cut that should have already collected that saving. **This run
clears the flag.** `part_glue_flow` reads 95 842 cycles against `94468af`'s
95 774 — a 0.07 % move across an independent build, with an identical checksum.
Two consecutive runs now agree, so the second halving was a one-time
re-attribution when the 96-sample raster tick landed (cost the ablation had been
booking to the glue, correctly reassigned once the tick existed), not ongoing
instability in the family.

Every row the cut should not have touched held: `grit_drive_solo` 14 628 →
14 628 exactly, `synth_4_voices` 187 380 → 187 379, `mod_plane_2x_center` 56 667
→ 56 637, `micro_sinf` / `micro_powf` / `micro_fast_sin` flat, and `micro_tanhf`
— the untouched libm control — 20 081 → 19 852 (−1.1 %, jitter). The widest
untouched move is `oliverb_solo_sram` at −2.1 % with no reverb code change,
which is the cross-build layout shift the report's own precision note warns
about; treat ~2 % as this run pair's noise floor on solo rows.

The checksum column corroborates the confinement independently of the timings.
Exactly the rows that run FLUX or a driven limiter changed hash
(`fx_flux_sdram`, `instrument_worst`, all three `inst_worst_*` — they keep the
driven master — `limiter_driven`, both `echo_short_*`), and every other row is
byte-identical. The curve moved where the spec said it would and nowhere else.

Two ablation figures worth carrying forward, now that the family is trustworthy
again:

- **CHOKE is still not a worst-case axis.** The choke tax reads 40 023 cycles of
  *reduction* (`instrument_worst − inst_worst_choked`), against 39 553 at
  `94468af`. Stable across runs, and settled well below the 94 293 measured at
  `9be5df9` — the earlier figure belongs to a pre-cut instrument and should not
  be quoted any more.
- **In-context deltas carry much wider error bars than solo rows.** In-context
  reverb reads 104 255 cycles against the baseline's 115 544 despite zero reverb
  code change — an 11 289-cycle (~10 %) swing that is pure composition and
  layout noise. Any future prediction built on an `inst_worst_no*` difference
  should be quoted with that band, not to the cycle.

### The bound fix and what it cost

The final whole-branch review found that `|fast_tanh(x)| ≤ 1` — the property
three sections above call load-bearing — **was false**. The clamp constant
`3.646739f` sits 4.1e-7 *above* the true root (3.6467385950), leaving a
9.3e-6-wide band of floats just below the threshold where the raw rational form
evaluates up to 1.19e-7 over 1.0. Worse, the guard test could not have found it:
its uniform sweep steps 8e-5, 8.6× wider than the band, so zero sweep points
land inside. The "Verified: `max|f| = 1.0000000000` over the sweep" claim this
document previously made was a grid artifact, not a measurement.

`deb796f` enforces the bound on the **return value** instead of the threshold,
so it holds regardless of the constant's rounding, FMA contraction under
`-ffast-math`, or the target's division semantics — a constant validated on
desktop clang/x86 does not carry to the M7. Verified by exhaustive float32
enumeration over [0, 8) in both signs, under `-O2` and `-O2 -ffast-math`: zero
violations.

**It cost 1.9 points**, against an estimate of 0.08:

| `instrument_worst` anchored | `87f3538` | `6e38090` | delta |
|---|---:|---:|---:|
| avg | 90.91 % | 92.67 % | +1.76 pts |
| **max** | **95.77 %** | **97.69 %** | **+1.92 pts** |

This is not cross-build noise, and the row pattern proves it: rows that do not
call `fast_tanh` are flat (`synth_4_voices` 0.0 %, `oliverb_solo_sram` −0.1 %,
`fx_none` −1.3 %) while every row that does rose — `echo_short_sram` +16.5 %,
`limiter_driven` +8.3 %, `fx_flux_sdram` +6.2 %, `instrument_worst` +2.0 %
offline and anchored alike.

The mechanism is not branches: disassembling for `cortex-m7 -O3 -ffast-math`
shows the clamp compiled branchless, as `vmaxnm.f32` + `vminnm.f32`, two
instructions. The likeliest explanation is register pressure — two more live
constants inside a loop that already holds the delay line and the band-pass
state, paying for spills rather than for the compare itself. That was not
chased further: the alternative (lowering the threshold below the root and
dropping the clamp) trades a guaranteed bound for a ~5e-6 join step in a
saturator nobody has listened to yet, which is the worse bargain.

A related caveat found while verifying the fix, recorded because the contract
overstates itself: **monotonicity holds only to within float rounding.** 0.44 %
of adjacent float32 pairs decrease, the largest by 3.6e-7 (~6 ULP), the first at
x = 0.0019 — ordinary rounding noise in a float division, 3800× below the
function's own 1.36e-3 approximation error, and zero decreasing steps on a 1e-4
grid. `std::tanh` behaves the same way. Nothing folds; the word "monotonic"
simply cannot mean bit-exact ordering for a function of this shape.

### Where this leaves the budget

The 2×4 architecture fits, on this worst case, with **2.3 points of margin** on
the anchored max at `6e38090` (it was 4.2 before the bound fix) — and "this
worst case" is a specific one: `instrument_worst`
runs GRIT in Drive mode on both parts, not Reduce. Reduce costs 25 051 cycles
against Drive's 14 628 (2.60 % vs 1.52 % of budget per part), a delta of 1.08
points per part, **~2.2 points across both parts — which is almost the entire
2.3-point margin.** Redefining the worst case to use Reduce would put the
instrument back at the edge of the budget. `bench/report.cpp` now emits *"the
2×4 architecture fits"* on its own, which is the first time it has done so. That
is a real milestone and it is a thin one: the margin is well under half the 5.8
points this cut returned from its larger site, and a single unbudgeted feature
can spend all of it. The remaining
ranked candidates (`PartFx` rev-send `std::sin` → `fast_sin`, ≈1–2 points; the
double pitch quantization in `Part::process`) are no longer needed to clear the
gate and should be held as margin rather than spent.

The merge to `main` stays gated on the listening pass. Two specific things to
listen for: the echo bloom at maximum feedback, where the clamp caps the limit
cycle marginally harder than `tanh`'s asymptote did; and master DRIVE at high
settings, where `Limiter::shape` scales `fast_tanh`'s curve error by
`(1 - knee)` — the error on the bus reaches ~1.5e-4 (≈ −76 dBFS) at drive 0 and
~7.5e-4 (≈ −62 dBFS) at full drive, five times larger and on the summed master
rather than one FX return.

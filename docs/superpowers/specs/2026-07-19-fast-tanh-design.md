# Fast tanh — the echo loop and the master limiter off libm

- **Status:** accepted, unimplemented
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

1. **`|fast_tanh(x)| ≤ 1.0` strictly, for every finite input.** This is what
   bounds the echo feedback loop at coefficient 1.2 and what defines the master
   limiter's ceiling. The clamp guarantees it *hard*; the unclamped Padé does not
   (it runs to `x/15` for large x and would let the loop diverge). Verified:
   `max|f| = 1.0000000000` over the sweep.
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

# Generous Parameter Ranges in the Texture Deck — Design

**Date:** 2026-07-21
**Branch:** `sampler-deck` (M5a, still unmerged — this work lands on the same branch)
**Supersedes nothing.** Amends `2026-07-18-sampler-texture-deck-design.md`, whose
open MOTION/fog question this design is expected to answer as a side effect.

---

## Why

Spotykach is meant to be an instrument with experimental range, not a safe one.
The FX already behave that way and have for some time:

- The echo runs feedback to **1.2** — `Flux::set_feedback` clamps the knob to
  `[0,1]` and then scales by `1.2f` (`engine/fx/flux.cpp:53-58`). The loop stays
  bounded only because `fast_tanh` sits inside it; `engine/util/fast_tanh.h:14-21`
  states this as a contract in so many words.
- The reverb runs decay past unity to **1.05** (`engine/fx/reverb.cpp:51-56`),
  with an 80 Hz high-pass pinned inside the tank specifically so the bloom does
  not accumulate DC (`reverb.cpp:14,30`).

The sampler is the outlier. Its record feedback is clamped to `[0,1]` and mapped
onto a −60…0 dB curve that reaches exactly unity and never passes it
(`engine/sampler/sample_buffer.cpp:35-40`). Several of its other ceilings are
taste with no technical backing, and one of them — the position-scatter ceiling —
is a plausible cause of the fog gap recorded as open in the M5 spec.

This design brings the deck in line with the rest of the instrument.

## The governing rule

> A ceiling stays only if it prevents a **failure** — an out-of-range index, a
> division by zero, a NaN, filter instability, or a per-sample cost the hardware
> cannot pay. A ceiling that only prevents an **unpleasant result** goes.
> Where opening a path lets it diverge, add the bounding nonlinearity the
> instrument already has. Do not make it narrower; make it bounded.

Corollary, and the reason this direction is cheap: clamping back later is a
one-line change to a constant. Nothing here is a compatibility surface. The two
gates this project actually enforces — **synth neutrality** (pinned synth
scenarios render byte-identical) and **determinism** (a double render is
byte-identical) — are untouched by sampler-side range changes, and both must
still pass unchanged. Sampler renders are expected to change; they are sanity
checks, not baselines.

There is precedent for the inverse judgement too, and it is worth keeping in
view: `engine/fx/taps.cpp:42-45` refuses to clamp overlapping tap offsets and
mutes the tap instead, because *"clamping would put two taps at the same
position, turning a missing echo into a doubled one."* Clamping is not
automatically the safe choice.

## Shape of the change: "on top, not instead"

Every remapped control keeps its existing curve over the travel that has already
been listened to, and opens into new territory at the ends. The familiar sound
stays at the knob position where it has always been; the extreme is a journey,
not a new resting state.

---

## 1 — Record feedback above 100 % (the earthquake)

`SampleBuffer::set_feedback` gains a knee at knob position **0.9**. Below the
knee the mapping is **unchanged**: position `n` gives `−60 + 60·n` dB exactly as
today, so 0.9 still gives −6 dB. Above the knee, travel from 0.9 to 1.0 runs from
−6 dB on to **≈ +2.5 dB** (linear ≈ 1.33).

This is deliberately *not* the reverb's construction. The reverb scales its whole
knob by `1/0.9` (`reverb.cpp:53`), which moves every setting; that would violate
"on top, not instead". The cost of the stricter version is that unity no longer
sits at the top of travel but at roughly 0.926 — a narrow but findable spot where
the loop sustains forever, with the last sliver of travel blooming past it.

Not just a note for the record: `kDefaultFeedback = 0.95f` (`sampler_config.h:14`)
is above the knee and so changes meaning — from −3 dB to about −1.8 dB. It stays
where it is; the boot state gets marginally hotter, still short of unity. But this
is the resting state of **every** overdub, not an opened extreme, so the listening
pass must include a plain boot-state overdub render heard against today's — it is
listed in the acceptance for that reason.

The overdub write gains `fast_tanh`, in the same order of operations that keeps
`EchoDelay::Process` stable (`engine/fx/flux.h:129-141`): **saturate the value
read back from the buffer, then multiply by the feedback coefficient, then sum
the input.** Saturating after the multiply would not bound the write.

**The saturator engages only above unity.** Below the knee the write path stays
exactly as it is today, which is what "on top, not instead" requires — `fast_tanh`
compresses audibly from roughly half scale upward, so running it unconditionally
would give every overdub a tape character it does not have today. The cost is one
branch in the record path and an audible threshold at the knee; both are
accepted.

**Expected behaviour at the top of the knob:** the buffer saturates into itself
and stops being a recording. That is the intent.

## 2 — MOTION scatters across the whole buffer

`kScatterPosFrac` 0.25 → **1.0**. One constant.

Today MOTION at maximum scatters the read position over only a quarter of the
recorded material. The M5 spec records fog as unreached and records that an
attempt to reach it by stretching grain length was tried and reverted on 2026-07-20
because it read as tremolo, not fog. Widening the scatter is the lever that was
never tried; it is the cheapest of the three options that spec left open.

## 3 — SIZE, both ends

**The content clamp goes.** `sampler_engine.cpp:204-216` currently clamps grain
length down to `_buf.rec_size()`. This protects nothing: `SampleBuffer::read_linear`
folds the read position modulo the content length (`sample_buffer.cpp:184-190`),
so a grain longer than its material is simply a loop with a window drawn over it —
a slow swell, which is musical. Delete the clamp.

**The curve.** The middle 60 % of travel stays bit-identical; both outer fifths
steepen. Range becomes **1 ms … 42 s** (from 20 ms … 2 s). The top matches the
record buffer's capacity (42 s @ 48 kHz, `instrument.h:22-26`): at maximum a
grain reads the entire loop exactly once under a single window. Beyond that the
number would only add modulo-fold repeats of material already covered.

| knob | today | new |
|---|---|---|
| 0.00 | 20 ms | 1 ms |
| 0.10 | 32 ms | 7 ms |
| 0.20 | 50 ms | 50 ms |
| 0.50 | 200 ms | 200 ms |
| 0.80 | 796 ms | 796 ms |
| 0.90 | 1.4 s | 5.8 s |
| 1.00 | 2.0 s | 42 s |

Piecewise exponential, continuous in value at both joints, deliberately not
smooth in slope. The kinks are at 0.2 and 0.8.

**The 64-sample floor and what it guards.** The floor at
`sampler_engine.cpp:204-216` is a **CPU** guard, not a safety one: its comment
states it exists to keep `_spawn_every` off its one-sample floor, because that
would put `ratio_for`'s `std::pow` on the per-sample path. Opening SIZE downward
therefore requires removing the reason for the floor, not moving it:

- **Hoist the base ratio.** `ratio_for` (`sampler_engine.cpp:12-14`) depends only
  on the pitch target, which updates once per control block. Compute it in
  `_update_control` and store it; `_next_ratio` then only multiplies.
- **Approximate the detune factor.** DTUN's `2^(cents/1200)`
  (`sampler_engine.cpp:286-288`) is per-grain and depends on a random draw, so it
  cannot be hoisted. Its argument is bounded to ±35 cents by `kDetuneCeilCt`,
  i.e. a factor in `[0.980, 1.020]` — a polynomial approximation over that
  interval is accurate to well below the audible. **The Rng draw order must not
  change**; only the arithmetic applied to the drawn value changes.
- The octave scatter (`:197-199`) and SUB (`:283-284`) are exact ×0.5 / ×2
  multiplies and need nothing.

**The floor moves to the right quantity.** Today's floor sits on grain *length*,
but the cost it guards against is per *spawn*, and `kOverlap` decouples the two:
`_spawn_every = _grain_len / kOverlap`, floored at 1 sample
(`sampler_engine.cpp:218-219`). A length floor therefore does not bound the spawn
rate at all once §6 raises `kOverlap` — at length 32 and `kOverlap = 16` the deck
would spawn every 2 samples, 24 kHz, each spawn scanning up to 32 slots.

So: **delete the grain-length CPU floor entirely** and floor `_spawn_every` at
**8 samples** instead. Grain length then keeps only its safety minimum (`_len < 2`
in `Grain::spawn`, `grain.h:38`), and the two concerns stop being tangled:

- The spawn rate is bounded at 6 kHz per part regardless of `kOverlap`, so §6 can
  raise density without silently reopening this cost.
- SIZE's bottom stays honest. At the new minimum (1 ms ≈ 48 samples) with
  `kOverlap = 4`, the interval is 12 samples — above the floor, so 1 ms is fully
  reached. At a higher `kOverlap` the *density* saturates while the grain length
  still follows the knob, which is the graceful degradation and the right one:
  the ear notices a missing grain far less than a wrong length.

At the bottom of the curve the deck becomes a pitched buzz rather than a texture.
That is intent.

## 4 — Pitch

`ratio_for` gives ±18 semitones today (`8^(n−0.5)`). The knob is bipolar, so
"on top" means the **middle half** of travel (0.25…0.75) stays identical and both
outer quarters steepen, reaching **±4 octaves**.

Consequence to be aware of, not to guard against: in tape mode the grain's output
duration is `_grain_len / ratio` with **no upper clamp** (`sampler_engine.cpp:290-296`)
— this path is already unbounded today. At the new extremes a tape grain can last
minutes. It fits in `int` samples with room to spare and folds safely on read.
The `ratio > 0.001f` divisor floor at `:293` stays; the composed ratio's own floor
sits far above it.

## 5 — Resonance

`SamplerEngine::set_resonance` caps at **0.95** (`sampler_engine.cpp:376-380`)
with no comment giving a reason. Raise it toward and into self-oscillation, and
add a test that renders at the new maximum and asserts the output stays finite and
below a sane ceiling.

If DaisySP's `Svf` diverges, clamp back — and record *why* in the comment that is
missing today. That is this design's own rule applied honestly: the ceiling
returns as a measured fact, not as an inherited habit.

The neighbouring cutoff clamp `[20, 0.3·sr]` (`:243-244`) is a genuine Nyquist
stability guard and **stays**.

## 6 — Density, measured rather than guessed

A correction to a natural misreading: **`kGrains` is not the density control.**
Density is set by `kOverlap = 4` (`sampler_engine.h:30-32`), which divides the
grain length to give the spawn interval (`sampler_engine.cpp:218`). `kGrains = 8`
only supplies slots; a spawn that finds none is silently dropped (`:253`).
Raising density therefore means raising `kOverlap` **and** raising `kGrains`
behind it, or the extra spawns vanish.

`kGrains = 8` has no memory or CPU justification anywhere in `engine/` —
`Grain` is a small fixed struct in a member array (`sampler_engine.h:95`). But
cost scales linearly with concurrent grains, and `engine/fx/taps.h:85` records
that the instrument is already near its block budget.

So this is the one item that gets a measurement instead of a number:

- Render a worst-case scenario — both parts on the sampler, maximum MOTION,
  short SIZE so spawns are frequent, all slots contended — at three
  `(kOverlap, kGrains)` pairs: **(4, 8)** as today's baseline, **(8, 16)**, and
  **(16, 32)**.
- Report wall-clock render time per audio-second for each, plus the measured
  mean and peak concurrent grain count, so it is visible whether the slots are
  actually being used or the spawns are being dropped.
- Choose from the numbers.

**The measurement is a proxy and must be reported as one.** It is desktop render
time, not Daisy cycles. It reliably shows whether a setting costs 4× as much; it
does not show whether it fits on the board. That question belongs to the Rack
play-test and ultimately to hardware. And the proxy is at its blindest exactly at
the top pair: on the Daisy the likely limit for (16, 32) — 64 concurrent grains
across both parts — is not compute but **SDRAM traffic**, interpolated reads
scattered across ~32 MB of record buffer defeating the cache (`taps.cpp:201`
already halves SDRAM traffic for precisely this reason, on a far smaller buffer).
A desktop CPU with megabytes of cache does not feel this cost at all. So even a
clean desktop number for (16, 32) is provisional; (8, 16) is the largest pair the
proxy can meaningfully vouch for.

---

## What must not move

These clamps prevent real failures and stay, growing with the ranges where
relevant:

| Clamp | Location | Prevents |
|---|---|---|
| `read_linear` NaN / magnitude guard | `sample_buffer.cpp:162-173` | `static_cast<size_t>(NaN)` UB |
| `read_linear` index fold | `sample_buffer.cpp:184-190` | out-of-bounds read at the float edge |
| `set_rec_size` clamp to `_buffer_size` | `sample_buffer.cpp:148` | buffer overrun |
| `load_sample` clamp to capacity | `sampler_engine.cpp:102` | buffer overrun |
| `Grain::spawn` `_len<2`, `atk<1`, `dec<1`, atk+dec rescale | `grain.h:36-48` | divide-by-zero, malformed window |
| `Grain::release` `fade_len<1` | `grain.h:74` | divide-by-zero |
| tape-mode `ratio > 0.001f` divisor floor | `sampler_engine.cpp:293` | divide-by-zero |
| SVF cutoff `[20, 0.3·sr]` | `sampler_engine.cpp:243-244` | filter instability near Nyquist |
| `fb_fade` clamp `[0,1]` | `sample_buffer.cpp:113` | overshoot during the record crossfade |
| `kWindowHalfMax = 0.5` | `sampler_config.h:27` | ATK+DEC overlap in the grain window |

## Acceptance

1. Synth neutrality: pinned synth scenarios render byte-identical. **Gate.**
2. Determinism: a double render is byte-identical. **Gate.**
3. Feedback above the knee stays finite — no NaN, no unbounded growth — over a
   long render at maximum.
4. Feedback below the knee is unchanged from today, sample for sample.
5. SIZE over 0.2…0.8 of travel is unchanged from today, sample for sample.
6. Resonance at the new maximum stays finite.
7. No `std::pow` remains on the grain-spawn path.
7b. The spawn interval never falls below 8 samples, at any `kOverlap` and any
   SIZE. This is the CPU guard §3 relocates; a test must pin it, because the
   density work in §6 is exactly what would otherwise reopen it unnoticed.
8. The density measurement is reported with its three numbers and its proxy
   caveat stated.
9. A listening render exists for each opened extreme, **plus** a boot-state
   overdub render (untouched knobs, `kDefaultFeedback`) paired against the same
   render from today's build — the default got hotter and must be judged, not
   assumed.

The final judgement is by ear, and it is Bastian's. The M5 spec records why:
on the reverted MOTION stretch, the RMS continuity metric cleared a change that
the ear correctly rejected. **Numbers gate the failures; they do not gate the
sound.**

## Not in scope

- Delay and reverb. They already do this; a review of whether they go far enough
  is a separate piece of work.
- Any panel change. Nothing here adds or moves a control, so the hardware
  reducibility constraint is untouched.
- `kFiltNeutral`, `kNormSmoothS`, `kBurstReleaseS` — ear-tuned from measurement
  during M5a, no evidence they are wrong.

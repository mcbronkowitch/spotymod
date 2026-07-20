# Spotykach Ambient Reverb v2 — Oliverb Port Design

Replaces the M1.6 shared reverb core (DaisySP `ReverbSc` + shimmer) with a
port of **Oliverb** from the Clouds Parasite firmware — an Erbe-Verb-inspired,
MIT-licensed reverb with continuous size (Doppler warp), decay beyond 100 %
with in-loop soft clipping, and modulated delay lines. Supersedes the reverb
core sections of `2026-07-11-spotykach-fx-design.md`; signal-flow position,
sends, and the FX modulation row are untouched.

## Goal

The M1.6 reverb is a fixed room: `ReverbSc` with feedback capped at 0.99 and
a shimmer path that is scaled *down* as the room grows (a stability guard),
so at exactly the settings where an ambient player wants drama, it tames
itself. v2 turns the shared room into a playable instrument: ultra-long
tails, decay that crosses 100 % into a self-sustaining bloom, a size control
that audibly pitch-warps the tail while turned, and lush modulated lines —
within the same CPU budget (cheaper, in fact: the shimmer pitch shifter goes
away).

## Decisions (from brainstorming, 2026-07-12)

- **Character: Erbe-Verb / instrument** — the reverb is something to *play*,
  not a static room behind the mix.
- **Above 100 % decay: soft bloom** — saturation inside the loop catches the
  energy; the tail swells, densifies and compresses instead of exploding or
  hard-clipping. (Oliverb ships exactly this: `SoftLimit` in both loop
  branches.)
- **Parameter set "Kompakt+", minus freeze:** SIZE (true, Doppler), DECAY
  (> 100 % possible), TONE (loop damping), DEPTH (line modulation). FREEZE
  was considered and **dropped from scope** — decay ≥ 1 already sustains
  indefinitely; a dedicated freeze can be added later as a caller-level
  trick (mute input + pin decay) without touching the core.
- **Shimmer is removed entirely** — `set_shimmer` disappears from the API.
  Oliverb contains an optional in-loop pitch shifter; we do not compile it
  in. (It remains a cheap future option, deliberately unused.)
- **Build vs. reuse: reuse.** A research pass (2026-07-12) surveyed
  open-source reverbs; Oliverb (Clouds Parasite fork, MIT) already implements
  essentially the whole spec with a decade of hardware-proven tuning.
  Porting it replaces the by-ear-from-scratch FDN tuning that a self-built
  core would have required.
- **Fallback if Oliverb disappoints by ear:** Signalsmith Basics reverb
  (MIT, documented 8-channel FDN) as an alternative base — same facade,
  swappable core. Second fallback: fverb (BSD-2, Dattorro plate).

## Research summary (what was considered)

| Candidate | License | Verdict |
|---|---|---|
| **Oliverb** (mqtthiqs/parasites, `clouds/dsp/fx/oliverb.h`) | MIT (verified in header) | **ADOPT** — continuous smoothed SIZE with interpolated reads, unclamped decay + in-loop `SoftLimit`, 9 LFOs on the lines, LP+HP loop damping, pre-delay |
| Signalsmith Basics reverb | MIT | fallback base (would need >100 %, saturation, Doppler size added) |
| fverb (jpcima) | BSD-2 | plate-flavored fallback |
| Clouds stock reverb | MIT | too few axes (no size/saturation) |
| CloudSeed | MIT | blows the CPU on this exact chip (5 mono lines ≈ 100 %) — skip |
| Airwindows Infinity/Galactic | MIT | doubles throughout; reference for saturated-tank character only |
| Valley Plateau, zita-rev1, Freeverb3 | GPL | reference-only, cannot be linked |
| MI Beads reverb | closed source | unavailable |

## Signal flow (unchanged)

```
Part A/B post-FX × REVERB SEND (equal-power, morph-scaled per M4)
        → AmbientReverb (shared, wet-only) → + master after part mix
```

Same injection point, same send semantics, same wet-only join. Bypass
(send 0) remains bit-exact.

## Architecture

### Vendored core — `third_party/oliverb/`

`oliverb.h` + `fx_engine.h` from
[mqtthiqs/parasites](https://github.com/mqtthiqs/parasites)
(`clouds/dsp/fx/`), MIT headers retained, documented in `THIRD_PARTY.md` as
**vendored with modifications**:

1. **Buffer format: float32** instead of the Clouds 16-bit fixed format
   (`fx_engine.h` is templated on the data format; we instantiate/trim to
   the float path). Buffer memory is injected, never heap-allocated —
   `FxMem` pattern as today: static on desktop, SDRAM in the M6 shell.
2. **48 kHz constants** — all delay-line lengths scale ×1.5 from the native
   32 kHz (no resampler; zero CPU, tuning shift verified by ear).
3. **Pitch-shifter branch compiled out** (shimmer removal).
4. **stmlib shims** — the header pulls small stmlib utilities (`SoftLimit`,
   cosine oscillator, random); ported or shimmed into `engine/util/` (MIT),
   reusing existing helpers (`fast_sin`) where they fit.

Exact buffer size is determined during the port; estimate ≈ 0.5–1 MB float
(fine in 64 MB SDRAM / a desktop static). The old ~530 KB ReverbSc object
disappears.

### Facade — `engine/fx/reverb.h/.cpp`

`AmbientReverb` keeps its name, location, `init(sample_rate)` /
`process(in_l, in_r, out_l, out_r)` shape, and the normalized-setter style:

| Setter | Mapping (0..1 →) | Notes |
|---|---|---|
| `set_size(n)` | Oliverb size, exponential feel | internally smoothed → audible Doppler warp while turning |
| `set_decay(n)` | loop gain; **top ~10 % of travel crosses 1.0** (cap ≈ 1.05, tuned by ear) | above 1.0 the in-loop soft clip turns runaway into bloom |
| `set_tone(n)` | loop LP damping 500 Hz – 16 kHz, exp | same semantics as today |
| `set_depth(n)` | LFO modulation amount on the lines | LFO rates fixed internal spread (Oliverb defaults) |

- `set_shimmer` is **deleted**, not deprecated.
- Oliverb's HP loop damping is pinned internally (~80 Hz low-cut) so the
  bloom doesn't accumulate mud — no user parameter.
- Pre-delay pinned to 0 for now (the sends already sit post-FLUX).
- All setters clamp, coefficient updates ride the 96-sample control-rate
  raster like the rest of the engine.

### Modulation row (unchanged)

Global reverb character stays **static layer parameters** — only the
per-part REVERB SEND is a modulation target, exactly as decided in M1.6.
Scenario timelines can still automate decay/size/etc. via actions.

## Migration

| Touchpoint | Change |
|---|---|
| `engine/instrument.h` | `set_reverb_shimmer` **removed**; `set_reverb_decay(float)`, `set_reverb_depth(float)` added (same null-guarded forwarding style) |
| `host/render/scenario.cpp` | action `set_reverb_shimmer` removed; `set_reverb_decay`, `set_reverb_depth` added |
| Scenario JSONs (`ambient_wash`, `entropy_duet`, `capture_duet`, `capture_pentatonic` carry shimmer entries; others set size/tone only) | `set_reverb_shimmer` entries replaced with sensible decay/depth values; **`ambient_wash.json` rebuilt as the showcase**: a decay ride across 100 % and back, plus a size Doppler ride |
| `tests/test_reverb.cpp` | rewritten for the new core (see Testing) |
| `tests/test_instrument.cpp`, `tests/test_scenario.cpp` | shimmer references migrated to decay/depth; assertions kept equivalent or stronger |
| `tests/test_fx_deps.cpp` | ReverbSc static-allocation checks replaced with the Oliverb-buffer equivalent |
| CMake + `THIRD_PARTY.md` | **DaisySP-LGPL dependency removed entirely** (ReverbSc + allpass were its only users); LGPL note replaced by the Oliverb MIT entry — the whole repo becomes MIT-clean |
| `docs/roadmap.md` | new milestone entry (M4.5, see below); M1.6 reverb wording marked superseded |
| M1.6 FX spec | superseded-note added to its reverb/shimmer sections (core choice + the M6 `ALT + FLUX` knob map) |

### UX note (M6, deferred)

The reverb layer gesture (`ALT + FLUX` hold) now has four axes; suggested
knob map — RATE → SIZE, SHAPE → TONE, DEPTH → DEPTH, SMOOTH → DECAY.
Tuned on hardware in M6; the engine API is agnostic.

## Boot defaults

Instantly audible, nothing screams, nothing self-oscillates at boot:

- SIZE 0.6, DECAY 0.55 (well below 1.0), TONE 0.5, DEPTH 0.25
- REVERB SEND default stays 0.25 (M1.6)

## Testing

doctest, desktop, deterministic — as established:

1. **Decay < 1:** impulse-response energy falls monotonically across
   successive windows.
2. **Decay > 1 (bloom):** long run stays **bounded** — no NaN/Inf, peak
   under a fixed ceiling; energy first grows, then saturates.
3. **Size sweep while ringing:** continuous output, no NaN, sample-to-sample
   delta bounded (Doppler yes, clicks no).
4. **Tone:** closing it measurably reduces high-frequency tail energy.
5. **Determinism:** two instances, identical input → bit-identical output.
6. **Silence in → silence out** (denormal safety with the float buffer).
7. Existing instrument-level invariants (bypass bit-exact, null-reverb
   pointer safety) adapted, not weakened.

Sound character (does the bloom swell musically? does the size warp sing?)
is verified by ear via host renders of the two demo scenarios — the
remaining by-ear scope is the 48 kHz scaling check plus the four mapping
curves.

## CPU & memory budget

- Oliverb ran on a 168 MHz STM32F4 at 32 kHz *alongside* Clouds' granular
  engine. On the 480 MHz M7 at 48 kHz (×1.5 samples), estimate **3–6 %** —
  comfortably under the ~10 % reverb slot, and net cheaper than
  ReverbSc (+ shimmer's pitch shifter) it replaces.
- Memory: ≈ 0.5–1 MB float buffer (injected; SDRAM on Daisy, static on
  desktop) vs. ~530 KB today — irrelevant against 64 MB SDRAM.
- Desktop render-time-per-block remains the early indicator; the `METER`
  arrives with the M6 shell.

## Roadmap placement

New milestone **M4.5 — Ambient Reverb v2** (after M4 center section, before
M5). Rationale: it swaps a self-contained FX core behind a stable facade —
no interaction with the M5 scope — and every later milestone then renders
demos through the reverb the instrument will actually ship with.

## Assumptions to verify during implementation

- `fx_engine.h`'s float path drops in cleanly with the clang/ninja desktop
  build (no ARM intrinsics in the code paths we keep).
- The ×1.5 constant scaling keeps Oliverb's tuning character (verify by ear
  against a 32 kHz-native render if in doubt).
- The stmlib utility shims are small (SoftLimit, cosine LFO, random) — no
  hidden dependency web.
- Decay cap ≈ 1.05 and the >1.0 knee position (~top 10 % of travel) feel
  right under the send levels our scenarios actually produce — tune by ear.
- Removing DaisySP-LGPL breaks no other module (grep says ReverbSc/allpass
  were its only users; re-verify at CMake level).

## Acceptance criteria

- Repo builds and all tests pass with **no DaisySP-LGPL reference** left in
  CMake, code, or `THIRD_PARTY.md` (Oliverb MIT entry added instead).
- `set_shimmer` / `set_reverb_shimmer` / the scenario action no longer
  exist anywhere (grep-clean).
- `ambient_wash.json` render: the decay ride audibly crosses into
  self-sustaining bloom and recovers; the size ride audibly pitch-warps the
  tail; output stays bounded throughout.
- New `test_reverb.cpp` suite passes; existing suites stay green;
  determinism invariant (identical scenario → bit-identical WAV) holds.
- Desktop render time per block does not exceed the M4 baseline (shimmer
  removal should offset the new core).

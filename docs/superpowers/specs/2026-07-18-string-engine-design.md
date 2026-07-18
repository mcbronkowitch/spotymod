# STRING — experimental Karplus-Strong part engine

**Date:** 2026-07-18
**Status:** design approved, implementation gated on the hardware bench (see §7)
**Scope:** a fourth selectable part engine (`ENGINE_STRING`) — a 4-voice
double-string Karplus-Strong instrument behind the existing
`IPartEngine`/`SynthEngine` semantics, with a playable exciter, tape
excitation from the part's own FLUX echo, and string abuse (nonlinearity,
inharmonic doubling) as first-class territory. No new panel control, no new
parameter id. No change to the mod plane, FX, reverb, or the master chain.

## Problem

SYNTH covers the analog corner, WAVE (spec of the same day) the digital-glassy
one. What neither can do is *physics*: attacks that ring out on their own,
material that reacts to how hard it is struck, decay that is a property of the
string rather than an envelope setting. The engine-expansion research
(residency repo, 2026-07-18) identified DaisySP PhysicalModeling as the
cheapest first step — but a plain port of `daisysp::StringVoice` would keep
the exciter sealed (fixed Dust noise), fight the string's natural decay with a
VCA envelope, and leave no seam for the experimental directions this fork
cares about. The brief for this engine: **string physics as the cheap core,
experiments at the edges** — a playable strike, the FLUX tape as an exciter,
and the string pushed past its polite Rings sweet spot.

CPU is the hard constraint: every published Daisy CPU figure proved unreliable
(bench-firmware spec, 2026-07-18), so this engine must be *structurally*
cheaper than SYNTH, and implementation waits for the on-hardware bench.

## Decisions (user)

- Core: **Karplus-Strong strings** (`daisysp::String` primitive, MIT) —
  pluck/bow/harp territory. Modal/bell territory (`Resonator`, 24 modes with
  per-sample coefficient math) is out: too expensive, wrong corner.
- Experimental directions **in**: string abuse (nonlinearity, inharmonic
  doubling), a playable exciter replacing Dust, tape excitation (FLUX → string).
  **Out**: sympathetic cross-part coupling (deliberately deferred).
- Surface: **strict reinterpretation** — zero new params, zero panel changes.
  Existing VOICE-layer knobs and lanes take string-native meanings (§3).
- Voice interior: **4 voices × double string** — every voice carries two
  `daisysp::String` instances spread by DETUNE, from piano shimmer to broken
  inharmonic metal.

## Existing infrastructure this reuses

- `engine/synth/synth_engine.*` — the entire allocation machine: round-robin +
  oldest-steal, FLOW sustain/demote, drone promise, chord slots + stab
  humanization, CHOKE hold, velocity slew, control-rate cadence
  (`kCtrlInterval = 96`), FILT silence-fade invariant (`_filt_gain`).
- `lib/DaisySP/Source/PhysicalModeling/KarplusString.{h,cpp}` — the string
  primitive: 1024-sample delay line + stretch line, one-pole damping filter,
  DC blocker, curved-bridge/dispersion nonlinearity. MIT (Electrosmith +
  Émilie Gillet), joins the `daisysp_min` target. The `StringVoice` wrapper is
  **not** used.
- `engine/mod/rng.h` (deterministic exciter), `engine/util/fast_sin.h` (ping
  exciter), `engine/fx/flux.h` (tape tap), `engine/util/onepole.h`.
- `host/render/scenario.cpp` engine parsing; VCV per-part engine button
  (`Spotymod.cpp` ENGINE_A/B); bench-firmware workload list.

## Design

### 1. Integration — engine templated on the voice type

The WAVE spec planned `SynthEngineT<OscT>`. A string voice is not an osc swap
(no Svf, no Env), so the template moves one level up: **the engine is
parameterized on the voice type.**

- `SynthEngineT<V>` owns allocation, FLOW, chords, CHOKE, control cadence —
  inherited by every engine, never copied.
- `using SynthEngine = SynthEngineT<Voice>` — SYNTH reference renders stay
  **byte-identical** after the refactor (regression gate).
- WAVE becomes `SynthEngineT<VoiceT<WtOsc>>` — the WAVE spec's osc-level
  template remains valid one layer down (amendment noted in that spec).
- STRING is `SynthEngineT<StringVoice>`. `EngineId` grows
  `ENGINE_STRING = 3`; scenario parser learns `"string"`; the VCV engine
  button cycles four states (LED shade; panel unchanged —
  hardware-reducibility constraint). No other surface.

**Voice contract.** `StringVoice` implements exactly the methods
`SynthEngine` calls on `Voice` today — `trigger`, `set_sustaining`,
`set_pitch_hz`, `set_vel`, `set_env_times`, `set_morph`,
`set_detune_cents`, `set_sub_level`, `set_cutoff_hz`, `set_resonance`,
`set_pan`, `set_drift_amount`, `update_control`, `process`, `active`,
`env_value` — with string-native semantics (§3). Compile-time substitution,
no virtual dispatch. The contract grows one method, `set_hold(bool)`
(palm mute, §3); the SYNTH `Voice` implements it as a no-op.

**`active()` without an envelope.** The string rings out on its own; nothing
reports "done". Replacement: a control-rate energy follower (block peak,
decaying). `active()` = follower above ~−72 dB **or** recently triggered (a
minimum hold so a quiet strike is not stolen instantly). `env_value()`
returns the follower — LED/introspection semantics stay meaningful.

New files: `engine/synth/string_voice.h/.cpp`, `engine/synth/exciter.h`.

### 2. Voice interior

```
Exciter (playable) ──┬──→ String A ─┐
  + tape excitation ─┘    String B ─┼→ sum → pan (MOTION fan + drift) → vel
       (SUB)           (±spread/2) ─┘
```

No Svf, no Env — the string's decay *is* the envelope; the FILT
silence-fade invariant keeps running through the engine-side `_filt_gain`.

**The exciter** (`exciter.h`, own deterministic `Rng` per voice, fixed
distinct seeds). TIMBRE morphs the strike character across four zones:

| zone | character |
|---|---|
| 0 | **click** — filtered impulse, bare Karplus pluck |
| 1 | **noise burst** — Dust-like filtered noise |
| 2 | **granular sputter** — rng-gated micro-bursts |
| 3 | **tonal ping** — short `fast_sin` blip at string frequency (pitched hammer) |

Zone boundaries and crossfades are tuning material (DUST-zone precedent). In
STEP the exciter fires as a strike of ATTACK-controlled length; in FLOW the
same character becomes *continuous* excitation — the bowed drone, honoring
the drone promise. Excitation level follows velocity (chord gain comp).

### 3. Control mapping — strict reinterpretation

| control | SYNTH meaning | STRING meaning |
|---|---|---|
| TIMBRE lane | morph + t²·DETUNE_MAX | exciter character + t²·spread (same formula) |
| FILTER lane + FILT | Svf cutoff 60 Hz–14 kHz | brightness 0..1 (log map of the same Hz value) |
| ATTACK | env attack (% of cycle) | exciter length: 2 ms click ↔ bow swell (tempo-coupled) |
| DECAY | env decay (× cycle) | damping — ring time follows the tempo |
| RESO | Svf resonance | nonlinearity, bipolar: 0 = curved bridge max ↔ 0.5 neutral ↔ 1 = dispersion max |
| DETUNE | ±35 ct osc spread | string spread × ~4 (up to ~140 ct — double string into broken inharmonic) |
| SUB | sub-sine level | **tape excitation**: FLUX tape playback into the strings |
| CHOKE | drone release + retrigger pause | the same + **palm mute**: damping snaps high, strings physically choke |
| PITCH / MOTION / LEVEL | (unchanged) | (unchanged: latch/track, pan fan + drift, master gain) |

Exact response curves (damping-vs-decay map, nonlinearity taper, spread
scale) are tuning material for the listening pass — the table rows are the
contract, the curves are not.

### 4. Tape excitation (SUB)

The tap is the **part's own FLUX tape read** (the echo playback signal, not
the mixed output): the string hangs in the room and rings along with the
echo. Path: mono sum → DC block → soft clip → SUB² gain (max ≈ 0.5) → added
to every active voice's excitation input, delayed by one control block
(96 samples ≈ 2 ms, inaudible — breaks feedback simultaneity). `PartFx`
hands the engine the previous block's tape tap; the engine consumes it
per-sample through the voice contract.

The loop string → GRIT → FLUX tape → string is *intended* (self-oscillation
territory at high SUB + echo feedback) but bounded: soft clip + gain < 1 +
the string's own damping filter. **SUB = 0 ⇒ the path is hard-gated,
bit-exact off.**

### 5. CPU budget & the hardware gate

Per string per sample: 1 interpolated delay read + one-pole + DC block +
crossfade + nonlinearity branch. 16 strings total replace, at SYNTH: 16
polyblep oscillators + 8 `fast_sin` subs + 8 Svf + 8 exponential envelopes —
**structurally cheaper**; the exciter is an rng draw + one filter, near
free. All `Set*` calls stay on the 96-sample control cadence.

That stays an *estimate* until measured:

- The bench firmware's workload list grows one entry: **"StringEngine 2×4
  voices (both parts)"** — same harness, same anchor calibration.
- **Implementation of this spec starts only after the hardware bench has
  run** and confirms the workload fits alongside the full instrument.

### 6. Memory

`String` holds 1024 + 256 floats ≈ 5 KB → 2 strings × 4 voices × 2 parts ≈
**≈ 80 KB static SRAM** — deliberately *not* SDRAM: the reads are random
access, exactly the cache trap the research flagged. The delay-line floor
(~47 Hz at 48 kHz) sits safely below the pitch contract (min 110 Hz); the
primitive's internal upsampler path stays unused. Desktop/VCV compile the
same code — identical bits on every platform.

### 7. Hosts & demo scenarios

- Scenario parser: `"string"`. VCV: engine button cycles
  test-tone → synth → wave → string.
- `string_strum.json` — STEP + chord layer = plucked harp, DETUNE ride into
  inharmonic territory (listening + regression anchor).
- `string_bow.json` — FLOW drone, bowed excitation, SUB sweep into tape
  self-oscillation, choked by CHOKE.

### 8. Testing

- **Templatization gate:** SYNTH reference renders byte-identical after the
  `SynthEngineT<V>` refactor.
- **Parity semantics:** the existing engine tests (FLOW drone, steal order,
  chords, CHOKE) run as a second instantiation against
  `SynthEngineT<StringVoice>`.
- **Determinism:** same seed → bit-identical render on desktop and VCV.
- **Stability torture:** max nonlinearity + max SUB + max brightness + min
  damping over a long render → bounded output, no NaN/Inf (the soft-clip
  guard holds).
- **SUB-0 gate:** byte-identical to the tape path compiled out.
- **Follower:** a voice frees after ringing out; a quiet strike is not
  stolen instantly; a palm-mute render shows the fast energy drop.
- **Tuning:** string pitch within a few cents across the register
  (fractional delay interpolation).

## Out of scope

- Sympathetic cross-part coupling (deliberately deferred — candidate for a
  later extension, noted here so the idea is not lost).
- Modal/bell territory (`Resonator`), Rings' polyphonic string-synth part,
  bowed-exciter modeling beyond continuous excitation.
- New panel controls or parameter ids; exciter samples; per-string outputs
  beyond the existing pan.
- Any change to GRIT/FLUX/reverb/master; the tape tap is read-only.

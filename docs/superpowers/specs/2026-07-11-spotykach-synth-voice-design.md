# Spotykach Synth Voice — M2 Design

The polyphonic synth engine for the modulation-first firmware: a 4-voice,
trigger-driven ambient voice that replaces `TestToneEngine` as the default
`IPartEngine`. Extends the main design
(`2026-07-10-spotykach-modulation-first-synth-design.md`, section "Engine 2 —
Synth voice"); milestone **M2**, after M1.6 (FX), before M3 (capture
sequencer).

Fork: `github.com/mcbronkowitch/spotymod`, local at
`c:\Users\bernd\Documents\AI\Spotykach`.

## Goal

Deliver the "overlapping voices" half of the instrument: every PITCH-lane
probability event starts a voice whose pitch is latched at trigger time
(quantized upstream), long decays overlap into pads, and FLOW mode turns the
part into a continuously modulated drone. The engine slots into the final
M1.6 signal chain (engine → GRIT → FLUX → reverb send) unchanged and is fully
testable on the desktop.

## Decisions (from brainstorming, 2026-07-11)

- **Internal voice parameters: tempo-coupled defaults + edit layer.**
  Envelope times are ratios of the master modulation cycle (slow modulation =
  overlapping pads, fast = plucks); a VOICE edit layer (PLAY-pad hold) sets
  the ratios and the other voice parameters via soft takeover.
- **Drone rule: sustaining last voice.** In FLOW, the most recently triggered
  voice holds at a sustain level and its pitch continuously follows the
  quantized PITCH target; new fires spawn overlapping voices on top. In STEP,
  all voices are plain attack/decay — notes end.
- **MOTION = pan spread + drift.** Stereo width with a slow per-voice
  pan/micro-detune drift; no delay-based chorus in the engine (that is
  FLUX's territory, per the M1.6 boundary).
- **TIMBRE = dark→rich macro.** One sweep travels pure sine → triangle →
  saw/pulse while the oscillator detune opens along the way; sub level stays
  an edit parameter.
- **Oscillator core: custom `MorphOsc`** (one phasor, continuous morph with
  polyblep anti-aliasing) + DaisySP `Svf` lowpass. Deterministic,
  engine-owned, unit-testable like `waveforms.h`; DaisySP only for the
  filter.
- **Beyond the master spec: PLAY tap = manual trigger** on a synth part (the
  pad is otherwise unassigned there) — the part becomes strummable.
- **Boot-default engine is the synth**; `TestToneEngine` stays selectable
  (tests, A/B reference).

## Prerequisite

M1.6 (FX) finishes first: it is mid-implementation in the fork and brings the
DaisySP desktop build (`daisysp_min`) that the voice filter needs, plus
`SoftSwitch` (reused for click-free engine switching) — and M2 then builds on
the final signal chain.

## Voice architecture (×4 per part)

```
MorphOsc A ─┐
MorphOsc B ─┼→ mix → Svf lowpass (FILTER) → AD/ADS envelope (VCA) → equal-power pan (MOTION)
sub sine ───┘
```

- **MorphOsc** (new, `engine/synth/`): single phasor; output is a continuous
  blend sine → triangle → saw → pulse, with polyblep corrections on the
  saw/pulse discontinuities. Same morph-bank idea as `mod/waveforms.h`, at
  audio rate and band-limited.
- **Sub:** plain sine one octave below the note, level = SUB edit parameter.
- **Filter:** one DaisySP `Svf` per voice, lowpass output. No keytracking in
  v1.
- **Envelope:** attack/decay, exponential segments; in FLOW the last-triggered
  voice decays to a fixed sustain (0.7) and holds (ADS); everywhere else
  sustain = 0 (AD).
- **Pan:** per-voice equal-power position (see MOTION below).

### Pitch contract

Same as `TestToneEngine`: normalized 0..1 = 36 semitones, 110–880 Hz
(`freq = 110 · 8^p`). The PITCH target arrives already quantized-and-tuned
from `Part` (tune summed before quantization), so it maps straight to
frequency. Pitch is **latched per voice at `trigger(pitch_norm)`**; only the
FLOW sustaining voice tracks the target continuously afterwards.

## Targets (fixed slots, per master spec)

| Pad | Target | Acts on |
|-----|--------|---------|
| 1 | TIMBRE | dark→rich macro: morph position sine→tri→saw/pulse **and** detune spread. Detune cents = t² × DETUNE_MAX (default 18 ct), split ± across osc A/B — 0 is exact unison. |
| 2 | FILTER | cutoff, exponential ~60 Hz … 14 kHz. Resonance is an edit parameter (default 0.15). |
| 3 | PITCH | note pitch, quantized upstream; latched at trigger. |
| 4 | MOTION | stereo width + drift (below). |
| 5 | LEVEL | engine master gain, smoothed. |

TIMBRE, FILTER, MOTION, LEVEL act on **all voices continuously** (they are
part character, not per-note snapshots); only PITCH is latched.

### MOTION: pan spread + drift

- Width `w` fans the four voices to base pans `[-1, +1, -0.5, +0.5] × w`
  (w = 0 → dead mono center).
- Each voice adds a slow drift LFO (~0.05–0.2 Hz, per-voice rate) on pan and
  micro-detune (±3 ct max), amount ∝ w. Drift uses per-voice deterministic
  `Rng` seeds — renders stay bit-reproducible.
- Equal-power pan law.

## Triggering, allocation, drone

- **Trigger sources:** the PITCH lane's probability events (as today via
  `Part`), the gate in (M6), and **PLAY tap** (manual trigger at the current
  quantized pitch; UX wired in M6, engine sees an ordinary `trigger()`).
- **Allocation:** round-robin over free voices; if none free, **steal the
  oldest** (by trigger order). Stealing retriggers the envelope **from its
  current output level** — click-free without a fade-out.
- **STEP mode:** pure AD; silence between fires is legitimate.
- **FLOW mode (drone):** the most recently triggered voice becomes the
  **sustaining voice**: it decays to sustain 0.7 and holds; its pitch
  continuously follows the quantized PITCH target (glides come from SMOOTH
  and the quantizer's 40 ms slew). A new fire demotes it (it is released:
  decays to zero at the decay rate) and the new voice takes over sustain.
- **Drone promise:** whenever the part **enters FLOW with no sustaining
  voice** (engine start, engine switch, or a STEP→FLOW switch mid-run), one
  voice **auto-triggers** at the current PITCH target — the part hums
  immediately, even at probability 0.

## Tempo-coupled envelopes + VOICE edit layer

Envelope times are **ratios of the master modulation cycle** (PITCH-lane
cycle length, forwarded by `Part`):

- ATTACK default 2 % of cycle, floor 2 ms.
- DECAY default 1.5 × cycle, clamped 50 ms … 20 s (release in FLOW uses the
  same time).

The edit knobs set the **ratios**, not absolute times, so the coupling
survives editing: slow the RATE and the same patch stretches into pads.

**VOICE edit layer** (gesture grammar as FX layers; hardware wiring in M6,
engine API now): **hold the part's PLAY pad** while the synth engine is
active = edit layer via soft takeover, ring hold-to-inspect on entry:

| Knob | Voice parameter |
|------|-----------------|
| RATE | ATTACK ratio |
| DEPTH | DECAY ratio |
| SHAPE | RESONANCE |
| SMOOTH | SUB level |
| PROBABILITY | DETUNE_MAX |

**PLAY tap** (short release, no knob moved — standard tap rules) = manual
trigger. ALT + PLAY tap/hold keep their master-spec meanings (record-arm is
sampler-only; ≥ 1 s hold = engine switch).

## Engine architecture

New directory `engine/synth/`:

| Module | Contents |
|--------|----------|
| `morph_osc.h` | polyblep morphing oscillator (phasor, morph, detune in cents) |
| `env.h` | AD/ADS envelope, exponential segments, retrigger-from-level |
| `voice.h/.cpp` | one voice: 2× MorphOsc + sub + Svf + envelope + pan/drift |
| `synth_engine.h/.cpp` | `IPartEngine` impl: allocation, drone logic, target mapping |

**Interface additions** on `IPartEngine` (default no-op implementations, so
TestTone and the M5 sampler ignore them):

```cpp
virtual void set_cycle(float seconds) {}   // master-lane cycle length
virtual void set_flow(bool flow) {}        // STEP/FLOW state
```

`Part` forwards both (cycle on change, not per sample) and gains
`set_engine(EngineId)`; engine switching crossfades through the M1.6
`SoftSwitch` so it is click-free. `Part` keeps calling
`trigger(targets[LANE_PITCH])` on PITCH-lane fires — unchanged.

**Constraints as established:** no heap, no allocation in the audio path;
all voice state in fixed arrays; all randomness via `spky::Rng`; knob→engine
paths through `OnePole`; no libDaisy includes (DaisySP only, already a
desktop build dependency since M1.6).

## Instrument API

```cpp
enum EngineId { ENGINE_TEST_TONE = 0, ENGINE_SYNTH = 1 };

void set_engine(int p, EngineId e);          // boot default: ENGINE_SYNTH
void set_voice_attack(int p, float n);       // ratio, exponential map
void set_voice_decay(int p, float n);        // ratio, exponential map
void set_voice_resonance(int p, float n);
void set_voice_sub(int p, float n);
void set_voice_detune(int p, float n);       // DETUNE_MAX, 0..~35 ct
void trigger_manual(int p);                  // PLAY tap path
int  active_voices(int p) const;             // for CSV / LEDs
```

## Boot defaults

Instantly audible, nothing screams: engine = synth, attack 2 % / decay 1.5 ×
cycle, resonance 0.15, sub 0.3, DETUNE_MAX 18 ct, sustain 0.7 (fixed).
Persistence via the M6 settings storage; the desktop renderer sets them from
scenario JSON.

## Desktop render host

- New scenario actions, 1:1 with the API: `set_engine`, `set_voice_attack` /
  `decay` / `resonance` / `sub` / `detune`, `trigger_manual`.
- `mods.csv` gains per part: `voices` (active count 0–4) and `v0..v3`
  (per-voice envelope levels) — the overlapping-voices plot for the
  portfolio.
- Demo scenarios:
  - `overlapping_voices.json` — STEP Dorian melody, probability ~0.6, long
    decay: the master spec's acceptance demo ("evolving, quantized melodic
    texture with overlapping voices").
  - `flow_drone.json` — FLOW drone with breathing TIMBRE/FILTER, then
    probability opens and fires swell voices over the drone.

## Testing

doctest, desktop, as established:

- **MorphOsc:** frequency accuracy (zero-crossing count over N seconds),
  morph anchor shapes at 0 / ⅓ / ⅔ / 1, output bounds across the full morph
  sweep, detune produces the expected beat frequency.
- **Allocation:** 4 triggers fill 4 voices; 5th steals the oldest;
  round-robin order deterministic; steal retriggers from current level (no
  output discontinuity above a click threshold).
- **Envelopes:** decay length tracks `set_cycle` (ratio honored); attack
  floor at 2 ms; STEP notes decay to silence.
- **Drone:** FLOW auto-voice on engine start; sustaining voice holds at 0.7;
  its frequency follows a PITCH-target change (within slew); a new fire
  demotes it and it decays to zero.
- **MOTION:** width 0 → L == R; width 1 → voices separate; equal-power sum
  approximately constant across pan; drift deterministic per seed.
- **Pitch contract:** `trigger(p)` → 110 · 8^p Hz; a STEP voice's pitch does
  not move when the target changes after the trigger.
- **Engine switch:** test tone ↔ synth without clicks (SoftSwitch).
- **Determinism:** identical scenario → bit-identical WAV (existing
  invariant).

Sound character (do the pads bloom? does the drone breathe?) is verified by
ear via renders.

## CPU budget

Daisy Seed @ 480 MHz, 48 kHz → 10,000 cycles/sample; master-spec target
< 70 %. Worst case is both parts synth (8 voices) + full M1.6 FX:

| Stage | estimate |
|---|---|
| 8 voices (2× MorphOsc + sub + Svf + env + pan, fast sine) | ~15–18 % |
| 10 mod lanes + smoothers | ~4–6 % |
| 2× GRIT + FLUX | ~8–10 % |
| ReverbSc | ~10 % |
| Shimmer (when on) | ~10 % |
| Quantizer, morph mix, misc | ~3–5 % |
| **Worst case total** | **~50–60 %** |

Two constraints are load-bearing for these numbers (binding for M2):

- **No libm `sinf` in the voice audio path.** MorphOsc and the sub use a
  polynomial fast-sine approximation (~10–15 cycles vs. ~80–120 for
  `std::sin` on the M7). With naive `sinf` the 8-voice share roughly triples
  and the worst case bursts the budget. (`shape_value`'s `std::sin` in the
  mod lanes is the same issue at lower stakes — flagged for M6, not an M2
  blocker.)
- **Control-rate updates:** drift LFOs and envelope coefficient
  recomputation run once per 96-sample block, not per sample.

Headroom levers: shimmer off saves ~10 %; a sampler part B is cheaper than
four more voices. Desktop render time per block remains the early indicator;
the `METER` goes live in M6.

## Assumptions to verify during implementation

- Polyblep at the pulse end of the morph is clean enough for high notes at
  48 kHz (else: add polyblep also to the triangle→saw transition midpoint).
- Retrigger-from-level is click-free in practice for hard steals of loud
  voices (else: 2 ms micro-ramp before restart).
- The fixed sustain 0.7 sits right against decaying voices in FLOW (listen;
  it is a constant, not a parameter, until proven otherwise).
- Existing tests/scenarios that implicitly assumed the test tone: update
  them to select `ENGINE_TEST_TONE` explicitly rather than weakening the
  boot-default decision.

## Acceptance criteria

- `engine/synth/` compiles on desktop with no libDaisy include; DaisySP
  (`Svf`) only.
- `overlapping_voices.json` render: `voices` column shows ≥ 2 simultaneously
  active voices; `v0..v3` overlap visibly; the WAV is an evolving, quantized
  melodic texture — the master spec's M2 acceptance demo fulfilled.
- `flow_drone.json` render: continuous non-silent output from t = 0 (drone
  promise) with audible TIMBRE/FILTER breathing; later fires layer over the
  drone.
- Probability 0 in FLOW: output never goes silent. Probability 0 in STEP:
  output decays to silence and stays there.
- All new unit tests pass; existing tests stay green (updated to explicit
  `ENGINE_TEST_TONE` where they depend on the old default).
- Bit-determinism invariant holds.

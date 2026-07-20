# Spotykach Center Section — M4 Design

The center section of the modulation-first firmware: MORPH (equal-power blend
A ↔ B), COUPLE (mutual pull between the two banks), DRIFT (global weather),
SPOT (the eponymous stumble) and SETTLE (the panic gesture). Extends the main
design (`2026-07-10-spotykach-modulation-first-synth-design.md`, section
"Center section"); milestone **M4**, after M3 (capture sequencer), before M5
(sampler adapter).

Fork: `github.com/mcbronkowitch/spotymod`, local at
`c:\Users\bernd\Documents\AI\Spotykach`.

## Goal

Make the interaction between the two parts playable: one fader morphs the
mix, COUPLE pulls the two SuperModulators toward a common clock, DRIFT is a
shared slow weather system, SPOT decorrelates everything with one tap, SETTLE
returns the instrument to what the knobs say. Fully testable on the desktop;
all gestures (ALT/SPOT modifiers, fader layering with pickup and catch-up
slew, CV_CROSSFADE wiring, TAP-hold detection, LEDs) are M6 — engine API now,
as established in M2/M3.

## Decisions (from brainstorming, 2026-07-12)

- **COUPLE = Kuramoto-style, phase AND rate, realized as a PLL.** The pull is
  implemented entirely as rate modulation — no phase jumps ever. Rates
  converge toward the geometric mean; a `sin`-of-phase-error term pushes and
  pulls the momentary rates until the banks lock. In SYNC mode a bank keeps
  its quantized rate (no convergence term) and acts as anchor; the phase pull
  always applies.
- **DRIFT = one weather system, correlated taps.** A single slow
  mean-reverting random walk; six destinations (rate A/B, shape A/B, tune
  A/B) tap it with hardcoded, distinct polarities/scales. Everything breathes
  together but not identically; opposite tune polarities give slow beating
  between the parts.
- **SPOT: the PITCH master lane is immune (live OR replaying).** Pitch is "my
  melody" — the deliberately set anchor everything else stumbles around. SPOT
  skips `LANE_PITCH` entirely (both kicks) and stumbles the other four lanes.
  Consistent with M3 ("the captured loop never mutates", metronomic) and keeps
  CV-out sequences stable for the rack. (By-ear refinement, 2026-07-12: the
  original ±½-cycle phase kick on a *live* PITCH lane threw the melody around,
  so the captured-loop immunity was extended to the live lane too.)
- **Kick anatomy: phase permanent, shape decays.** The per-lane phase kick is
  a real jump (phase is memoryless — lasting decorrelation, which is the
  point); the per-lane shape kick decays back to the knob value over a few
  seconds. Works identically in LOOP and EVOLVE; after the decay LOOP is
  deterministic again.
- **MORPH fades dry and reverb send together** — this **supersedes the
  M1.6 FX spec's §Ownership pre-morph send** ("a part morphed away can
  still haunt the room"): morph 0/1 fully isolates a part including its
  send. When M4 lands, update the `instrument.cpp` ownership comment and
  any M1.6-era pre-morph-send test with it. The same equal-power gain
  acts on a part's dry L/R and send L/R: at the fader extreme the other part
  is completely gone and its already-injected reverb tail rings out
  naturally.
- **Architecture: one `Center` class + narrow hooks** (approach A of the
  brainstorm). `engine/center/center.h/.cpp` owns the weather walk and all
  cross-part logic, runs at control rate (per 96-sample block, as established
  in M2), reads both SuperModulators' phases/rates and writes back through
  four narrow hooks (`rate_scale`, `shape_offset`, per-lane `kick`, per-part
  `detune_cents`). Cross-part logic in exactly one place, testable in
  isolation.

## Behavior model

### COUPLE (PLL, no phase jumps)

Per control tick (every 96 samples) `Center` reads both PITCH-lane phases
(the master lanes) and both base master rates (the knob/sync value, before
any center influence), then computes one rate multiplier per bank from two
parts:

- **Convergence:** the effective master rate is pulled toward the geometric
  mean of the two base rates: `scale_conv = (f_other / f_own)^(couple / 2)`.
  At couple = 1 both banks run at exactly the same speed; at 0 exactly as
  set.
- **Phase pull:** classic Kuramoto as momentary rate adjustment:
  `scale_pull = 1 + couple · K · sin(2π · Δφ)`, applied with opposite sign
  to the two banks. At low COUPLE this is audible as periodic pulling and
  letting go (the "loose relationship"); at 1 it hard-locks within one or
  two cycles.

The combined multiplier is clamped to ~×0.5..×2 momentary (phase never runs
backwards, nothing explodes). **SYNC rule:** a bank in SYNC mode gets no
convergence term (its rate stays clock-quantized) and acts as the anchor;
the phase-pull term always applies to FREE banks. When both banks are in
SYNC, rates are already related through the clock and only the phase pull
acts. A replaying capture loop is pulled along (tempo/phase — consistent
with M3, where live RATE drives loop speed); its content is untouched.

### DRIFT (Ornstein–Uhlenbeck weather)

One slow mean-reverting random walk (time constant ~45 s, output softly
bounded to −1..1 — it never wanders off indefinitely), stepped at control
rate from a seeded `Rng`. Six destinations tap it with **hardcoded, distinct
polarities/scales** (deterministic character, YAGNI over configurability):

| Tap    | Polarity/scale (of full walk, at drift = 1) |
|--------|---------------------------------------------|
| rate A | +1.0 → up to ±½ octave rate shift           |
| rate B | −0.6 → same law, opposite direction         |
| shape A| +0.8 → up to ±0.15 shape offset             |
| shape B| −1.0                                        |
| tune A | +0.5 → up to ±25 cents detune               |
| tune B | −0.9                                        |

`set_drift(0..1)` scales all taps (smoothed). Tune taps are applied as
detune **after** the quantizer — the scale stays intact, the tuning floats;
opposite polarities on A and B give slow beating. The rate taps combine
multiplicatively with the COUPLE multiplier into the single `rate_scale`
hook. `weather()` exposes the raw walk for the CSV and the later SPOT LED
("weather lightning", M6). Exact polarity/scale constants above are starting
values — tune by ear.

### SPOT (the stumble)

`spot()` fires one kick per lane of both banks **except the PITCH master lane**
(immune live or replaying — the melody is the anchor). Per remaining lane,
drawn from a dedicated seeded `Rng`:

- **Phase kick:** uniform ±½ cycle, permanent — the lane simply continues
  from the new point. The one allowed exception to "no phase jumps": a tap
  is a deliberate, audible act, not a creeping state.
- **Shape kick:** uniform ±0.35 offset that decays back to the knob value
  with τ ≈ 1.5 s (inaudible within ~5 s) — the lightning flashes and fades.

DRIFT is slow weather, SPOT is lightning: the kick magnitudes are fixed,
independent of the DRIFT amount.

### MORPH

`set_morph(m ∈ 0..1)` sets equal-power gains `g_A = cos(m·π/2)`,
`g_B = sin(m·π/2)` — full A at 0, full B at 1, both at −3 dB in the
middle. Applied in
the Instrument mix stage to each part's dry L/R **and** send L/R before
summing and feeding the shared reverb. OnePole-smoothed (click-free on
jumps). Boot default 0.5 (both audible). CV_CROSSFADE mapping and the fader
catch-up slew are M6; per-part CV outs and gates are unaffected by MORPH.

### SETTLE

`settle()` (M6 gesture: TAP held ≥ 1 s) — the panic gesture for "why is
everything slowly going wrong":

- DRIFT amount and the weather walk state glide to 0 over ~1 s (no jump).
- All lanes' EVOLVE random-walk states re-center, gliding over ~1 s.
- Open SPOT shape offsets decay along with them.

Afterwards the instrument is exactly what the knobs say. COUPLE and MORPH
are untouched — they were set deliberately, they never creep.

## Module changes

All in `namespace spky`; no heap, no allocation in the audio path, no
libDaisy — as established. `Center` runs entirely at control rate; the only
audio-path addition is the morph gain multiply in the mix.

| Module | Change |
|---|---|
| `engine/center/center.h/.cpp` | **new**: `Center` — owns morph (OnePole-smoothed), couple, drift (smoothed), the OU weather walk + seeded `Rng`, the SPOT `Rng`. `update(...)` per control tick: reads both banks' phases (`pitch_phase()`), base rates and sync modes, writes back `rate_scale` (Kuramoto + drift rate tap), `shape_offset` (drift), `detune_cents` (drift); computes `gain_a()/gain_b()`. `spot()`, `settle()`, `weather()`, `phase_err()`. Testable in isolation. |
| `engine/mod/lane.h/.cpp` | narrow hooks: `kick(float dphase, float dshape)` — phase jump permanent, shape offset decays lane-internally with τ ≈ 1.5 s; no-op on a replaying lane (guard lands in whichever of M3/M4 is built second). `settle()` — EVOLVE walk states and open kick offsets glide to 0 over ~1 s. The shape offset enters the effective SHAPE clamped. |
| `engine/mod/super_modulator.h/.cpp` | `set_rate_scale(float)` (multiplies `_master_hz` after knob/sync — the one hook carries COUPLE **and** DRIFT rate), `set_shape_offset(float)` (bank-wide drift tap), `spot(Rng&)` (draws per-lane kicks, skips the PITCH master lane entirely — live or replaying), `settle()` forwarding; getters `base_hz()` (rate before scale), `sync_mode()`. |
| `engine/parts/part.h/.cpp` | `set_detune_cents(float)` — acts on the pitch handed to the **engine** (post-quantizer); `pitch_cv()` is untouched — the rack CV out stays cleanly quantized, only the internal tuning floats. |
| `engine/instrument.h/.cpp` | owns one `Center`; API in the established delegation style: `set_morph(float)`, `set_couple(float)`, `set_drift(float)`, `spot()`, `settle()`; getters `weather()`, `phase_err()`. `process()` calls `center.update()` once per block and multiplies the morph gains onto each part's dry L/R and send L/R before summing into the output and the shared reverb. Boot defaults: morph 0.5, couple 0, drift 0. |

Deliberately **out of scope** (M6): ALT/SPOT gestures, fader layering with
pickup and catch-up slew, CV_CROSSFADE wiring, TAP-hold detection, LEDs. The
mode switch (stereo routing) stays a later extension per the master spec.

## Desktop render host

- Five new scenario actions, 1:1 with the API: `set_morph`, `set_couple`,
  `set_drift`, `spot`, `settle`.
- `mods.csv` gains five global columns: `morph`, `couple`, `drift`,
  `weather` (the walk), `phase_err` (phase error of the two master lanes).
- Demo **`couple_lock.json`**: both parts synth, clearly different FREE
  rates; COUPLE ramps 0 → 0.4 → 1.0 → 0. CSV shows: at 0.4 the periodic
  pull-and-release in `phase_err`, at 1.0 lock (error → 0, rates converge),
  after 0 they drift apart again.
- Demo **`weather_spot.json`**: DRIFT to ~0.7 (weather audible: tempo
  breathing, tune beating between the parts), a few SPOT taps (phases jump
  in the CSV, shapes twitch and fade), `settle` at the end → back to knob
  nominal, the `weather` column runs to 0.

## Testing

doctest, desktop, as established:

- **Morph:** equal-power law (g_A² + g_B² ≈ 1 across the sweep); morph = 0/1
  → the other part contributes exactly zero (dry **and** send); smoothing
  (no per-sample step above threshold after a fader jump).
- **Couple:** couple = 0 → bit-identical to before (zero-effect invariant);
  couple = 1 from random phase offsets → lock (|phase_err| below a small
  epsilon) within a fixed number of cycles pinned in the plan; rate
  convergence to the geometric mean; symmetry (both banks move toward each
  other); SYNC anchor rule (SYNC bank's rate untouched, FREE bank pulled);
  clamp bounds hold.
- **Drift:** drift = 0 bit-identical; walk bounded to −1..1 and
  mean-reverting; documented tap polarities; detune acts on engine pitch
  while `pitch_cv()` is unchanged; deterministic per seed.
- **Spot:** lanes get independent phase deltas; shape offset below ε within
  5 s; determinism. Replay immunity is designed here as a lane guard but
  only testable once M3 is implemented — the guard lands in whichever
  milestone is built second.
- **Settle:** afterwards the weather contribution and EVOLVE walks reach 0
  within ~1.5 s; output matches a knobs-only reference after the transient.
- **Bit-determinism invariant:** identical scenario → bit-identical WAV.

Sound character (does COUPLE breathe musically? is the weather weather?) is
verified by ear via the renders.

## Budget

CPU negligible: `Center` computes at control rate only (one `sin`, a few
`pow` per 96-sample block, matching the M2 control-rate convention); the
audio path gains four multiplies per part (morph on dry/send). Memory: a few
dozen static floats — no heap.

## Assumptions to verify during implementation

- Kuramoto gain K and the ×0.5..×2 clamp produce a lock within 1–2 cycles at
  couple = 1 without audible wobble at low COUPLE — tune by ear via renders.
- Drift tap scales (½ octave / 0.15 shape / 25 cents) are musical — tune by
  ear; the polarity table is a starting point.
- OU time constant ~45 s: perceivable but not intrusive.
- Mixed SYNC/FREE anchor behavior feels right (SYNC bank as anchor).
- Phase kick ±½ cycle on the LEVEL lane may be too brutal (volume jump when
  SMOOTH = 0) — possibly scale the kick per lane.
- M3 is specced but not yet implemented; if M4 is built first, the replay
  guard in `ModLane::kick` and the replay skip in `spot()` land with M3.

## Acceptance criteria

- Master spec M4 criterion (build order item 4): COUPLE / DRIFT / MORPH /
  SPOT work as one center module on the engine API.
- `couple_lock` demo: CSV shows phase_err ebbing at couple 0.4, converging
  to ~0 at couple 1 with both master rates equal; back at 0 the banks drift
  apart again.
- `weather_spot` demo: drift produces slow correlated motion including tune
  beating; each SPOT visibly kicks per-lane phases in the CSV; after
  `settle` the weather column reaches 0 and the output returns to knob
  nominal.
- morph 0/1 fully isolates one part including its reverb send; the
  equal-power law holds.
- Zero-effect invariants: couple 0 + drift 0 → modulation behavior and all
  pre-existing `mods.csv` columns bit-identical to pre-M4. The mix itself changes once:
  pre-M4 summed both parts at unity (placeholder), equal-power at the
  morph-0.5 default is −3 dB per part — existing pinned scenario WAVs are
  re-rendered (level change only), which the plan calls out explicitly.
- `engine/` still compiles with no libDaisy include; all new unit tests
  pass; existing tests stay green; `src/` untouched.
- Bit-determinism invariant holds.

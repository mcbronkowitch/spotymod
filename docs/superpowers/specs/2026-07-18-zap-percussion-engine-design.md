# ZAP — percussion voice part engine

**Date:** 2026-07-18
**Status:** design approved (brainstorm with Bastian, 2026-07-18), implementation
gated on the hardware bench (STRING precedent)
**Scope:** a fifth selectable part engine (`ENGINE_ZAP`) — a monophonic
two-oscillator FM/AM percussion voice modeled on the Winter Modular ×
Plankton Electronics **Zaps** module. The PITCH lane selects one of 12
percussion archetypes instead of playing a melody; a deterministic seed
scatters the archetypes into banks; the chord layer morphs between them.
No new panel control, no new parameter id. No change to the mod plane, FX,
reverb, or the master chain.

## Problem

SYNTH, WAVE and STRING are all *pitched* engines — the sequencer's rhythm
machinery (groove, variation zones, ratchets, DENS) has no percussive voice
to drive. The reference is Zaps: a digitally-controlled analog percussion
voice — two triangle-core VCOs with shape morph, FM **and** AM
cross-modulation, noise, two AHR envelopes with a log↔lin fading response,
high-pass filters — whose defining idea is **stored sound slots selected by
sequencer CV**: one mono voice that jumps to a different stored sound on
every trigger, so a single voice plays an entire mutating kit.

Spotykach already has everything around that idea: a pitch lane to select
slots, groove/ratchet machinery to drive them, a chord layer that fires
multiple pitches at once, and MOTION as a per-trigger modulation source.
The brief: **translate Zaps' slot-kit concept onto the existing surface** —
strict reinterpretation, zero new controls.

## Decisions (user)

- Sound source: **(c) hybrid kit** — 12 hand-tuned archetypes as the
  skeleton + deterministic seed scatter around them. Pure hand-tuned kits
  go static; pure random kits lack a usable kick/snare foundation.
- Voice: **(a) Zaps-faithful** — 2 shape-morphing oscillators, FM *and* AM
  cross-mod, noise source, 2 AHR envelopes with log↔lin shape morph, plus a
  dedicated fast pitch envelope. Mono buys the CPU headroom for the full
  architecture.
- Slot selection: **(b) pitch class selects, octave transposes** — the
  semitone class (C..B) picks the archetype, the octave transposes it
  (kick → tom, zap → laser melody: Zaps' "melodic percussion"). TUNE
  transposes globally. Envelope times do NOT scale with transposition —
  character survives the octave ride.
- FLOW: **(b) held Hold phase** — the AHR envelopes' H phase sustains until
  the next trigger. The kick becomes a bass tone, the zap a laser drone,
  the snare a noise bed. The drone promise, honored literally.
- Banks & mutation: **(b)+(c)** — DETUNE zones select banks (seeds) with
  scatter growing inside each zone; the MOTION lane adds per-trigger
  mutation depth on top.
- Chord layer: **slot morph** — a chord fires ONE hit whose parameter
  snapshot blends the selected slots. Flams/ratchets explicitly rejected
  (the sequencer's ratchet machinery already owns that territory).
- Mono retrigger: **hard cut** with a 1–2 ms declick ramp — percussion
  lives on the choke.

## Existing infrastructure this reuses

- `engine/parts/engine_iface.h` — `IPartEngine`, implemented directly
  (§1): `trigger`, `trigger_chord`, `set_targets`, `set_cycle`, `set_flow`,
  `set_hold`, `process`.
- `engine/synth/env.h` (envelope base — extended or wrapped for AHR +
  shape morph), `engine/util/fast_sin.h`, `engine/util/onepole.h`,
  `engine/mod/rng.h` (deterministic scatter/mutation).
- Chord layer (spec 2026-07-17): the builder already delivers root +
  chord tones through `trigger_chord`; COLOR shapes note count and
  voicing — the morph consumes exactly that, zero new plumbing.
- Control conventions: 96-sample control cadence (`kCtrlInterval`), FILT
  silence-fade invariant (`_filt_gain`), CHOKE `set_hold` semantics,
  tempo-coupled time knobs (`set_cycle`).
- `host/render/scenario.cpp` engine parsing; VCV per-part engine button;
  bench-firmware workload list.

## Design

### 1. Integration — direct `IPartEngine`, no `SynthEngineT`

ZAP does not fit `SynthEngineT<V>`: the entire allocation machine
(round-robin, oldest-steal, chord slots, per-voice pan fan) is meaningless
for a mono voice. ZAP implements `IPartEngine` directly — integration
surface at test-tone level, voice interior at SYNTH level.

- `EngineId` grows `ENGINE_ZAP = 4`; scenario parser learns `"zap"`; the
  VCV engine button cycles five states (LED shade; panel unchanged —
  hardware-reducibility constraint). No other surface.
- Conventions carried over: all parameter smoothing/derivation on the
  96-sample control cadence; `_filt_gain` silence-fade; deterministic
  `Rng` with a fixed seed-derivation path (part index → bank → slot).
- New files: `engine/zap/zap_engine.h/.cpp`, `engine/zap/zap_voice.h/.cpp`,
  `engine/zap/zap_kit.h` (archetype table + scatter ranges).

### 2. Voice interior

```
Osc2 (tri→saw→pulse) ──┬─ FM ──→ Osc1 (tri→saw→pulse) ─┐
                       └─ AM ──→ (VCA on Osc1)         ├─ mix → HP → out
Noise ── noise VCA ────────────────────────────────────┘
Env1 (AHR, log↔lin) → amp (osc mix + noise)
Env2 (AHR, log↔lin) → routable per slot: pitch | FM amount | noise amp
PitchEnv (fast, fixed shape) → Osc1/Osc2 pitch (the zap sweep)
```

- Both oscillators: polyblep tri→saw→pulse shape morph (SYNTH's
  `morph_osc` reused or trimmed); Osc2 tuned by a per-slot ratio to Osc1.
- FM: Osc2 → Osc1 frequency, per-slot base amount, TIMBRE macro on top.
- AM: Osc2 → Osc1 amplitude, entering above the TIMBRE midpoint (§3).
- Envelopes: **AHR** with a log↔lin shape morph (Zaps' fading response).
  In STEP the hold phase is a short per-slot time; in FLOW it sustains
  (§4). Env2's routing target is a per-slot discrete choice.
- PitchEnv: fixed fast exponential sweep, per-slot depth (semitones,
  bipolar) and time — the signature zap. Env2 routed to pitch gives the
  second, slower sweep on top where a slot wants it.
- Output: one high-pass (per-slot base cutoff, FILTER lane on top), then
  the engine-side `_filt_gain` fade. Mono voice, centered; a small
  per-slot static pan offset is scatter material (tuning pass).
- Retrigger: hard cut — output ramps to zero over 1–2 ms, then the new
  slot fires. No voice crossfade.

### 3. The kit — 12 archetypes, seed scatter, banks

**A slot** is a parameter snapshot: osc shapes, Osc2 ratio, FM/AM base
amounts, noise mix, Env1/Env2 A/H/R times + shape, Env2 routing, pitch-env
depth/time, HP base cutoff, base tune, level trim.

**The 12 archetypes** (pitch classes C..B): kick, tom, snare, rim/click,
clap, closed hat, open hat, low zap, high zap, blip, FM metal (cowbell
corner), noise sweep. Exact snapshot values are tuning material for the
listening pass — the slot list is the contract, the numbers are not.

**Scatter.** Each parameter carries a per-parameter scatter range in the
kit table (envelope times scatter wide, level trim barely). A bank seed +
slot index derive deterministic offsets via `Rng`; the scatter amount
scales them 0..1. Same bank + same amount = the same kit, every run, every
platform.

**Banks (DETUNE).** The DETUNE knob's travel splits into **4 zones**
(DUST/STRING zone precedent, with hysteresis at the boundaries): each zone
is one bank seed; within a zone the scatter amount grows 0 → max. Zone
start = the pure archetypes under a fresh seed; zone end = a heavily
mutated kit. Four kits to dial through, each dosable.

**Mutation (MOTION).** The MOTION lane's engine-side meaning becomes
**per-trigger mutation depth**: at each trigger, the slot's parameters
re-scatter within a radius scaled by the current MOTION value — the kit
mutates *while the sequence runs* ("percussive sequences that mutate over
time" is Zaps' own claim). Deterministic: the mutation Rng streams from
(bank seed, slot, trigger count), so the same sequence + same MOTION
values reproduce bit-exactly. MOTION at 0 = the kit is stable. SYNTH's
pan-fan/drift meaning does not apply (mono); MOTION's mod-plane role
(COLOR target etc.) is untouched.

### 4. Control mapping — strict reinterpretation

| control | SYNTH meaning | ZAP meaning |
|---|---|---|
| PITCH lane | note | pitch class → slot, octave transposes the slot; TUNE global transpose |
| TIMBRE lane | morph + t²·DETUNE_MAX | cross-mod macro: 0 = clean single-osc → FM amount rises → above the midpoint AM blends in (organic → metallic → ring-mod chaos) |
| FILTER lane + FILT | Svf cutoff 60 Hz–14 kHz | high-pass brightness on the per-slot base cutoff (same Hz map); FILT silence-fade invariant unchanged |
| ATTACK | env attack (% of cycle) | Env1/Env2 attack scale — click transient ↔ swelled hit |
| DECAY | env decay (× cycle) | global H+R scale, tempo-coupled — tight kit ↔ ringing kit |
| RESO | Svf resonance | envelope shape morph: log (snappy, percussive) ↔ lin (soft, tonal) — Zaps' fading response as one knob |
| DETUNE | ±35 ct osc spread | 4 bank zones + scatter amount within each zone (§3) |
| SUB | sub-sine level | noise-layer level scale on the per-slot noise mix |
| MOTION | pan fan + drift | per-trigger mutation depth (§3) |
| LEVEL / DENS / GROOVE | (unchanged) | (unchanged: gain, trigger probability, timing) |
| CHOKE | drone release + retrigger pause | the same: releases a held FLOW drone click-free, pauses auto-retrigger |

Exact response curves (attack/decay scale maps, TIMBRE macro breakpoints,
scatter ranges) are tuning material for the listening pass — the table
rows are the contract, the curves are not.

### 5. FLOW — the held Hold phase

- STEP: trigger → A, per-slot H, R. Classic one-shot percussion.
- FLOW: trigger → A, then **H sustains** until the next trigger or a CHOKE
  hold. On handover the hard-cut declick ramp applies (as in STEP); the R
  phase runs only on a CHOKE release. The drone promise fires the current
  slot on entering FLOW. Env2 in FLOW: A then holds its own H — a pitch-routed Env2 parks
  the sweep at its target (detuned drone color), FM/noise-routed Env2
  holds the modulation open.
- CHOKE (`set_hold`): releases the sustaining drone through R, click-free,
  pauses retriggering; release re-arms. Mono makes this trivial.

### 6. Chord layer — slot morph

`trigger_chord(pitches, n)` fires **one** hit whose snapshot is a weighted
blend of the chord tones' slots:

- Each chord tone maps to (slot, octave) as a single note would. The root
  dominates; the remaining weight is shared by the other tones. More chord
  tones (= higher COLOR, denser voicing) pull the sound further from the
  root slot — COLOR becomes a live performance morph without being plumbed
  to the engine at all: the note count and voicing it produces *are* the
  signal. n = 1 is bit-exact the plain `trigger` path.
- Blend rules: times and frequencies interpolate in the log domain, levels
  and amounts linearly; discrete parameters (Env2 routing, quantized shape
  choices) come from the root slot. The blended snapshot then passes the
  same scatter/mutation stage as any slot.
- Deliberate mild accent: the blended level trim rises slightly with n (a
  few dB, ear-tuned) so chord steps read as accents. (The chord builder's
  1/sqrt(n) loudness compensation targets summed polyphony and does not
  apply to a single blended hit.)
- Exact weight curve (root dominance vs. n) is tuning material.
- Because MOTION can modulate COLOR (spec 2026-07-18), sequenced morphing
  percussion falls out for free.
- FLOW + chord: the morphed snapshot sustains like any slot;
  `set_chord`-driven live COLOR moves re-blend the *sounding* snapshot at
  control rate, click-free (parameter smoothing, no retrigger).

### 7. CPU & memory, bench gate

Per sample: 2 polyblep oscillators + 1 noise draw + 3 envelopes + 1
one-pole HP + a handful of multiplies — a fraction of one SYNTH voice, of
which 8 run today; ZAP replaces all 8. Structurally the cheapest engine in
the fork. All snapshot derivation (scatter, morph, macro curves) runs at
control rate.

Memory: the kit table (12 slots × ~20 params + scatter ranges) is a few KB
of flash/static data. No SDRAM, no delay lines.

Bench: one entry in the bench-firmware workload list — "ZapEngine ×2
(both parts)" — same harness, same anchor calibration. Implementation
starts only after the hardware bench has run (STRING precedent), though
the risk here is nominal.

### 8. Hosts & demo scenarios

- Scenario parser: `"zap"`. VCV: engine button cycles
  test-tone → synth → wave → string → zap.
- `zap_kit.json` — STEP, full kit sequence walking the pitch classes,
  DETUNE ride across two bank zones, MOTION ramp (mutation audible).
  Listening + regression anchor.
- `zap_drone.json` — FLOW, held kick-bass and zap-laser drones, CHOKE
  releases, COLOR sweep morphing the sounding drone.
- `zap_morph.json` — STEP + chord layer at rising COLOR: root slot pure →
  hybrid hits.

### 9. Testing

- **Determinism:** same scenario + same seed → bit-identical render on
  desktop and VCV; mutation stream reproducible from (bank, slot, trigger
  count).
- **Slot map:** 12 pitch classes → 12 distinct snapshots; octave changes
  transpose pitch only (envelope times byte-identical across octaves).
- **Zones:** DETUNE bank boundaries carry hysteresis — a knob parked on an
  edge never flutters; zone start ≈ scatter zero.
- **Retrigger:** hard cut completes the declick ramp (no discontinuity
  above threshold) before the new slot fires.
- **FLOW:** hold sustains indefinitely; CHOKE releases click-free and
  re-arms; drone promise fires on FLOW entry.
- **Chord morph:** n = 1 bit-exact equals `trigger`; blend weights sum to
  1; discrete params always from root; FLOW re-blend is click-free.
- **Stability:** max TIMBRE (full FM+AM) + max mutation + extreme DETUNE
  over a long render → bounded output, no NaN/Inf.
- **Parity:** DENS/GROOVE/LEVEL and ratchet machinery drive ZAP unchanged
  (sequencer-side tests re-run against the new engine).

## Out of scope

- Flams/ratchets inside the engine (explicitly rejected — the sequencer's
  ratchet/groove machinery owns rolls).
- User-editable or savable slots (Zaps' SD-card banks) — the kit is the
  compiled archetype table + seeds; revisit only if a preset system ever
  lands.
- Velocity layers, round-robin sample-style alternation beyond the
  deterministic mutation.
- Cross-part choke groups (hat-choke between parts) — candidate for a
  later extension, noted so the idea is not lost.
- New panel controls or parameter ids; any change to GRIT/FLUX/reverb/
  master; per-slot outputs.

# Spotykach FX — FLUX / GRIT / Ambient Reverb Design

Effects for the modulation-first firmware: the original FLUX (tape echo) and
GRIT (drive/reduce) blocks return as per-part, fully modulatable effects, plus
a new shared ambient reverb with per-part modulatable sends. Extends the main
design (`2026-07-10-spotykach-modulation-first-synth-design.md`); scheduled as
milestone **M1.6** — after M1 + scales, before M2 (synth voice), so M2–M5
build on the final signal chain from the start.

## Goal

The stock firmware's FLUX/GRIT are set-and-forget. In this fork, "modulation
is the instrument" extends to the effects: curated FX parameters become
first-class modulation targets driven by the part's existing five lanes —
tape-delay wobble, dub feedback swells and a breathing reverb send get the
same motion character as the rest of the part. The FLUX/GRIT touch pads
(present in hardware, unassigned in the new firmware until now) carry the UX.

## Decisions (from brainstorming, 2026-07-11)

- **Fully modulatable** — FX parameters are modulation targets, not static
  knob values (consistent with the fork's core philosophy).
- **Topology:** FLUX + GRIT per part (as in the original); **one shared
  ambient reverb** behind both parts with a modulatable send per part
  (CPU-friendly; a common room sounds coherent).
- **Routing model:** no new lanes, no routing matrix — the existing five
  lanes are tapped a second time. Fixed 1:1 mapping lane → FX target,
  mirroring the system's "lane index == pad slot == target slot" principle.
- **Curated 5 FX targets per part** (matches the 5 pads in the FX layer):
  GRIT INTENSITY, FLUX TIME, FX MIX, REVERB SEND, FLUX FEEDBACK. All other
  FX parameters (individual FLUX/GRIT mixes, global reverb character) stay
  static layer parameters.
- **Reverb:** DaisySP `ReverbSc` (proven on Daisy, long lush decays) plus an
  optional pitch-shifted feedback path (+12 st) as a switchable **shimmer**
  mode. Global controls: SIZE/DECAY, TONE (damping), SHIMMER amount.
  **[Superseded by M4.5, 2026-07-13]:** the core is now a vendored MIT
  Oliverb port (Clouds Parasite) and **shimmer is removed entirely**
  (`set_shimmer` deleted from the API and scenario actions; the
  DaisySP-LGPL dependency is gone). New global controls: SIZE (true room
  size, Doppler), DECAY (crosses 100 % into a soft bloom), TONE, DEPTH
  (line modulation). See
  `2026-07-12-spotykach-ambient-reverb-v2-design.md`.
- **Implementation approach: port + wrap** — the original `EchoDelay`,
  `Drive`, `Reduce` modules are ported nearly unchanged into `engine/fx/`
  (they depend only on DaisySP DSP classes, which are portable C++). DaisySP
  becomes an `engine/` dependency — M2 planned that anyway. The original
  FLUX/GRIT sound identity is preserved; notably the echo's tape-style time
  slew becomes a feature under modulation (FLOW = wobble, STEP = dub pitch
  jumps).
- **Docs location:** this spec lives in the residency folder; the repo only
  gains a roadmap entry (M1.6).

## Signal flow

```
Part A:  IPartEngine → GRIT (Drive|Reduce) → FLUX (tape echo) → FX MIX ┐
                                                                       ├→ MORPH mix → master
Part B:  IPartEngine → GRIT → FLUX → FX MIX ──────────────────────────┘
              │                                        │
              └── post-FX × REVERB SEND (per part) ────┴→ AmbientReverb (shared) → + master
```

- The reverb send taps **post-FX**, so delay tails wash into the room
  (ambient-typical).
- **FX MIX** is the dry/wet of the whole per-part chain (the modulatable
  target); the original per-effect mixes (FLUX MIX, GRIT MIX) remain as
  static parameters inside the chain.
- Bypass (effect off) is bit-exact dry, switched via the original
  `SoftSwitch` (click-free).

## Engine architecture

New directory `engine/fx/`:

| Module | Contents |
|--------|----------|
| `grit.h/.cpp` | Port of original `Drive` (daisysp::Overdrive) + `Reduce` (Decimator + SampleRateReducer) + `XFade`, mode switch |
| `flux.h/.cpp` | Port of `EchoDelay` / `DeLine` / BPF12: tape echo, feedback soft-clipped, band-passed, smoothed delay time |
| `reverb.h/.cpp` | Wrapper around DaisySP `ReverbSc`; optional shimmer: pitch shifter (+12) in the feedback path, skipped entirely at shimmer = 0 |
| `part_fx.h/.cpp` | Per-part chain: GRIT → FLUX → FX MIX, exposes send tap |

- **Ownership:** `Part` owns its `PartFx`; `Instrument` owns the single
  `AmbientReverb`. Sends are tapped per part **pre-morph** (post-FX); the
  reverb output is added to the master **after** the morph mix. Deliberate
  consequence: a part morphed away can still haunt the room until its send
  is closed — an ambient feature, not a bug.
  **[Superseded by M4, 2026-07-12]:** the M4 center spec applies the morph
  gains to dry *and* send — morph 0/1 fully isolates a part including its
  send; only the already-injected tail rings out. When M4 lands, the
  `instrument.cpp` ownership comment and any pre-morph-send test are
  updated with it.
- **No heap:** delay buffers are injected as in the original firmware
  (`Fx::Params` pattern). Desktop: static arrays; Daisy: SDRAM. 5 s stereo
  echo × 2 parts ≈ 3.8 MB (240 000 samples × 4 buffers × 4 B) —
  comfortably inside the 64 MB SDRAM. *(Corrected 2026-07-12; an earlier
  revision said 7.7 MB, double the actual figure.)*
- **DaisySP** becomes a dependency of `engine/` (portable C++, no libDaisy).
  The desktop CMake build compiles the required DaisySP sources.

## Modulation routing

The 5 FX targets are a **second target row** following exactly the existing
pattern: an `FxTargetId` enum (analogous to `LaneId`), per target
`{active, base, depth}`, and `Part::fx_target_value(i)` computed like
`target_value()`:

```
fx_value(i) = clamp01( base(i) + lane_output(i) × depth(i) × master_depth )
```

No new lane code, no second modulation engine — a second tap on the existing
lanes. Master DEPTH, PROBABILITY, SMOOTH, STEP/FLOW, EVOLVE all apply
automatically because it is the same lanes: the FX breathe in the same
character as the rest of the part.

Fixed lane → FX target mapping (pad slot in the FX layer = lane index):

| Pad | Lane (ratio) | FX target | Musical character |
|-----|--------------|-----------|-------------------|
| 1 | Lane 1 (×2) | GRIT INTENSITY | fast rhythmic texture |
| 2 | Lane 2 (×½) | FLUX TIME | slow tape drift / dub steps |
| 3 | Lane 3 (×1, master) | FX MIX | accents locked to the melody cycle |
| 4 | Lane 4 (×¾) | REVERB SEND | polyrhythmic breathing |
| 5 | Lane 5 (×1½) | FLUX FEEDBACK | swells |

Parameter mapping (normalized 0..1 → DSP):

| FX target | 0..1 maps to | Under modulation |
|---|---|---|
| FLUX TIME | 50 ms … 5 s, exponential | original tape slew retained: FLOW = wobble/chorus, STEP = dub pitch jumps |
| FLUX FEEDBACK | 0 … 1.1 (soft clip catches > 1) | swells up to self-oscillation, as in the original |
| GRIT INTENSITY | drive gain / reduce rate (original curves) | rhythmic tearing |
| REVERB SEND | 0 … 1, equal-power onto wet | room breathing |
| FX MIX | 0 … 1 chain dry/wet (XFade) | accents |

## UX

Mirrors the established gesture grammar exactly (release-based, tap = toggle,
hold = edit layer, soft takeover / MValue afterwards, hold-to-inspect on the
ring). The hardware's per-side FLUX and GRIT touch pads (with LEDs) carry it.

**FLUX pad (per part):**

- **Tap** = FLUX on/off.
- **Hold** = FLUX edit layer while held. Knobs via soft takeover:
  RATE knob → TIME, DEPTH knob → FEEDBACK, SHAPE knob → FLUX MIX,
  SMOOTH knob → REVERB SEND (base). Ring shows current values immediately
  on hold (hold-to-inspect).

**GRIT pad (per part):**

- **Tap** = GRIT on/off.
- **Hold** = GRIT edit layer: RATE knob → INTENSITY, DEPTH knob → GRIT MIX.
- **ALT + GRIT tap** = Drive ↔ Reduce (as `switch_grit_mode()` in the
  original). GRIT LED color: warm = Drive, cold = Reduce.

**Reverb, global (shared):**

- **ALT + FLUX hold** (either side) = reverb layer: RATE knob → SIZE/DECAY,
  SHAPE knob → TONE (damping), DEPTH knob → SHIMMER amount (0 = off, saves
  CPU).
  **[Superseded by M4.5, 2026-07-13]:** shimmer no longer exists; the layer
  now has four axes — suggested map RATE → SIZE, SHAPE → TONE,
  DEPTH → DEPTH (mod), SMOOTH → DECAY, tuned on hardware in M6 (the M6
  shell spec's gesture resolution, which folds this layer into the FLUX
  edit layer, applies on top).

**FX modulation:** while an FX layer is held, the **5 target pads stand for
the 5 FX targets** — tap = toggle that FX target's modulation on/off, exactly
like the main layer. Editing an FX target's mod depth: in the FX layer, hold
the target pad + DEPTH knob (same gesture as engine targets). Base values
come from the layer knobs above.

**Precedence:** FX layers follow the existing "first held wins" rule; while
an FX pad is held, target-pad taps address FX targets, never engine targets.

**LEDs:** FLUX/GRIT pad LED on = effect active; brightness follows the
modulation actually applied (same rule as target pads — a dead chain is
visibly dead). The reverb has no LED of its own; its state shows on the ring
during ALT + FLUX hold.

## Instrument API

Follows the existing normalized-setter style, all paths through the OnePole
smoothers:

```cpp
void set_fx_on(int p, FxBlock which, bool on);      // FLUX / GRIT per part
void set_grit_mode(int p, GritMode m);              // Drive / Reduce
void set_fx_target_active(int p, int i, bool on);
void set_fx_target_base(int p, int i, float n);
void set_fx_target_depth(int p, int i, float n);
void set_flux_mix(int p, float n);                  // static layer params
void set_grit_mix(int p, float n);
void set_reverb_size(float n);                      // global
void set_reverb_tone(float n);
void set_reverb_shimmer(float n);   // [M4.5: removed — now set_reverb_decay/_depth]
float fx_target_value(int p, int i) const;          // for CSV / LEDs
```

## Boot defaults

Philosophy: instantly audible, nothing screams.

- FLUX off, TIME ≈ 0.4 (dotted-ish), FEEDBACK 0.45, FLUX MIX 0.5
- GRIT off, mode Drive, INTENSITY 0.3, GRIT MIX 0.5
- REVERB SEND 0.25, SIZE 0.7, TONE 0.5, SHIMMER 0
  *(M4.5: SIZE 0.6, DECAY 0.55, TONE 0.5, DEPTH 0.25 — shimmer gone)*
- All FX target modulations **inactive** — effects boot static like the
  original; modulation is switched in deliberately per target.

Persistence: like engine targets — the M6 shell's settings storage picks
them up; the desktop renderer sets them from scenario JSON.

## Desktop render host

- New scenario actions, 1:1 with the API: `set_fx_on`, `set_grit_mode`,
  `set_fx_target_active` / `base` / `depth`, `set_flux_mix`, `set_grit_mix`,
  `set_reverb_size` / `tone` / `shimmer`.
- `mods.csv` gains 5 columns per part (`fx0..fx4` = actual FX target
  values) — FX modulation becomes plottable like the lanes.
- Demo scenarios: `dub_delay.json` (STEP lane on FLUX TIME + feedback
  swells) and `ambient_wash.json` (Dorian melody, breathing REVERB SEND,
  shimmer on).

## Testing

doctest, desktop, as established:

- `fx_target_value()` routing: lane modulates base correctly, clamping,
  master DEPTH multiplies in, inactive target = base only.
- FLUX: delay-time accuracy (impulse in, measure peak position), feedback
  decay rate, bypass is bit-exact dry.
- GRIT: Drive/Reduce switch click-free (SoftSwitch), bypass clean.
- Reverb: send 0 → silent wet path; mono impulse produces a stereo tail;
  shimmer 0 → pitch shifter not computed.
- Determinism: identical scenario → bit-identical WAV (existing invariant).

Sound character (does the dub delay feel right? does the shimmer glitter?)
is verified by ear via renders.

## CPU budget

The main spec's < 70 % worst-case target already included FX. Rough Daisy
estimates: 2× (GRIT + FLUX) ≈ 8–10 %, ReverbSc ≈ 10 %, shimmer (when on)
≈ +10 %. The `METER` goes live in the M6 shell; on the desktop, render time
per block serves as an early indicator. Shimmer at 0 skips the pitch
shifter entirely.

## Roadmap placement

New milestone **M1.6 — FX** (after M1 + scales, before M2). Rationale: the
test tone suffices to hear and verify delay/grit/reverb in the renderer, and
M2–M5 then build on the final signal chain instead of rewiring it later.
`docs/roadmap.md` in the repo gets the entry; this spec stays in the
residency folder.

## Assumptions to verify during implementation

- DaisySP compiles cleanly on desktop with the project's CMake/clang setup
  (only the needed sources, not the whole library).
- `ReverbSc` buffer/state fits alongside the echo buffers (SDRAM on Daisy,
  static on desktop) — check its internal memory model when wiring up.
- Original `Drive`/`Reduce` intensity curves translate unchanged to
  normalized 0..1 targets (listen, then adjust mapping if needed).
- Gesture details (ALT + FLUX hold entering the reverb layer vs. plain FLUX
  hold) feel right — tune on hardware in M6; the engine API is agnostic.

## Acceptance criteria

- `engine/fx/` compiles on desktop with no libDaisy include; DaisySP only.
- `dub_delay.json` render: FLUX TIME visibly steps in `mods.csv` and audibly
  pitch-jumps in `out.wav`; feedback swells are audible.
- `ambient_wash.json` render: reverb send column breathes with lane 4; with
  shimmer on, the tail glitters an octave up.
- FLUX + GRIT off and REVERB SEND 0 → output bit-identical to a render
  without the FX stage.
- All new unit tests pass; existing tests stay green.
- CPU early indicator: desktop render time per block increases by a sane
  factor (< 2× vs. M1 baseline with all FX + shimmer on).

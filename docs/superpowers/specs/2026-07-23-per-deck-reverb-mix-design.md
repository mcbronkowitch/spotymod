# Per-Deck Reverb Mix — Design

Splits the single shared reverb MIX knob into one per deck. The reverb stays
**one shared room**; what becomes per-deck is how much each deck feeds the room
(its send) and how much of its own dry it keeps. 100 % on a deck = that deck
fully wet. The central ROOM MIX control is removed; each deck's FX box gains a
`ROOM` knob.

## Goal

Today one `REV_MIX` knob crossfades the *summed* A/B blend against the shared
reverb return at the master join (`Instrument::process`, equal-power). There is
no way to send deck A hard into the room while keeping deck B dry, or to sit the
two decks at different wet amounts — a common ask when one deck is a pad and the
other a rhythmic source. This change gives each deck its own MIX while keeping
the room, its CPU cost, and its character shared.

## Decisions (from brainstorming, 2026-07-23)

- **Variant A — one shared room, per-deck aux send.** Not two reverb
  instances. A second `Oliverb` would roughly double the engine's most
  expensive block (a real M6/firmware budget hit) and add ~130 KB SDRAM for a
  second inline buffer. The ask is "same behaviour, split the mix", not
  "independent rooms", so the shared room is correct and essentially free.
- **The per-deck mix rides the SEND, not a per-deck return.** With one room
  there is only one wet return bus; you cannot apply a different return gain to
  each deck's tail because the tails are already summed inside the room. So the
  equal-power `sin` curve moves onto each deck's send, and the shared return is
  added at unity (the internal `kWetGain` stays baked into the reverb output).
  Each deck's dry keeps the equal-power `cos` curve independently.
- **Endpoints exact.** `mix = 0` → dry `cos 0 = 1`, send `sin 0 = 0` (deck
  fully dry, contributes nothing to the room). `mix = 1` → dry `0`, send `1`
  (deck fully wet). Matches "100 % = komplett wet" per deck.
- **MORPH still scales both dry and send** per deck (`ga`/`gb`), unchanged from
  the M4 rule: a fully morphed-away deck injects no new reverb.
- **Default 0.410 per deck reproduces today's sound.** Because the reverb is
  **linear** below self-oscillation (DECAY < 1), scaling the send by `sin(m)`
  then the return by `kWetGain` is identical to today's scaling of the return by
  `sin(m)·kWetGain`. With both decks at the old default 0.410 the master output
  is bit-near identical to today. See *Defaults & calibration*.
- **One accepted behavioural caveat — the bloom regime.** When DECAY ≥ 1 the
  room self-oscillates into a soft-limited bloom, which is **non-linear**.
  There, "drive the room softer via the send" differs from today's "attenuate
  the bloom output via the return", because the soft-limiter sees a different
  input level. At default DECAY and normal use there is no bloom and the two are
  identical; only a user cranking DECAY into bloom on one deck would hear a
  slightly different bloom onset than the old shared-return knob gave. This is
  the inherent cost of a shared room and is accepted.
- **Central ROOM MIX removed; new knob labelled `ROOM`, in each deck's FX
  box.** "MIX" would be ambiguous beside FLUX (the delay mix). `ROOM` mirrors
  the shared ROOM box — "how much of this deck goes into the room".
- **`REV_MIX` removed cleanly; param ids after it shift.** Chosen over keeping a
  dead placeholder enum. `init.vcvm` is regenerated; old saved `.vcv` patches
  load their reverb/CHOKE/trailing params at shifted ids. Acceptable for a
  pre-release dev instrument.

## Signal flow

```
per deck d in {A, B}:
    dry_d  = pmix_d × ga|gb × cos(mix_d)          // deck dry, morph- and mix-scaled
    send_d = psend_d × ga|gb × sin(mix_d)         // deck send into the shared room

room_in = send_A + send_B
    → AmbientReverb (shared, wet-only, kWetGain inside) → wl, wr

out = dry_A + dry_B + wl|wr  → limiter → out
```

Same injection point as today (`Instrument::process`, the only scope where each
deck's dry and send exist separately). What changes: the dry gain and the
send/wet gain are applied **per deck** before summing, instead of one dry gain on
the blended sum and one wet gain on the shared return.

## Architecture

### Engine — `engine/instrument.h/.cpp`

- **Per-deck mix state.** `_rev_dry_target`, `_rev_wet_target` and the smoothers
  `_rev_dry`, `_rev_wet` become 2-element arrays (index by `PART_A`/`PART_B`).
  Each `set_reverb_mix(part, n)` computes that deck's `cos`/`sin` targets once
  (control-rate libm, same policy as today); no per-sample trig.
- **Setters:**
  - `void set_reverb_mix(int part, float n)` — the real per-deck setter
    (0..1 clamped, exact endpoints, wakes the room if its target > 0).
  - `void set_reverb_mix(float n)` — convenience overload; sets **both** decks.
    Keeps the render host, bench workloads and existing global-mix tests
    compiling and behaving unchanged.
- **Process loop.** Replace the single `l = l·dg + wl·wg` join with per-deck
  gains:
  ```
  const float dgA = _rev_dry[A].process(_rev_dry_target[A]);   // etc. for B
  const float wgA = _rev_wet[A].process(_rev_wet_target[A]);
  float l   = al*ga*dgA + bl*gb*dgB;                 // per-deck dry blend
  float sl  = asl*ga*wgA + bsl*gb*wgB;               // per-deck send blend
  _reverb->process(sl, sr, wl, wr);                  // wl already × kWetGain
  l += wl;  r += wr;
  ```
  `kWetGain` stays inside `AmbientReverb::process`; the return is added at unity.
- **Sleep gate (per-deck OR).** The room sleeps only when **both** decks' wet
  targets are 0 *and* both smoothed wet gains have snapped to 0 — then
  `clear()` once, `_rev_asleep = true`, skip `process()`, discard sends. Any
  deck's target > 0 keeps the room awake. Waking still starts from the cleared
  buffer. `_rev_primed` snap-on-first-block extends to both decks' gains.
- **Null-reverb path** (`init` without `FxMem`) unchanged: pure per-deck dry
  blend, mix setters stored no-ops.

### Facade — `engine/fx/reverb.h/.cpp`

No change. The room already takes a single stereo send and returns a single
wet-only stereo signal with `kWetGain` applied. The per-deck split lives
entirely in the instrument's mix point.

### Panel — `host/vcv/res/gen_panel.py` (+ regenerated `generated_panel.hpp`, SVG)

- **Remove `REV_MIX`** from `SHARED`. The ROOM box becomes a clean 3×2 grid
  (SIZE/DECAY, TONE/DIFF, SMEAR/WOBL); the centre-column MIX slot in row 1 is
  gone. The box may shrink accordingly (bottom edge / height retuned so it still
  reads as a group).
- **Widen `FX_TOP` to 4 columns** = the four `FX_BOT` x-positions, so the FX
  box's top and bottom rows align: `FRATE · FLUX · FFB · ROOM`.
- **Append `REV_MIX_A` / `REV_MIX_B`** at the **end of `PARAMS`** (like
  `FILT_A/B`, `FLUXRATE_A/B`, `COLOR_A/B`), coordinates `FX_TOP[3]` /
  `W - FX_TOP[3]` at `ROW_V1`, kind `SMKNOB`, label `"ROOM"`. They are **not**
  added to `part_controls()` — that would grow `PART_STRIDE` and shift every
  part-B/SHARED id. `PART_STRIDE` stays put; only the trailing appended ids grow
  by two.
- `host/vcv/res/test_panel.py`: drop `REV_MIX`, add `REV_MIX_A`/`REV_MIX_B` and
  their expected coordinates; the ROOM-row coordinate map loses `REV_MIX`.

### Host — `host/vcv/src/Spotymod.cpp`

- `defaultFor`: replace `case REV_MIX: return 0.410f;` with
  `case REV_MIX_A: case REV_MIX_B: return 0.410f;`.
- Process: replace the single `inst.set_reverb_mix(params[REV_MIX]...)` with two
  per-deck calls:
  `inst.set_reverb_mix(PART_A, params[REV_MIX_A].getValue());` and the B pair.
- **Regenerate `init.vcvm`** from the rebuilt module (ids shifted; the init
  snapshot is the source of Initialize state per the init-defaults convention).
- Rebuild + reinstall the VCV plugin with the documented build env
  (`./build-local.sh`, never a hand-rolled g++), so the two `ROOM` knobs are
  usable in Rack.

### Render host / scenarios / bench

Unchanged. The global `set_reverb_mix(float)` overload keeps
`host/render/scenario.cpp`'s `"set_reverb_mix"` action, every scenario JSON, and
the bench workloads working as-is (both decks set together). A per-part scenario
action is possible later but is out of scope.

## Defaults & calibration

- **Both decks default 0.410**, matching the old shared default. Derivation:
  old join at 0.410 → dry `cos(0.410·π/2) = 0.800`, wet `sin = 0.600`;
  master wet = `0.600 · reverb(sum) · kWetGain`. New join at both-decks-0.410 →
  dry `0.800·(al·ga + bl·gb)` (identical), send `0.600·(asl·ga + bsl·gb)` into a
  linear room, return `× kWetGain` → master wet `= kWetGain · reverb(0.600·sum)
  = 0.600 · reverb(sum) · kWetGain` (linear). **Identical below bloom.**
- `kWetGain` (0.40) unchanged.
- No by-ear retune needed for the default; the sanity render just confirms the
  identity. The bloom-regime difference is documented, not tuned out.

## Migration

| Touchpoint | Change |
|---|---|
| `engine/instrument.h` | mix state → 2-element arrays; `set_reverb_mix(int,float)` + `set_reverb_mix(float)` overload |
| `engine/instrument.cpp` | per-deck dry/send gains in `process`; per-deck-OR sleep gate; `init` sets both decks to default |
| `engine/fx/reverb.h/.cpp` | none |
| `host/vcv/res/gen_panel.py` | remove `REV_MIX` from SHARED; widen `FX_TOP` to 4; append `REV_MIX_A/B` (label `ROOM`) at end of `PARAMS`; ROOM box geometry retuned |
| `host/vcv/src/generated_panel.hpp`, `res/Spotymod.svg` | regenerated |
| `host/vcv/res/test_panel.py` | `REV_MIX` → `REV_MIX_A`/`REV_MIX_B`; coordinate map updated |
| `host/vcv/src/Spotymod.cpp` | `REV_MIX_A/B` defaults 0.410; two per-deck `set_reverb_mix` calls |
| `host/vcv/…/init.vcvm` | regenerated (param ids shifted) |
| VCV plugin binary | rebuilt + reinstalled via `./build-local.sh` |
| `host/render/*`, scenario JSONs, `bench/*` | none (global overload) |
| `tests/test_instrument.cpp` | new per-deck independence test; existing global tests unchanged |
| `docs/roadmap.md`, `docs/milestone-history.md` | new milestone entry |

## Testing

doctest, desktop, deterministic:

1. **Per-deck independence.** Deck A mix 1, deck B mix 0, with a dry signal on
   both and equal sources: post-settling, deck B's dry is present at full level
   and B contributes no reverb (mute A's source → wet tail is silent); deck A's
   dry is gone and only its wet remains. Measured on separated A-only / B-only
   source paths.
2. **Endpoints per deck.** For each deck independently: mix 0 → that deck's send
   into the room is zero (its contribution to the wet return is silent); mix 1 →
   that deck's dry is zero.
3. **Default identity.** Both decks at 0.410 with DECAY < 1 → master output
   matches the pre-change build's `REV_MIX 0.410` render within float tolerance
   (bit-near; bit-exactness not required).
4. **Sleep only when both decks dry.** A mix > 0, B mix 0 → room stays awake
   (`reverb_asleep()` false); both → 0 → sleeps after settling; either back up →
   wakes into a cleared buffer, no ghost tail.
5. **No zipper.** Step one deck's mix 0→1 mid-note; sample-to-sample delta stays
   bounded (per-deck smoother works) while the other deck is untouched.
6. **Global overload.** `set_reverb_mix(x)` sets both decks equal — existing
   global mix/bypass tests stay green unchanged.
7. **Determinism.** Identical scenario with per-deck mix automation → identical
   output across two runs.

By-ear: `reverb_wash` / `chord_bloom` renders at defaults confirm "= today";
one A-wet/B-dry render confirms the split does what it says.

## CPU & memory

- The shared `Oliverb::Process` runs **exactly once per sample**, unchanged —
  the engine's heaviest block is untouched. Added cost: one extra dry/wet
  smoother pair (4 `OnePole` vs. 2) and a handful of multiplies per sample.
  Negligible, including on the M6 firmware target.
- No new buffers; memory unchanged (the ~130 KB room is still a single inline
  member). This is the whole reason to prefer Variant A over two instances.

## Roadmap placement

Self-contained: touches the master mix point, the panel generator, and the VCV
host. No interaction with in-flight sampler/groove work. New milestone entry
(minor) after the current head.

## Acceptance criteria

- `set_reverb_mix(int part, float)` and the `set_reverb_mix(float)` overload
  exist engine-side; per-deck defaults 0.410.
- The shared central `REV_MIX` is gone; each deck's FX box shows a `ROOM` knob
  in the top row (FRATE·FLUX·FFB·ROOM), rows aligned.
- `PART_STRIDE` unchanged; `REV_MIX_A`/`REV_MIX_B` are the trailing appended
  params; `init.vcvm` regenerated.
- VCV plugin builds clean via `./build-local.sh`, reinstalls into Rack; manual
  smoke test: A wet / B dry (and vice-versa) behaves as specified, 100 % = fully
  wet for that deck.
- New tests pass; existing suites stay green.
- Default render (`reverb_wash`) at both-decks-0.410 signed off by ear against
  the pre-change build (expected: no audible change below bloom).

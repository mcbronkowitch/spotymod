# Spotykach Reverb DIFFUSION Knob (replaces DEPTH) — Design

Turns the reverb's fixed diffusion into the fourth reverb parameter and
retires DEPTH, whose line-modulation effect was too subtle to earn a knob.
Motivated by the full-wash use case: at MIX/DECAY/SIZE full, synth attacks
still punch through — the Oliverb topology feeds the freshly diffused input
(4 allpasses, ~4–12 ms smear at stock coefficient 0.625) straight to the
output taps at full level, so the room's first pass is essentially a lightly
blurred echo of the attack. More diffusion smears that first pass into the
wash (verified by ear on an A/B render at 0.625 vs 0.85, 2026-07-14).

## Decisions (from brainstorming, 2026-07-14)

- **DIFFUSION replaces DEPTH** — same parameter slot in engine API, render
  host, and VCV panel. `set_reverb_depth` / `set_depth` (facade) disappear
  ersatzlos, like `set_shimmer` did in M4.5.
- **Full mapping range 0–0.90**: AP coefficient = `0.90 · n`. Knob bottom =
  discrete slap-echo cluster (sound-design zone, deliberately reachable),
  today's stock character (0.625) sits at knob ≈ 0.69, top = dense wash.
  Above ~0.90 the allpasses turn metallic-ringy — capped.
- **Line modulation stays, weakly coupled to the knob**: the 9 tail LFOs
  keep running; their amount rides the DIFFUSION knob from a 0.05 to a 0.25
  DEPTH-equivalent (`mod_amount = (0.05 + 0.20·n) · 450` samples). More
  smear = slightly more motion, never dominant. No independent control.
- **Boot default 0.7** — coefficient 0.63 ≈ stock 0.625, modulation ≈ 0.19
  equivalent (previous boot was 0.25): the instrument boots with practically
  today's room, marginally calmer tail.
- **Input swell is explicitly out of scope** (YAGNI): if plucks still read
  through at DIFFUSION 0.90, a slow-attack envelope on the send input is
  the next, separate step — the mechanism was discussed and parked.

## Architecture

### Facade — `engine/fx/reverb.h/.cpp`

- `set_depth(float)` **deleted**; `set_diffusion(float norm)` added:

```
void AmbientReverb::set_diffusion(float norm) {
    norm = clampf(norm, 0.f, 1.f);
    _verb.set_diffusion(0.90f * norm);
    // weak coupling: more smear = slightly more line motion
    _verb.set_mod_amount((0.05f + 0.20f * norm) * 450.f);
}
```

- The `kDiffusion` constant and the boot `set_depth(0.25f)` call go away;
  `init` boots via `set_diffusion(0.7f)`. `kModRate` (LFO speed) is
  untouched.

### Engine — `engine/instrument.h`

`set_reverb_depth` removed; `set_reverb_diffusion(float)` added (same
null-guarded forwarding style as the other reverb setters).

## Migration

| Touchpoint | Change |
|---|---|
| `engine/fx/reverb.h/.cpp` | `set_depth` → `set_diffusion` (mapping above); `kDiffusion` const removed; boot default 0.7 |
| `engine/instrument.h` | `set_reverb_depth` → `set_reverb_diffusion` |
| `host/render/scenario.cpp` | action `set_reverb_depth` removed; `set_reverb_diffusion` added |
| `host/render/scenarios/ambient_wash.json` | `set_reverb_depth 0.348` → `set_reverb_diffusion 0.7` (character-preserving; by-ear check) |
| `host/vcv/res/gen_panel.py` | `REV_DEPTH` → `REV_DIFF`, label **DIFF**, same panel position; regen `generated_panel.hpp` + `Spotymod.svg` |
| `host/vcv/src/Spotymod.cpp` | `defaultFor`: `REV_DIFF` → 0.7; forwarding → `set_reverb_diffusion` |
| VCV plugin binary | rebuild + reinstall (documented build env, incl. `CC=gcc CXX=g++` on `make install`) |
| Tests | depth references migrated; new diffusion tests (below) |
| `docs/roadmap.md` | milestone entry **M4.9** |

## Testing

doctest, desktop, deterministic — as established:

1. **Sweep safety:** sweeping DIFFUSION 0→1 while the room rings stays
   bounded, no NaN, sample-to-sample delta bounded (no clicks).
2. **Discreteness metric:** impulse response at diffusion 0.0 concentrates
   energy in fewer, sharper events than at 0.9 — assert via an early-window
   peak-to-RMS (crest) comparison: crest(0.0) > crest(0.9).
3. **Coupling:** the facade forwards both the AP coefficient and the
   mod amount from one call (verified behaviorally: two rooms at diffusion
   0 vs 1 diverge in tail motion, not just density — or via a focused
   mapping assertion if the core exposes state; the plan decides).
4. **Determinism:** identical scenario with a diffusion ride → bit-identical
   across two runs.
5. Null-reverb safety: `set_reverb_diffusion` on an engine-only Instrument
   is a no-op, no crash.
6. Existing suites stay green with depth references migrated, not weakened.

By-ear scope: full-wash pluck render (the 2026-07-14 experiment scenario) at
DIFFUSION 1.0 — attacks should melt into the wash noticeably beyond the old
0.85 test render; boot-default render of `ambient_wash` ≈ today's character.

## Acceptance criteria

- `set_reverb_diffusion` exists engine-side, in the render host and the VCV
  host (`REV_DIFF`, label DIFF, default 0.7); grep-clean: no
  `set_reverb_depth` / facade `set_depth` reference left in code, tests,
  scenarios, or generated panel.
- All tests pass; VCV plugin builds clean and is reinstalled into Rack.
- Determinism invariant holds (identical scenario → bit-identical WAV).
- By-ear sign-off by the user: full-wash pluck at DIFF 1.0, and
  `ambient_wash` at boot defaults (≈ unchanged character).

## Roadmap placement

**M4.9 — Reverb DIFFUSION knob** (after M4.8, before M5). Self-contained
facade + host change behind the stable reverb API.

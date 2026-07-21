# Spotykach — Milestone History

This is a chronological, commit-level record of how each milestone actually
happened: commit ranges and SHAs, plan defects caught during execution,
review verdicts, test counts, retired scenarios, by-ear/play-test rounds,
and release tags. It was extracted on 2026-07-21 from the assistant's memory
file (which had grown to 79% of the entire memory store) so the detail lives
beside the code instead of in a context window.

`docs/roadmap.md` is the forward-looking, feature-level counterpart: it
already gives the authoritative "what's built, what's planned" status table
and per-milestone feature descriptions through M5a and Bench. Where this
document repeats a milestone's scope, treat roadmap.md as the summary and
this file as the archive of *how it got there* — commit SHAs, false starts,
reviewer verdicts, and the specific by-ear/play-test iterations, none of
which roadmap.md carries.

Current state (which milestones are merged/pushed/released today) lives in
the assistant's `spotykach-milestone-status` memory, not here — that file is
kept current; this one is not.

---

## M1 — Engine foundation (2026-07-10, merged 2026-07-11)

Plan: `docs/superpowers/plans/2026-07-10-spotykach-modulation-first-engine-foundation.md`.
Merged into `main` via fast-forward on 2026-07-11 (16 commits); branch
`feat/m1-engine-foundation` deleted afterwards. All commits were rewritten to
the GitHub noreply email `29505413+mcbronkowitch@users.noreply.github.com`
(repo `git config user.email` set to it) to pass GH007 push protection —
later work kept using that email. The public fork was renamed `Spotykach` →
`spotymod`.

Delivers the portable `engine/` core (`namespace spky`, no libDaisy, no
heap): `Rng`, waveform bank, RANGE, `ModLane` (FLOW/STEP/EVOLVE incl. rate
wander), `SuperModulator` (5 lanes, ratios ×2/×½/×1/×¾/×1½), `Part` + engine
interface + `TestToneEngine`, `Instrument` public API; plus the desktop
render host (`render` CLI, scenario JSON → WAV + `mods.csv`) and 34 doctest
cases. Build via the desktop build env (clang+ninja+vendored headers, no
MSVC; `source env.sh`).

### Scales layer (2026-07-11, on `main`)

Pitch quantization (6 scales, SCALE/CHROM/FREE, root) on the PITCH lane; UI
wiring deferred to M6.

## M1.6 — FX (2026-07-11)

Implemented subagent-driven from plan `docs/superpowers/plans/2026-07-11-spotykach-fx.md`
(spec `...-fx-design.md`). 11 commits (`2c8300a`..`c5b62d6`), 77 doctest
cases green. Delivers: `daisysp_min` desktop build; `engine/fx/` =
`fx_util.h` (XFade/SoftSwitch ports), `grit.*`, `flux.*` (injected echo
buffers, `Flux::kMaxSamples=240000`), `reverb.*` (`AmbientReverb` =
ReverbSc + optional +12st shimmer, ~530 KB, always static), `part_fx.*`
(chain GRIT→FLUX→FX MIX + equal-power reverb send; ids `FXT_GRIT_INT=0,
FLUX_TIME=1,FX_MIX=2,REV_SEND=3,FLUX_FB=4`); `Part` 2nd FX target row +
`init(sr,seed,echo_l,echo_r)` + 4-output `process`; `Instrument` `FxMem` /
`init(sr,FxMem)` / FX API + shared reverb post-mix; host: 10 scenario
actions, 5 FX CSV cols/part, `dub_delay` / `ambient_wash` demos.

Acceptance verified: bit-exact bypass, determinism, CPU ~1.09× baseline. One
plan test ("grit click-free") was rewritten with user approval to compare
ramped-vs-always-on baseline (the Drive fuzz itself slews >0.1/sample at
intensity 0.7, unrelated to the switch). SoftSwitch API fixed for M2:
`set_on(bool,bool immediate=false)`, `is_on()`, `is_idle()`, `process()`.

**By-ear listen: one real bug found and fixed (commit `2d02b01`).** The
shimmer reverb feedback ran away to +Inf (click then silence in
`ambient_wash` after ~2 s). Root cause: shimmer re-injected the
pitch-shifted wet at a fixed 0.5 gain while the room's own gain is
~1/(1-feedback), so loop gain exceeded 1 at large sizes. Fix: scale shimmer
feedback by `(1-reverb_feedback)` + a SoftClip safety net; added a stability
regression test (78 cases); retuned `ambient_wash.json` to sane levels.
`dub_delay`'s "rubber band" effect turned out to be the intended FLUX TIME
(delay-time) modulation by the STEP lane sweeping 0.19–4.2 s — not a bug, a
taste knob, left as-is.

DaisySP headers leak transitively through `part.h`/`instrument.h` from here
on (fine per plan; PIMPL was noted as an M6 option if a DaisySP-free
non-fx build is ever wanted — never pursued).

## M2 — Polyphonic synth voice (2026-07-11)

Executed via a sequential build-gated workflow (one general-purpose agent per
TDD task, stop-on-failure, plus a final independent constraint/acceptance
auditor) from plan `docs/superpowers/plans/2026-07-11-spotykach-synth-voice.md`.
9 commits `5e9bbc6`..`6e40c27` (fast_sin → MorphOsc → Env → Voice →
SynthEngine → Part engine-select → Instrument API → host scenario/CSV →
demos+docs), 113 doctest cases green. Audit passed all 6 checks, 0
violations: fresh `rm -rf build` rebuild 100%; no libm sine in the voice
audio path (all `std::pow`/`std::exp` control/trigger-rate only); engine
purity (no heap/vector/`<random>`, sole DaisySP header `Filters/svf.h`, sole
`Rng` seeded once in `Voice::init`); `src/`/Makefile/firmware `main.cpp`/
`app.*` untouched; acceptance renders pass (overlapping_voices hits 4
simultaneous voices; flow_drone never silent incl. the prob-0 24–36 s
stretch; determinism byte-identical).

Delivers `engine/synth/` = `fast_sin` (`engine/util/`), `MorphOsc`, `Env`,
`Voice` (2× MorphOsc + sub + `Svf` + env + equal-power pan/drift),
`SynthEngine` (4-voice round-robin, oldest-steal retrigger, FLOW
sustaining-drone w/ auto-trigger+demotion, target map
TIMBRE/FILTER/PITCH/MOTION/LEVEL, control-rate per 96 samples);
`IPartEngine` no-op `set_cycle`/`set_flow`; `Part` boot-default
`ENGINE_SYNTH` + click-free SoftSwitch engine switch + flow/cycle forwarding
+ `trigger_manual`; `Instrument` synth-voice API; host 7 scenario actions +
`voices`/`v0..v3` CSV + `overlapping_voices.json`/`flow_drone.json` demos
(existing 7 scenarios pinned to `test_tone`).

**Two plan defects found and fixed during execution:** (1) the `Env`
retrigger test pre-rolled 24000 samples (0.5 s) but the AD envelope idles at
−80 dB ≈ 0.45 s (~21.6 k samples) → `before==0` failed `CHECK(before>0)`;
corrected to 12000 in both the fork test and the plan doc (implementation
was correct per the locked design decision). (2) plan prose said "+7 voice
cases" but the verbatim block had 6 `TEST_CASE`s — code treated as
authoritative.

Renders were not committed: `renders/` is gitignored in this fork (the
plan's "repo already tracks renders/" premise was false); WAVs are
regenerable. Spec `docs/superpowers/specs/2026-07-11-spotykach-synth-voice-design.md`.
UI wiring (VOICE edit layer, PLAY-tap trigger, engine-switch gesture)
deferred to M6.

## M3 — Capture sequencer (2026-07-12)

Subagent-driven from plan `docs/superpowers/plans/2026-07-12-spotykach-capture-sequencer.md`
(spec `...-capture-sequencer-design.md`). 9 commits `f7c6cc2`..`b30e801`,
137 doctest cases green, pristine. Approach B: new header-only
`engine/mod/capture.h` `CaptureLoop` (two 192-slot double buffers;
`kSlots=192`; `reset`/`record`/`capture_now`/`value`/`fired`/`valid`; a dumb
buffer, no timing of its own — `ModLane` drives slot indexing via one
`_phase_slot()` helper so record & replay share the mapping); `ModLane`
gains `set_capture_loop(CaptureLoop*)` (PITCH lane only, others keep
`nullptr` = unchanged), rolling record of pre-smooth `_target`+fired into
the ring every generative sample (pure write, never touches RNG →
bit-determinism preserved), and a replay rule (`set_replay`/`replaying`):
boundaries come from recorded fired slots, live PROBABILITY dice roll per
fired slot (fail = frozen hold), SMOOTH/RANGE + Part base/depth/TUNE/
quantizer stay live, EVOLVE fully ignored on the replaying lane;
`SuperModulator` owns one `CaptureLoop` wired to `LANE_PITCH` in `init()` +
`capture_now`/`set_replay`/`replaying`/`loop_valid`; `Instrument` per-part
delegation; host `capture_now`/`set_replay` scenario actions +
`a_cap`/`b_cap` CSV cols + `capture_loop.json` demo.

Final Opus whole-branch review: Ready to merge = YES, all 5 acceptance
criteria PASS, no Critical/Important.

**Plan corrections made during execution:** (1) plan said "Part needs no
change" but `Part::mod()` was non-const → added a minimal
`const SuperModulator& mod() const` overload for the const Instrument
accessors. (2) several plan tests compared a sampled target stream across
cycles, which is flaky against a pre-existing ~0.0006/cycle float32
`_phase` drift that jitters samples at the loop's hard ±1 transitions
(slots 0/96) — rewrote determinism/EVOLVE/persistence + Instrument
TUNE/scale tests to drift-immune exact comparisons (two identically-seeded
instances that drift identically, or a snapshot of the frozen 192-slot
buffer), and added a criterion-1 unit test asserting
`target()==loop.value(slot)` at every slot over 3 cycles. (3) Task-2 test
helper `shape=0.9→0.75` (0.9 lands in the S&H blend region where
`_sh_cycle` is RNG-redrawn per cycle; 0.75 = pulse/S&H boundary, zero
random weight).

Known accepted minors (non-blocking): no bounds-check in `CaptureLoop`
(matches `rng.h` caller-guarantees convention); the EVOLVE
phase-rate-guard branch isn't unit-covered (confirmed correct by code
review; an exact test was deemed too fragile). Bit-determinism
independently re-verified. UI wiring (ALT+SEQ gesture, ring step display)
deferred to M6.

### Entropy sequencer (2026-07-12, reworks the lane core)

Subagent-driven from plan `docs/superpowers/plans/2026-07-12-spotykach-entropy-sequencer.md`
(spec `...-entropy-sequencer-design.md`; supersedes a same-day
per-step-S&H spec explicitly banner-marked "do not implement"). 5 fork
commits `f205197`..`f259046`, 145 doctest cases green; residency doc
commits `11bcdfe`+`4980cbe`.

Delivers: bipolar ENTROPY replaces EVOLVE everywhere (`set_entropy` −1..+1
through ModLane/SuperModulator/Instrument; scenario action renamed
`set_evolve`→`set_entropy`; positive = old EVOLVE walk scaled, negative =
walk decays ×(1+0.2·e)/cycle toward neutral, 0 = frozen); looping S&H step
buffer `_seq[32]` per lane (pre-filled at init from seed; STEP reads
`_cur_step % 32`, FLOW slot 0; per-wrap redraw removed — the LOOP contract
now holds in the S&H zone); `_mutate_slot()` on fired steps only (dice =
entropy², GROW `v=(v+r³·lerp(0.5,2,e))·0.9` clamped, ERODE `v·=0.6`
snap-to-0 below 0.02; constants `kGravity`/`kErode`/`kRootSnap` in
lane.cpp, ear-tunable); `shape_value()` fixed so shape==1 returns the S&H
operand exactly (old code left 0.01% pulse bleed).

Master spec updated: switch 2 = ERODE/LOOP/GROW (M6 must map it, detents
±0.4), ENTROPY macro bullet, signal-path diagram. `demo_step_melody.json` =
entropy showcase; capture demos melodized. Final Fable whole-branch review:
Ready to merge = YES.

**Known interaction (judged benign, documented):** positive entropy's
`_ev_shape` walk pulls effective shape below 1.0 → slight pulse bleed in
the S&H zone; two plan test seeds changed 42→14/12 for this confound
(thresholds untouched, reviewer reproduced independently). Accepted polish
backlog: `_ev_*` erode decay never snaps to 0 (LOOP-after-erode not
bit-exact); no freeze-after-wander test; stale EVOLVE comments in
`melody_then_drift.json`.

**By-ear listen DONE (3 rounds, all fixed):** 64th-note rates retuned to
8ths/quarters; demo part B (unconfigured default lanes = pitch siren)
silenced; part A LEVEL S&H clicks → LEVEL target turned off (lesson: STEP+
S&H on LEVEL at low SMOOTH clicks by construction on sustained tones). New
demo `entropy_duet.json`. Dev-diary entry published with 4 embedded
renders.

## M6 firmware shell — spec done (2026-07-12)

Spec `docs/superpowers/specs/2026-07-12-spotykach-firmware-shell-design.md`
(commit `48ded8b`): portable `shell/` UI core (`PanelIn`→`Instrument`+
`LedFrame`, no libDaisy/heap) + a thin `app_mod.cpp` Daisy adapter.
Resolves the accumulated pad-overbooking problem with a targets-first
rule: the plain layer of the 5 per-side pads (PLAY/REV/GRIT/FLUX/SEQ =
target slots 1–5, GRIT=PITCH, SEQ=LEVEL/no-LED) stays the master-spec
targets, and ALL other functions move behind ALT matching the printed
labels — superseding M2's plain PLAY-tap trigger (now ALT+PLAY) and
M1.6's plain FLUX/GRIT gestures (now ALT+FLUX/GRIT); the reverb layer
folds into the FLUX edit layer; Drive↔Reduce becomes a two-zone knob. FX
edit layers stay sticky while ALT is held; scale select is the SHAPE knob,
8 zones inside the PITCH-pad edit layer; MIDI is minimal (clock in + note
ch1/2→A/B); persistence is one versioned QSPI block named `"mod"` (all
hand-set values; COUPLE/DRIFT/MORPH/capture stay volatile); the scenario
host gains panel events + `params.csv`/`leds.csv`. Verified against a
product photo of the real hardware: one big knob per side (TUNE, with a
ring around it), RATE/DEPTH are small blob knobs, input trimmers are
analog-only, and the faceplate already prints "record"/"+alt" hints
confirming the ALT scheme independently. (This spec predates the later
scale-re-home and edit-layer-latch amendments below, and the pad/gesture
scheme was revisited repeatedly through 2026-07-16/17 as CHOKE, FILT, MOD/
TIDE and the step-clock landed — see those sections for what actually
shipped; M6 implementation itself has not started as of M5a.)

## Cross-spec review + amendments (2026-07-12, commit e638cab, residency repo)

Three parallel reviewer agents (UX / embedded / promise-accounting) audited
all 8 specs against the fork; essential findings folded back into the
specs before M4/M5/M6 implementation started. Key amendments implementers
had to honor: (1) M6 scales re-home to a global-scale + per-part-mode
model, mode = 3-zone SHAPE knob in the PITCH edit layer (an earlier 8-zone
per-part idea was dropped). (2) ALT-entered edit layers latch; a plain ALT
tap exits. (3) M6 gained 4 engine deltas: `set_pitch_cv_offset` (V/Oct,
pre-quantizer), `trigger_note` (MIDI), master SoftClip at the Instrument
mix, `fast_sin` swap in `shape_value`+`part_fx`. (4) M5 spec amended:
`set_monitor`/`sampler_monitor` input monitoring, loading = clear +
fill-follows. (5) M4 supersedes M1.6's pre-morph reverb send. (6) FX spec
echo-buffer figure corrected to 3.8 MB. (7) M6: pads/sr_165 polled in the
main loop only (never ISR), LED frames composed in the audio tick, factory
state + BOOT-held-at-power-on reset, 100 BPM default, EVOLVE 0.3/1.0 +
SMOOTH floor 0.35 constants, SETTLE also clamps FLUX FEEDBACK bases >0.95,
sampler content gestures, `arm-none-eabi-size` acceptance, first hardware
measurement = METER reverb+shimmer alone.

## M4 — Center section (2026-07-12)

Plan `docs/superpowers/plans/2026-07-12-spotykach-center-section.md` (10
TDD tasks). Executed subagent-driven on `main` via two build-gated
background workflows (`spotykach-m4-center` = tasks 1–7 then correctly
STOPPED at task 8 on a real plan-test bug; `spotykach-m4-finish` =
corrective commit + tasks 9–10 + final whole-branch review) — one
implementer + Opus task-reviewer + one fix pass per task, stop-on-failure.
Fork commits `d784bc7`..`aa6c921` (M4) + `fc41ae2` (docs), base `db7e731`;
170 doctest cases green. Final whole-branch review: approved / ready to
merge, all acceptance criteria PASS, determinism bit-identical (double-
render `cmp`), no libDaisy in engine, `src/` untouched.

Delivers `engine/center/Center` at control rate (per 96 samples) + 4 narrow
hooks: `SuperModulator::set_rate_scale` (COUPLE×DRIFT rate, splits
`_update_rate`→`_apply_rate`, adds `base_hz`/`sync_mode`),
`set_shape_offset`, `Part::set_detune_cents` (post-quantizer; `pitch_cv()`
stays clean), per-lane `ModLane::kick`/`set_shape_offset`/`settle`. MORPH =
equal-power `cos/sin(m·π/2)` on dry AND reverb send (supersedes the M1.6
pre-morph-send rule; morph-0.5 boot = −3 dB/part, a deliberate level
change — no test pins audio so existing tests stayed green); COUPLE =
Kuramoto PLL (geometric-mean convergence + sin phase pull, SYNC-anchor
rule, clamp ×0.5..×2, `kK`=0.15); DRIFT = OU weather walk (τ≈45 s,
tanh-bounded) with 6 hardcoded taps; SPOT per-live-lane kick (phase
permanent, shape decays τ=1.5 s, replaying PITCH lane immune); SETTLE panic
glide. Host: 5 scenario actions + 5 `mods.csv` cols (`morph,couple,drift,
weather,phase_err`) + `couple_lock`/`weather_spot` demos. Zero-effect
invariant (couple 0 + drift 0) verified bit-identical vs a bare
SuperModulator.

**M4 deviations flagged in the final review (non-blocking, ear-check
later):** (a) morph smoother 0.03 s not 0.02 s — inaudible, fine. (b)
DRIFT response ~quadratic not linear — implementers scale the OU noise by
the smoothed `_drift` in `_step_weather()` AND the taps by `_drift` in
`update()`, so `weather()`≈0 at drift 0 and the knob feels exponential;
rationale was SETTLE robustness (smoothed drift never hits exactly 0);
deviates from the spec's linear "scales all taps." If the drift taper ever
feels off, scale only the taps and gate the OU on `_drift_target`. (c)
COUPLE mixed SYNC+FREE was only exercised by a unit test that checks the
anchor is unchanged, NOT that the FREE bank reaches the anchor's rate —
for a large detune the free bank may lock only with a static phase offset.
(Two implementer plan-test bugs were caught & fixed: SPOT decay test was
too strict at the sensitive shape-0.5 pulse edge → rebased to shape 0.2 +
relative-decay; the morph-isolation test demanded the shared-reverb tail
vanish within 1 s → split into exact dry-isolation + decaying-send-
isolation.)

Residency-repo M4 docs: plan committed (`201f4e8`); dev-diary "Center
section" entry + `couple_lock`/`weather_spot` web renders (`83b4c1a`).

### M4 by-ear pass (2026-07-12)

User heard `weather_spot`/`couple_lock` pitch wandering wildly and set the
rule: **pitch must not be a drastic target of other (center-section)
modulations.** Root cause (found via CSV, not the center module): both
demos drove the PITCH lane full-range (default `range` 1.0 + high base) = a
3-octave siren; SPOT/DRIFT were negligible there. Fixes: (1)
`SuperModulator::spot` now skips `LANE_PITCH` entirely (live or replaying)
— pitch immune to the stumble, the other 4 lanes still lurch; commit
`5c09ddf`, proven byte-identical pitch column with vs without SPOT. (2)
Both demos retuned to stepped pentatonic melodies in a moderate unipolar
range with low pitch bases; commit `42acefe`. Residency spec/plan/dev-diary
updated + web renders re-encoded (`69aaa88`). 170 tests still green.

## M4.5 — Ambient Reverb v2 (Oliverb port) (2026-07-13)

Spec `docs/superpowers/specs/2026-07-12-spotykach-ambient-reverb-v2-design.md`
(commit `fe29fdd`), plan `docs/superpowers/plans/2026-07-12-spotykach-ambient-reverb-v2.md`
(`a4e7a26`); M1.6 FX spec superseded-notes `e13f4cb`. Executed
subagent-driven, 7 fork commits `3de9002`..`84202b8`, 181/181 doctest cases
green. Delivers: vendored MIT Oliverb (Clouds Parasite) under
`third_party/oliverb/` (`stmlib_shim.h` + deterministic `RandomOscillator`
on spky::Rng/fast_sin + float-only `fx_engine.h` + `oliverb.h` — ×1.5
constants for 48 kHz, pitch shifter removed, per-sample Process,
control-rate `Prepare()`, size smoothing 0.0002 for ~100 ms Doppler rides,
slope /300); facade `AmbientReverb` (SIZE Doppler / DECAY crosses 1.0 at
0.9 cap 1.05 / TONE / DEPTH; `set_shimmer` deleted everywhere; buffer 128 KB
inline member — M6 SDRAM placement must account for the embedded buffer, no
separate FxMem pointer); DaisySP-LGPL fully unlinked, repo MIT-clean;
scenarios migrated (old `set_reverb_size` had actually meant decay —
semantics moved to `set_reverb_decay`); `ambient_wash.json` = showcase
(bloom ride 20–30 s, Doppler size ride 36–46 s, 60 s).

**Clipping saga:** the plan's showcase values clipped — bloom plateaus
near full scale at core output taps (2× tap gain + Hermite overshoot under
depth mod); fixed after 4 passes with `kWetGain = 0.40f` (−8 dB,
ear-tunable 0.40–0.50) wet trim in reverb.cpp + retuned mix (LEVELs
0.40/0.32, sends 0.20/0.18, depth 0.348, bloom decay 0.94); final peak
30853 / 0 clips / deterministic. Final Fable review verified transcription
against actual upstream: Ready to merge; a stmlib_shim MIT-notice gap
(plan defect) fixed in `84202b8`.

## M4.6 — Dynamics (2026-07-13)

Spec `docs/superpowers/specs/2026-07-13-spotykach-dynamics-design.md`
(`d2c5de7`), plan `docs/superpowers/plans/2026-07-13-spotykach-dynamics.md`
(`7730831`), shell-spec delta-3 supersession `51a6815`. Executed
subagent-driven, 7 fork commits `2cc6b2c`..`5156b96`, 199/199 doctest
green. Delivers: `Comp` one-knob compressor per part (`engine/fx/comp.*`,
end of PartFx before the reverb send tap — dry AND send compressed; knob =
loudness first: thr −32a dB / ratio 1+9a² / release 60→350 ms / auto-makeup
`−thr×(1−1/ratio)×kMakeupComp`; stereo-linked detector, gain computer
decimated ×16, amount 0 = bit-exact bypass w/ re-arm) + `Limiter`
(`engine/fx/limiter.h`, stmlib recipe stereo-linked, exactly bit-transparent
below −1 dB knee at drive 0, tanh ceiling asymptote 1.0 — delivers M6
Engine delta 3 early; vendored daisysp::Limiter rejected: unconditional
SoftLimit(x·0.7)) + `set_comp(p,n)`/`set_master_drive(n)` (pre-gain 1–4×),
boot 0/0, denormal floors 1e-9. Host: 2 scenario actions + `comp_pump.json`
showcase (RMS arc raw 2603 < glue 2703 < dense 8343 < pump 10311, peak
32766, deterministic). Final Fable review: Ready to merge = YES; pre-M4.6
bit-identity verified against a true `8fc1b1a` worktree build (14/15
scenarios byte-identical; `ambient_wash` 15 samples ≤109 LSB accepted as an
above-knee limiter touch).

**By-ear round 1 (2026-07-13):** user heard clipping at 0:26 and a 0:56
outro cliff. Diagnosis: makeup 0.9 lifted the dense/pump program envelope
to ~−4 dBFS → peaks grounded the limiter tanh continuously. Root fix
`e678de0` in the gain computer: `kEnvCeiling 0.4` (−8 dBFS post-comp
envelope cap; quiet keeps full makeup), asymmetric gain smoothing (down
0.5 ms / up 2 ms), attack scales 5→2 ms with amount (supersedes fixed
5 ms; spec amended). 200/200 tests (new env-cap test), arc
2603/2703/7056/7430 monotonic. `kMakeupComp` stays 0.9 — now safe under the
cap. Re-listen APPROVED (round 2). New dev-diary showcase `m7_bloom.json`
(`e2f45ed`): spread Am7 strum, tail 2197 → comp ride 8117 RMS (+11 dB
resurrection), zero limiter contact. Dev-diary entry "One knob louder"
published. M6 notes: GRIT layer SMOOTH→COMP, FLUX-layer TUNE (ex-shimmer)
→MASTER DRIVE (collision w/ reverb DECAY/DEPTH axes unresolved); the
limiter is load-bearing against the comp post-silence makeup burst.

## M4.7 — Melody engine rework (2026-07-14)

On fork branch `melody-engine-rework`. 15 commits `fb4f6ab`..`f77e9ca` +
post-completion `152b88e` (DRIVE warm saturation). Removes
PROBABILITY/capture/entropy APIs in favor of MELODY (bipolar VARIATION),
DENSITY, 5 phrase PRINCIPLEs, new_phrase; VCV plugin fully migrated
(`gen_panel.py` is the single source of truth for the panel). 13 old
scenarios retired (kept as mp3/ogg). Follow-ups owned by the user: audition
principles in VCV, fresh dev-diary showcase, VCV PRINCIPLE dataToJson
persistence.

### M4.8 — Reverb dry/wet MIX (2026-07-14, merged into `melody-engine-rework`, HEAD `b42327a`)

Spec `docs/superpowers/specs/2026-07-14-spotykach-reverb-drywet-design.md`,
plan `...plans/2026-07-14-spotykach-reverb-drywet.md`. 8 commits
`fef3857`..`b42327a`, 199 cases green, final Fable review Ready-to-merge
(fix applied). Delivers `set_reverb_mix` (equal-power, exact endpoints,
10 ms OnePole glide, at the master join in `Instrument::process`),
clear-on-sleep CPU bypass at MIX 0, scenario action + VCV `REV_MIX` knob.
**Default 0.25 is a by-ear choice, deliberately leaner than the old join**
— MIX multiplies on top of `kWetGain`, so the pre-M4.8 balance sits at MIX
0.5 (−3 dB overall); the original spec claim that 0.25 ≈ old balance was a
controller arithmetic error, corrected in spec/roadmap/code. Accepted
minors incl. REV_MIX param-id insertion before TEMPO shifting Rack
saved-patch ids (append-after-TEMPO in `gen_panel.py` until the M6 panel
freeze).

### M4.9 — Reverb DIFFUSION knob (2026-07-14, merged into `melody-engine-rework`, HEAD `b27052f`)

5 commits `8867021`..`b27052f`. DEPTH removed ersatzlos; `set_reverb_diffusion`:
AP coeff `0.90·n` (0 = slap echoes, boot 0.7 ≈ old stock, 1 = wash that
melts attacks — the full-wash motivation: Oliverb feeds the diffused input
straight to the output taps), line-mod weakly coupled `(0.05+0.20·n)·450`.
VCV `REV_DIFF` "DIFF" in REV_DEPTH's enum slot (param-id stable). Final
Fable review: Ready to merge = Yes. Caution carried forward:
`Part::set_depth`/`Instrument::set_depth(int,float)` is the unrelated lane
macro — never confuse with the removed reverb depth (see the gotchas
memory).

## SYNC/COUPLE redesign (2026-07-16, released v2.1.0/v2.1.1)

Spec `docs/superpowers/specs/2026-07-16-sync-couple-redesign-design.md`,
plan `...plans/2026-07-16-sync-couple-redesign.md`. Subagent-driven, 11
commits `ad06645`..`1637d07`, 216 doctest green, final Fable whole-branch
review Ready-to-merge after one polish commit (`987866e`).

Delivers: per-part `SyncMode` (Free/Sync/Triplet) removed everywhere →
global `Instrument::set_sync(bool)`; `engine/mod/divisions.h` 17-rung
speed-sorted ladder (straight+dotted+triplet, `division_index =
round(norm*16)`) + `free_hz` moved there; `engine/center/transport.h`
`Transport` beat accumulator (double; `clock_pulse()` snaps to nearest
beat, `reset()` zeroes downbeat) owned by Center; `SuperModulator::
set_rate_scale(pitch_s, mod_s)` split; Center grid world (SYNC on:
per-bank pitch servo onto transport — sin-shaped hard-lock law
`kKHard·sin(2π·err)` capped, NOT the plan's linear form — adjudicated:
linear failed the 0.02 lock bound under EVOLVE wander; COUPLE gates only
mod-lane DRIFT wander via `drift^(1−couple)`) + free world (pairwise
Kuramoto unchanged; COUPLE>0.5 smoothstep grid gravity toward nearest
ladder rung + downbeat, hard at 1.0 with the same sin law); VCV: panel
layout A (params 63→62, stride 24→23 — breaks saved 2.0.x patches), CLK
edge now also phase-aligns transport, RST jack finally live, RATE tooltip
shows division name when synced / Hz when free; plugin 2.1.0 built +
installed. Init RATE defaults remapped to the ladder.

**v2.1.1** added init part-A RANGE=1.00; user's Rack play test passed
("geht alles"). Residency-repo note: a rogue commit `8c865d0` had deleted
the whole website locally — reverted, was never on origin. Post-merge test
backlog left: Instrument-level `set_sync`/`clock_pulse`/`reset_transport`
integration test; SYNC flip mid-run continuity; free-world full-COUPLE lock
under DRIFT=1.

## Ranked-slot groove / rhythm engine (2026-07-16)

Spec `docs/superpowers/specs/2026-07-16-rhythm-groove-design.md`, plan
`...plans/2026-07-16-rhythm-groove.md`. Subagent-driven, 6 commits
`1db7458`..`f191786`, 229 doctest green, final Fable review READY TO MERGE
after one fix commit. Replaces the metric-weight gate threshold with
composed rhythm: `pg_gen_groove`/`GrooveCell` in phrase_gen.h — per-phrase
seeded firing-order ranking, period = motif length L, syncopation via
push/anticipation displacement, slot-0 anchor pinned to rank 0; DENSE =
monotonic ranking depth (`_groove_k`, k = max(1, round(density·L)); turning
the knob only adds/removes notes from the same groove, never re-rolls);
composed note durations 1..4 steps on `gate_state()`; RENEW keeps the
groove, NEW PHRASE/step-length change re-rolls it.

**GATE jack routing (final-review catch, plan defect):**
`ModLane::note_sustain()` (step+melodic qualified) →
`SuperModulator::pitch_sustain()` → OR'd into `Part::gate()` with the 5 ms
pulse; at DENSE 1 the jack is continuously high (spec'd legato). Also
fixed mid-flight: stale note state on live FLOW→STEP toggle (guarded clear
in `set_step`); 3 silence-window tests pinned to DENSE 0. Deferred to user
(by ear): Rack play test (DENSE sweep, GATE articulation, FLOW/STEP
toggle), listening pass, tuning constants in `pg_gen_groove` (sync degree
0.15–0.75, push bonus 0.05, note_len distribution 55/25/15/5).

### Groove variation zones (2026-07-16)

Spec `docs/superpowers/specs/2026-07-16-groove-variation-zones-design.md`,
plan `...plans/2026-07-16-groove-variation-zones.md`. Commits
`65b4bf6`..`5b1f985`, 235 doctest green, final Fable review READY TO MERGE
(+ follow-up fix for odd-L bounds). MELODY (VARIATION) knob is now zoned:
`|v|` 0–0.25 = melody-only (groove frozen, exactly old behavior); 0.25–1.0
= per-wrap dice `r²` (`r=(|v|−0.25)/0.75`) triggers at most one groove
mutation (GROW: adjacent-rank swap or note_len ±1; RENEW: push flip or
nudge, full re-roll only at `|v|≥0.9`). All mutations preserve permutation/
anchor/len invariants structurally → DENSE monotonicity survives. LOOP
draws nothing.

**Determinism gotcha found here:** zone-1 melodic STEP lanes draw the
(always-failing) dice each wrap → RNG stream ≠ previous release at same
seed; renders not byte-comparable across versions by design (fixed draw
count per wrap; see the gotchas memory). Also that day: `demo_step_melody.json`
retuned (was 32 notes/s straight 16ths relying on boot defaults; now a
2-bar cycle, DENSE 0.55, synth voices, DENSE-sweep tail, commit `c3cce41`).

### CHOKE event-priority knob (2026-07-16)

Spec `docs/superpowers/specs/2026-07-16-choke-priority-design.md`, plan
`...plans/2026-07-16-choke-priority.md`. Subagent-driven, commits
`7ca4ffe`..`ff9ce34`, 243 doctest green, final Fable review READY (no
must-fixes). Center control CHOKE (Trimpot at CX/y=51 between SCALE and
DRIFT, appended last — default 0 = bit-identical off): negative = A
priority/B yields, positive mirrored.

**Rev. 2** after user play-test (commit `7834fec`): the original continuous
curve "fühlt sich kaputt an" → five snapped states (off, per-side "choke",
per-side "choke thru decay"). Dice/`_claim`/choke-Rng deleted. Skip-not-
delay: `Part::set_inhibit` blocks engine trigger + gate pulse + latches the
suppressed note's STEP sustain out of `gate()`.

**Rev. 3 bugfix (commit `7291895`, tag v2.2.1, PUSHED + released):**
"choke only works with equal rates" — root causes: stage-1 window was the
literal GATE signal (FLOW 5 ms pulses ≈ never open); the yielder's FLOW
drone was immune. Fix: both stages became env windows — stage 1 = priority
LOUD (gate high or max voice env > 0.1), stage 2 = audible (> 1e-4); new
`IPartEngine::set_hold(bool)` demotes the yielder's FLOW drone click-free
while inhibited.

**Rev. 4 (commit `ebed761`, tag v2.2.2, final semantics):** rev-3's loud
window (env > 0.1) over-blocked with long decays. Final: stage 1 = while
the priority side HOLDS a note (STEP: gate high; FLOW: always counts as
held via new `Part::flow()`), stage 2 = additionally through the audible
decay (env > 1e-4). New regression test pins skip-not-delay: with choke −1
and different rates, the yielder's lanes/pitch CV/fire slots stay
bit-identical to an unchoked run. Suite 244/244. Releases v2.2.0/v2.2.1/
v2.2.2 all tagged+CI-built.

## FILT cutoff-trim knob (2026-07-17, pushed as v2.3.0, not tagged/released)

Spec `docs/superpowers/specs/2026-07-17-filt-knob-design.md` (v2 — the v1
HP-half was cut for CPU: no new filter at all), plan `...plans/2026-07-17-filt-knob.md`.
Subagent-driven, commits `a6196dc`..`1cd5e56` (base `64f1896`), 253 doctest
green, final Fable review READY TO MERGE. Bipolar FILT per part (Trimpot,
voice row now 6 knobs `ATK DEC FILT RES SUB DTUN`, FILT_A/_B appended last
— stride 23 unchanged, patches compatible): offset in normalized lane space
before the voice SVF (left ×1.25, right ×1.0), below the 60 Hz rail a
`_filt_gain` fade through the LEVEL OnePole → full left = silent for any
lane, center bit-identical, full right = 14 kHz open. Deferred to user (by
ear): FILT sweep both halves, fade constants, faded part still fires
gates/CV/CHOKE windows, high RES+COMP at full left.

## MOD & TIDE — modulation surface split (2026-07-17)

Spec `docs/superpowers/specs/2026-07-17-mod-tide-design.md` (+ errata),
plan `...plans/2026-07-17-mod-tide.md`. Subagent-driven, commits
`140e0fd`..`3f49711` (base `a1dc9fc`), 263 doctest green, final Fable
review READY TO MERGE (the 1 Important was a spec defect, resolved as
errata commit `3f49711`). Delivers "modulation first": MOD (ex-DEPTH, per
deck) now scales only the 4 texture lanes + FX targets (`Part::target_raw`
exempts LANE_PITCH); RANGE = melody ambitus only; TIDE (new global center
knob right of MORPH, default 0.5 = bit-identical) scales texture-lane rate
¼×–4× via `_tide_mult`; synced → 9-rung reciprocal ladder `kTideRatios`;
REV_MOD label → WOBL; VCV init defaults = old RANGE·DEPTH products (A 0.78,
B 0.236); plugin 2.4.0 built+installed.

**Play-test revs:** (1) LEVEL floor (commit `921ec81`) — `target_raw`
clamps LANE_LEVEL to ≥ `kLevelFloor`(0.4)·base. (2) Boot-targets (spec
`2026-07-17-boot-targets-design.md`, commit `1315319`) — `_active` boots
all-true (was PITCH+LEVEL only; root cause of "MOD reads as tremolo" in
VCV), `_tdepth` staggered boot {SOURCE 1, SIZE 0.55, PITCH 1, MOTION 0.7,
LEVEL 1}. Suite 266/266. **RELEASED 2026-07-17 as v2.4.0** (fork pushed +
tag, CI success, all 3 assets live; website downloads + diary updated).

### Step-clock — STEPS/Tempo decoupled (2026-07-17)

Spec `docs/superpowers/specs/2026-07-17-step-clock-design.md` (+
`_ev_phase` errata), plan `...plans/2026-07-17-step-clock.md`. Subagent-
driven, commits `4442794`+`052849f` (base `63d08c5`), 272 doctest green,
final Fable review READY TO MERGE. STEP mode runs RATE as a step clock with
8-step reference: `cycle_hz = rate_hz·8/steps`; at 8 steps factor exactly
1.0 = bit-identical to before. Composes multiplicatively with
COUPLE/DRIFT/TIDE/lane ratios.

**Rev. 2 bugfix (commit `e4edf87`)** after a Rack test found SYNC still
following STEPS: `Center::_grid_servo` was targeting `beats×cpb`,
overpowering the step-clock. Fix: `ModLane::clock_scale()` (8/steps STEP, 1
FLOW) scales the SYNC servo target AND the free-world grid-gravity phase
target. Suite 273/273. **Rev. 3 (commit `5dbad12`)** after another Rack
test: live-STEPS turns briefly sped/slowed the loop under SYNC — the grid
servo targeted total-steps mod NEW count while `set_step()` preserved local
position. Fix: `Center::_rebase_grid()` rebases a per-bank offset onto the
lane's phase on a clock_scale change. RST is now also the bar resync
(`Instrument::reset_transport()` clears grid offsets + calls
`SuperModulator::reset_phases()` — snap instead of servo drag). Suite
276/276.

## Chord layer — COLOR knob (spec 2026-07-17, implemented 2026-07-17) — M4.10

Spec: `docs/superpowers/specs/2026-07-17-chord-layer-color-design.md` (rev
with fork commits `2ca6ed0`+`4e4c902` perf-review amendments), plan
`docs/superpowers/plans/2026-07-17-chord-layer-color.md` (commit
`d04d8e7`). Implemented subagent-driven: commits `ac65350`..`5452c2a` (10
commits), all 7 tasks + final Fable review + fix. Suite 311/311 (6.54M
assertions).

**COLOR-0 bit-identity gate PASSED post-fix:** 4 baseline renders
byte-identical (demo_step_melody, ambient_wash, demo_density_sweep + a 4th
FLOW/overlap scenario `chord_baseline_flow.json` that lived only in the
session scratchpad). Delivers: `engine/pitch/chord.h` ChordBuilder (zones
w/ hysteresis .02, additive ladder root/fifth-down/third/seventh, ninth
swap at .85, voice-leading lay search ≤27 candidates, exact-root
passthrough); `IPartEngine::trigger_chord`/`set_chord` defaulted virtuals;
SynthEngine STEP stabs (seeded ≤8 ms micro-offsets, 1/sqrt(n) vel w/ slew) +
FLOW multi-sustain surface; Part wiring; VCV COLOR_A/B knobs appended last
(patches compatible, default 0), plugin 2.4.1 rebuilt+installed.

**Final-review catch (empirically confirmed):** collapse→re-bloom steal
cannibalized a sustaining surface voice → whole-surface re-attack; fixed
`5452c2a` (steal prefers oldest non-sustaining + n<1 guard, regression test
RED→GREEN).

### COLOR as a MOTION target (2026-07-18, extends M4.10)

Spec `docs/superpowers/specs/2026-07-18-color-motion-target-design.md`,
plan `...plans/2026-07-18-color-motion-target.md`. Subagent-driven, 3
commits `eabaee6`..`f5230b6` (base `512aad5`), suite 318 cases / 6.586M
assertions, Opus final review READY TO MERGE. MOTION becomes COLOR's third
destination (`kColorMod = 0.2f`, `kColorGate = 0.01f`, ear-tunable). Bipolar
additive (not multiplicative) so reach stays constant across the knob
range. No new surface at all: git diff over lane_id.h, instrument.h,
host/vcv/, chord.h and res/ is empty.

**Two plan defects caught in-flight:** (1) the spec's own test probe
`COLOR = 0.005` is arithmetically impossible (gate halves the swing;
2-note edge sits at 0.145) — plan probe corrected to `0.02`. (2) the plan
told a task to `git add renders/*.wav|csv`, but `/renders/` is
`.gitignore:11` — implementer substituted direct hash/byte proof instead
(stronger than the plan's `git diff --stat`, which would have silently
false-negatived on a gitignored path). **Compatibility note:** every saved
`.vcv` patch with COLOR > 0 now sounds different on reload, since
`_active[LANE_MOTION]` boots true and `_depth` boots 1.0. Deferred to user
(listening pass, engine builds only, was never heard): FLOW bloom/collapse
at COLOR ~0.35/~0.6, level pumping on sustained pads, is `kColorMod = 0.2`
the right reach, COLOR ~0.85 ninth-flip musicality, COLOR ~0.02 occasional
dyad.

## M5 sampler spec redone (2026-07-18)

New spec `docs/superpowers/specs/2026-07-18-sampler-texture-deck-design.md`
(commits `c516b0a`+`e8e066e`); the residency-repo 2026-07-12 sampler-adapter
spec is banner-superseded (residency commit `05088ab`) — its slice-player
trigger model was dead. Key decisions: sampler = granular cloud (texture,
not a second melodic instrument); live input first (IN L/R wired through
`Instrument::process`), WAV second; PITCH quantized + chord cloud (grains
round-robin over COLOR's chord notes); MOTION = scatter macro order→chaos;
STEP = groove-gated bursts; panel = per-part ENGINE switch (SYNTH/SMPL) +
latching REC button w/ LED; architecture B = fresh 8-grain scheduler +
verbatim-copied `Buffer` record core; 42 s stereo injected buffer per part;
synth-neutrality bit-identity gate (counterpart of the COLOR-0 gate).

**Critical-review amendments (commit `b6be79b`, 4-question pass):** (1) the
per-part ENG pad already existed (latched Synth↔Test tone) — remapped to
Synth↔Sampler, test tone moved to a context menu; only REC button+LED is
new panel surface. (2) voice row remapped so no knob goes dead on a
sampler part (ATK/DEC = grain-window halves, FILT/RES = one stereo Svf,
SUB = octave-down grain share, DTUN = per-grain cents spread). (3)
play-while-recording: fill-follows in the record path. (4) factory sample
auto-loads on switch-to-sampler with an empty buffer, never overwrites
content. (5) honest SDRAM caveat: scattered grain reads bypass cache,
sampler tops the hardware-benchmark list. Plan deliberately not written yet
at spec-approval time — user decided to write it in an Opus session.

## Bench firmware (spec 2026-07-18, executed 2026-07-18)

Spec `docs/superpowers/specs/2026-07-18-bench-firmware-design.md`
(commit `be30496`). Standalone `bench/` app (own main+Makefile, libDaisy
pattern, links engine/+third_party/+full DaisySP — bench never ships so
LGPL is fine; `src/` untouched). 3 workload families: (1) own system
decomposed, (2) DaisySP expansion candidates per voice vs a MorphOsc-voice
reference anchor, (3) SRAM-vs-SDRAM A/B (8-grain scattered-read proxy for
M5 risk, Oliverb 128 KB placement, shortened echo window). Hybrid method:
offline DWT enumeration (budget = 2 ms/960k cycles @ block 96/48 kHz — not
460800, a self-review-caught arithmetic error) + audible in-callback anchor
mode via CpuLoadMeter. Per-workload output checksums defeat DCE and prove
cross-run determinism. `bench/run.py`: one command builds, flashes via the
user's debug probe (openocd, prefer RAM-load), captures RTT, writes a
committable `docs/bench/YYYY-MM-DD-<hash>.md`+`.csv` with a go/no-go
verdict. Plan executed on `main` 2026-07-18.

## CPU hunt round 1 — mod-plane cut + ablation family (2026-07-19)

Merged FF from branch `cpu-hunt` into local `main`, 7 commits
`20af2b5`..`55c1cda`. Both plans in `docs/superpowers/plans/`
(`2026-07-18-mod-plane-optimization.md`, `2026-07-18-bench-ablation-family.md`
— the latter carries its `## Outcome`). Final Fable whole-branch review:
Ready to merge, zero Critical/Important. Delivers bench families `mod` (7
rows) + `abl` (14 rows); one engine edit (`waveforms.h wave_sine` →
`fast_sin`). Mod plane 33%→26% of budget (prediction ~17% fell short — the
per-lane sine-call estimate was too high; real saving ~66 cyc/lane/sample);
`instrument_worst` ~159%→~151% (avg; 156% max); budget now closes with a
7.8%-of-budget residual attributed to additive stacking.

Closure findings: Part glue = 112820 cyc/part (~23% of budget for both
parts, the single biggest cut target); FLUX coupling measured NEGATIVE
(solo rows overestimate composition); tanh ≈60% of FLUX's solo delta; GRIT
Reduce +78% vs Drive; CHOKE tax + driven-limiter tax both measured.

**Gotchas carried from this round:** `util/fast_sin.h`'s header comment
still claims "~10–15 cycles" — empirically ~50–65 at these call sites
(never fixed, a cheap follow-up flagged); `docs/bench` keeps exactly one
committed result pair at a time, so historical Outcome citations dangle by
design; `run.py` names result files by HEAD hash, so measuring before
committing mislabels them — always commit code before measuring; one-time
icache layout shifts of ~7% on small rows appear when a new translation
unit links in — same-run-only arithmetic is the defense.

## CPU hunt round 2 — independent audit + mod-plane control-rate (2026-07-19)

On branch `cpu-hunt` (shared working tree with a parallel part-glue
session; commits interleave). Independent audit ranked further findings:
(1) mod plane to control rate (~22-24 pt ceiling), (2) FLUX feedback cap
≤0.95 → delete `std::tanh` outright (~8 pts, kills the bloom feature —
needs its own spec + listening decision), (3) global voice cap 6-of-8 (~9
pts worst case), (4) DaisySP `Svf` is double-sampled (single-pass LP-only
≈2-4 pts), (5) osc B ≈ duplicate of osc A at TIMBRE≈0 (conditional skip
~⅓ voice cost).

**4 hygiene commits landed:** `7e9f924` DeLine kMaxSamples 240000→262144 +
explicit AND masks; `8723bc5` one shared L/R delay-time slew in Flux;
`f88501a` Comp curve cache (update only when the OnePole moved); `7ea004b`
Oliverb random LFOs on the /32 raster + linear interpolation on the 4 input
diffusers + 2 static loop APs. First three proven byte-identical on 4
renders; the reverb cut changes references (tails decorrelate; 0.5 s RMS
envelopes agree 0.2 dB mean / 1.4 dB max) — renders NOT re-cut, held for
the user's listening pass. Suite 319/319 green.

**Hardware bench measured** (`docs/bench/2026-07-19-c7f6a73.md`):
`instrument_worst` 150.8/156.4 → 117.4 avg / 123.1 max offline, anchored
118.4/123.2 — total −33 pts but still over budget. Row deltas vs `9be5df9`:
`part_glue_flow` 29.92→19.86, `oliverb_solo_sram` 19.44→9.52 (halved),
`fx_flux_sdram` 10.28→10.16, `mod_plane` 26.4 unchanged (expected — its cut
hadn't landed yet).

**Mod-plane control-rate cut: commits `095ef2a`..`94468af`.** Spec
`docs/superpowers/specs/2026-07-19-mod-plane-control-rate-design.md` + plan,
subagent-driven, final Fable whole-branch review READY TO MERGE (0
Critical/Important). Approach C: 4 texture lanes/part advance via new
`ModLane::tick()` (boundary replay in phase order, exact OnePole compound
coefficient); PITCH lane stays per-sample — fires/gates/pitch_cv
bit-identical, no PITCH/groove/CHOKE/center test touched. Permanent
per-sample-vs-tick equivalence suite (9 `tick:` cases). 333/333 green,
`ctrl_identity.sha256` re-pinned. Accepted asymmetries (spec'd):
engine-swap and RST leave texture lanes ≤95 samples stale/skewed until the
next tick. Measured saving ~17-19 pts as predicted (see the release entry
below for the final measured figure).

## CPU hunt CLOSED + released as v2.6.0 (2026-07-19)

`cpu-hunt` fast-forwarded into `main` (42 commits, HEAD was `69d674b`),
`plugin.json` 2.5.0 → 2.6.0 (minor: COLOR/MOTION + the control-rate rework
change behaviour, not just speed), tag `v2.6.0` pushed, CI green, GitHub
Release carries win-x64 / mac-arm64 / lin-x64. Branch `cpu-hunt` deleted.

**The fast-tanh listening pass CLEARED** (both named targets checked by
ear: echo bloom at max feedback and master DRIVE high — neither audible),
recorded in `docs/roadmap.md` and the fast-tanh spec's Outcome (commit
`507f0e0`). Final measured state: anchored max 97.69%, under the 100% gate
for the first time, margin 2.3 points — GRIT Reduce alone would eat ~2.2 of
it, so the remaining ranked cuts should be held as margin, not spent.

Still open at that point: the residency website's per-platform download
links pointed at 2.5.0 (user's own follow-up); the older per-milestone
by-ear backlog; the M4.9/M4.10-era "VCV Rack play test + audio listening
pass deferred" notes in `docs/roadmap.md`.

## DUST/ROT — rhythm-fed delay taps (2026-07-20, released v2.7.0)

Merged FF into `main` (52 commits), pushed, `plugin.json` 2.6.1 → 2.7.0
(minor: both knobs changed meaning, saved patches sound different), tag
`v2.7.0` pushed, CI green on all four jobs. Branch `dust-explore` deleted
locally and on origin. Spec `docs/superpowers/specs/2026-07-20-rhythm-fed-delay-taps-design.md`
(rev 2) + plan.

**The DUST grain cloud was CUT** — it failed the listening gate twice. The
diagnosis both revisions missed: evenly spaced taps ARE a delay (zone S
played 2/4/8/16 equal slots per beat is a short delay by construction,
however precisely gridded). Rev 2 adds the missing half: a groove is an
unequal spacing that repeats. Replaced by `TapBank` — two read taps on the
FLUX tape whose offsets are the sample distances between the last onsets
of the OTHER bank's PITCH lane, latched once per that lane's cycle.
`derive_offsets` guarantees non-uniformity by rule (uniform gaps get spread
by the MOTION lane's ×3/4) plus an exhaustive property test. No beat anchor
needed: `DeLine::Write` decrements, so a constant offset behind it is 1×
forward playback — the fact that killed the earlier revision is the
mechanism here; `Center::beat_edge`/`beat_samples`, `Flux::sync_beat` and
the seed chain were removed. DUST = tap morph (accent hierarchy), ROT =
spectral spread (0 = deliberately delay-like). New:
`engine/fx/taps.{h,cpp}`, `engine/mod/rhythm_view.h`, an onset ring on
`SuperModulator`. 384 cases green.

**Key play-test fact:** both banks boot in FLOW where onsets are cycle
wraps, so gaps are uniform and the guard always fires — irregular tap
placement needs the partner bank in STEP with DENSE < 1. The knob that
shapes the figure is the other bank's DENSE/STEPS, not DUST. DUST is also
inert (~2.6 s at default rate) until the partner publishes a valid rhythm.

**Bench measured** (`docs/bench/2026-07-20-5d53901.md`): `instrument_worst`
93.37/98.61, `instrument_worst_taps` 99.92/106.00. Taps cost +6.55/+7.39 for
both parts. User accepted being over budget at worst case — deliberate,
not to be reopened unprompted. Two measured optimisations landed after a
+3.5-point baseline regression was traced (`TapBank::active()` scanning
cold taps per sample = 1.73 pts; the onset ring living on all 10 lanes =
0.93; layout 0.81): recovering 2.77. Caveat: code layout alone moves
`instrument_worst` by ~0.8 points — it read 92.61 and 93.37 across two
commits differing only in comment text plus an added bench row.

## Spotymod faceplate redesign (2026-07-18/19, plugin.json bumped to 2.5.0, not pushed/tagged)

Spec `docs/superpowers/specs/2026-07-18-spotymod-faceplate-redesign-design.md`,
plan `...plans/2026-07-18-spotymod-faceplate-redesign.md`. Subagent-driven,
commits `0dd1483`..`d6c0d4f` (7 tasks + 2 test-guard fixes + Opus final
review). Panel-only: no DSP/parameter/behavior change, and the enum
ParamId block is byte-identical to the pre-redesign commit, so every saved
`.vcv` patch still opens. Explicitly drops the old hardware-reducibility
rule for the VCV panel specifically (all controls exposed on the panel;
the hardware shortlist comes later from live play). Delivers a 9-knob
sector orbit (MOTION/TIMBRE/PITCH, COLOR promoted from satellite into
PITCH), one shared fieldset style across VOICE|FX|PLAY per part, 5 jack
groups. Architecture: `PanelCtl` carries absolute label position/anchor/
size/colour + a `tip` column — the duplicated `glyphR` table in
Spotymod.cpp was deleted so the SVG preview and Rack runtime cannot drift.
New `res/test_panel.py` guard script freezes the enum order + geometry.

**Lesson worth carrying: two guards in that file were vacuous on
arrival** (asserted on strings the generator never emitted —
`width="42.000"` vs `42.0`, collar `r+1.0` vs `r+0.85`); both found by
review, fixed, and each fix proven falsifiable empirically. Always
red-prove a new guard. Dev-diary entry "Infinitely many knobs" published.
Deferred to user: the 4 live Rack checks (label legibility, group legends,
loading a pre-redesign patch, jack tooltips); tag `v2.5.0` is the user's
call.

## M5a — Sampler texture deck + generous parameter ranges (2026-07-21, merged commit `499e5a7`, not pushed)

Two specs, both in the fork: `2026-07-18-sampler-texture-deck-design.md`
(the deck) and `2026-07-21-sampler-generous-ranges-design.md` (the
ranges). Merged into `main`, `--no-ff`, 53 commits. 481 doctest green;
synth neutrality and determinism byte-identical. `plugin.json` stays 2.7.0,
no tag.

The ranges spec's governing rule — **a ceiling stays only if it prevents
an actual failure, never merely an unpleasant sound; where opening a path
lets it diverge, add the bounding nonlinearity the instrument already
has** — is Bastian's stated philosophy for the whole instrument, not just
the sampler (the hardware-reducibility constraint still binds the panel
independently of this rule).

**Things flagged as would-bite-next-session at the time:**
- Neutrality-gate methodology: byte-identity comparison against an older
  commit requires a matching `CMAKE_BUILD_TYPE`; a Debug-vs-Release
  mismatch once reported all 8 scenarios as "moved" — a false alarm that
  cost a cycle.
- Resonance moved DOWN to 0.90, not up. The inherited 0.95 was already
  unstable under a swept cutoff; 0.90 is a headroom limit, not a
  divergence limit — only 1.0 diverges. Not to be "restored" to 0.95.
- The tape grain-length clamp (`lenf <= _spawn_every * kGrains`) is not
  free: in a sparse pattern it trims a grain that would have sounded
  (45.6 min → 84 s). Grain-stealing is the alternative if a listen ever
  disagrees; it is a no-op in digital mode only because
  `kGrains == 2*kOverlap` today.
- MOTION/fog: `kScatterPosFrac` moved from 0.25 to 1.0. This is NOT one of
  the three options the 2026-07-18 spec had left open — it is a fourth,
  genuinely new lever. Earlier work had hunted fog in grain length and
  never questioned the position range.
- A claim that did not survive measurement: the old grain pool was NOT
  starving (1 dropped spawn in 5962). The density rise to `kOverlap=8`/
  `kGrains=16` rests on cost and headroom alone, not on a starvation fix.

**M5b (VCV host binding)** is next and had no plan yet as of this entry.
Three findings from M5a it must carry: the ~32 MB sampler buffers must be
heap-allocated (the VCV module holds echo buffers by value); `reinit()`
runs on every sample-rate change and would discard a loaded sample; and
`host/vcv/Makefile` was never build-tested (no Rack SDK at the time), only
diffed line-for-line against CMake. Also needs: ENG remap to
Synth↔Sampler, REC pad + LED via `gen_panel.py`, `dr_wav` vendored,
`dataToJson`/`dataFromJson` from scratch, factory sample + autoload.

## M5b — Sampler on the VCV panel (2026-07-21, branch `sampler-vcv`, not merged)

Plan `docs/superpowers/plans/2026-07-21-sampler-vcv-binding.md` (9 tasks,
subagent-driven, one implementer + one independent reviewer per task,
stop-on-failure). Commit range `35e7b2a`..`ee232f8` (14 commits) on top of
the M5a merge; `plugin.json` stays `2.7.0`, not tagged, not pushed, per the
plan's global constraints. Makes the M5a texture deck reachable and playable
from the plugin: ENG (per part, latched) now selects Synth ↔ Sampler; REC
(per part, latched) records from IN L/R into that part's buffer with an LED
that pulses while recording and shows fill level otherwise; a right-click
**Sampler A/B** submenu carries Load/Save/Clear, Speed mode (Digital/Tape),
Reverse, Overdub feedback, and the dev-only test-tone escape hatch; a factory
sample (Bastian's own recording, `res/factory.wav`) autoloads on the first
ENG flip into an empty part; and a recorded or loaded texture now survives
patch save/reopen via new `dataToJson`/`dataFromJson`. `host/render/`'s
`wav_reader.h`/`wav_writer.h` moved to `host/shared/` so both hosts share one
WAV implementation.

**Two deliberate deviations from the 2026-07-18 spec, decided in the plan:**
1. **Reuse the repo's own WAV reader/writer instead of vendoring `dr_wav`.**
   The spec (written before M5a) called for a vendored `dr_wav`; M5a had since
   shipped a hand-rolled, hardened `wav_reader.h`/`wav_writer.h` (every
   declared chunk size validated against remaining file bytes, covered by
   `tests/test_wav.cpp`, handling 16/24/32-bit PCM, float and
   `WAVE_FORMAT_EXTENSIBLE`) — vendoring a second implementation would add a
   dependency and a second code path for no gain.
2. **Sample-rate change: snapshot and restore, not `restore()`.** The plan
   originally assumed (per the spec's silence on the question) that a
   `SampleBuffer::restore()` could cheaply re-declare a buffer's frame count
   after a rate-driven re-init. **That premise is false and was caught by the
   implementer's own Task 1 test failing**, not by review: `init()` ends in
   `clear()` (`sample_buffer.cpp:33` → `179-180`), which `memset`s the whole
   injected buffer — there is nothing left to re-declare. `restore()` was
   dropped from the plan entirely; the host instead reads content out through
   the new `sample_data()`/`sampler_rec_size()` accessors before `reinit()`
   and pushes it back afterwards with the existing `load_sample()`. Accepted
   consequence: the snapshot itself is **not resampled** (tape behaviour,
   matches the instrument's idiom), which is the deliberate opposite of what
   a fresh file *load* does (always resamples — an import at the wrong pitch
   would be a bug).

**Findings a later session would not re-derive from the code:**
- `onAdd` fires **before** `onSampleRateChange` in Rack v2.6.6. Not provable
  from the SDK, which ships headers only — Task 7's reviewer confirmed it by
  reading Rack's actual `Engine.cpp` on GitHub. This is why `onAdd` must force
  its own `reinit()` rather than assume the sample-rate callback will run
  first: the brief's literal code would otherwise have written into null
  sampler buffers.
- `dataFromJson` is called on an **already-added** module for preset load
  (`ModuleWidget::loadAction`/`loadDialog`) and clipboard paste
  (`pasteJsonAction`) — neither goes through `Engine::addModule_NoLock`, so
  `onAdd` never fires again for those paths. The restore flag has to be
  consumed defensively; leaving it unconsumed on that path is a real
  (UX-only) quirk, not data loss, still open at the end of Task 9
  (`factoryTried` isn't reset on a live-module preset load, so an
  already-autoloaded part can stay silent on reload until `onReset`).
- `Slider::quantity` is annotated `/** Not owned. */` in the Rack SDK
  (`include/ui/Slider.hpp:12-14`) — a widget (`FeedbackSlider`) owning and
  destroying its own `Quantity` is therefore correct, not a double free; a
  reviewer's initial suspicion to the contrary was wrong and worth recording
  so it isn't relitigated.
- Persistence originally lost a recording made **over** a loaded file:
  `onSave` skipped any part with a non-empty `path` (on the theory it can
  reload from that file), but nothing cleared `path` when the user then
  REC'd over it — reopening silently discarded the live overdub and restored
  the original file instead. Overdub is a feature (default feedback 0.95),
  so this was a likely workflow, not an edge case. Fixed in `466c601` by
  clearing `path`/`factoryLoaded` the moment a recording actually starts
  (already documented in `Spotymod.cpp`'s REC-push comment).
- Task 3's brief predicted panel param ids 78/79 and `NUM_PARAMS` 82 for
  REC_A/REC_B; the real generator output was 76/77 and `NUM_PARAMS` 78 — an
  explorer's earlier wrong "80 params" count had been propagated without
  recounting. The implementer reported the mismatch instead of bending
  `gen_panel.py` to match a wrong prediction; `res/test_panel.py`'s
  `test_lower_half_positions` fixture needed updating for the same reason
  (the REC pad's insertion re-spaced ENGINE_A/GRITMODE_A/STEPS_A on the PLAY
  row).
- The REC LED's fill-level branch originally didn't check the part's engine
  at all: record on a Sampler part, flip ENG to Synth, and the LED stayed lit
  at the fill level on what the user now reads as a plain synth voice —
  contradicting the plan's own prose ("REC on a synth part is inert with a
  dark LED"). The record *button* was correctly gated; only the *light*
  wasn't. Fixed in `4948ec0` by gating the fill branch on the same `eng2 &&
  !testTone` condition `pushParams` already uses.
- The factory-sample first-use path originally read and decoded
  `res/factory.wav` from disk on the audio thread, on the first ENG flip —
  accepted as "low tens of ms, once" in the plan, then reviewed as a real
  ship risk (a cold-file or AV-scanner read is not bounded, and a click on
  the very first "it just works" gesture is the worst place to spend one).
  Fixed in `ee232f8`: disk read + decode moved to `onAdd()` (main thread,
  before `process()` ever runs for the instance), leaving only the
  already-decoded buffer's resample-to-current-rate in the reactive path.

**Task 9 close-out (this entry):** 484/484 doctest cases green (481 M5a
baseline + Task 1's 3 new accessor-coverage cases). Synth-neutrality gate:
the 8 pinned non-sampler scenarios (`ambient_wash`, `assets_percussive_source`,
`chord_bloom`, `ctrl_identity`, `demo_density_sweep`, `demo_step_melody`,
`reverb_delay`, `reverb_wash`) rendered byte-identical between `HEAD`
(`ee232f8`) and a `git worktree` pinned at the pre-M5b commit `35e7b2a`, both
configured `CMAKE_BUILD_TYPE=Debug` to match the existing `build/` tree
(see the CMAKE_BUILD_TYPE warning above — a Debug/Release mismatch is a false
alarm, not a real difference); `ctrl_identity.wav`'s SHA-256 also matches the
repo's pinned `ctrl_identity.sha256` independently. Determinism gate:
`sampler_texture_deck` and `sampler_extremes` each double-rendered
byte-identical. Structural corroboration: `git diff 35e7b2a..ee232f8 --
engine/` touches only `engine/instrument.h` and
`engine/sampler/sampler_engine.h` (Task 1's two read-only accessors) — no
other engine file moved. Cold plugin build (`rm -rf host/vcv/build
host/vcv/dist`, WinLibs MinGW-w64 g++ via an MSYS2 `make`, Rack SDK 2.6.6)
succeeded for both the default and `dist` targets; the produced
`Spotymod-2.7.0-win-x64.vcvplugin` contains `res/factory.wav`, and
`plugin.json` still reads `2.7.0` and is untouched in the working tree.

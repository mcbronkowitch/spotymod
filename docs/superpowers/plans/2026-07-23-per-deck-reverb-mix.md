# Per-Deck Reverb Mix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split the single shared reverb MIX into one per deck — each deck gets its own dry/wet, 100 % = that deck fully wet — while the reverb stays one shared room.

**Architecture:** One shared `AmbientReverb` (Oliverb) stays. The per-deck mix becomes an aux-send model at the master mix point in `Instrument::process`: each deck's dry rides `cos(mix)`, its send into the shared room rides `sin(mix)`, and the single wet return joins at unity (the reverb's internal `kWetGain` is unchanged). Below self-oscillation the reverb is linear, so both decks at the old default reproduce today's sound. The central `REV_MIX` knob is removed; each deck's FX box gains a `ROOM` knob.

**Tech Stack:** C++17 engine (`engine/`), doctest (`tests/`), Python panel generator (`host/vcv/res/gen_panel.py` → `generated_panel.hpp` + `Spotymod.svg`), VCV Rack plugin (`host/vcv/src/Spotymod.cpp`).

## Global Constraints

- **Bit-exactness is NOT required** — renders/tests are sanity checks, not byte-identity gates. Use tolerances / energy checks, not checksums.
- **`PART_STRIDE` must stay 23.** New params are appended at the END of `PARAMS`, never added to `part_controls()` (that would grow the stride and shift every part-B/SHARED id).
- **`kWetGain` (0.40) stays unchanged**, inside `AmbientReverb::process`.
- **Per-deck default mix = 0.410** (both decks), matching the old shared default — reproduces today's sound below the bloom regime.
- **New knob label is `ROOM`** (not "MIX": FLUX beside it is already the delay mix).
- **Build the VCV plugin only via `./build-local.sh`** from `host/vcv/` — never a hand-rolled g++ (system g++ is the ARM cross-compiler and fails with "MinGW not found").
- **Commit trailer:** end every commit message with
  `Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>`
- All paths below are relative to the fork root `c:/Users/bernd/Documents/AI/Spotykach`.

---

## File Structure

- `engine/instrument.h` — mix state becomes per-deck arrays; two `set_reverb_mix` overloads.
- `engine/instrument.cpp` — per-deck gains in `process`, per-deck-OR sleep gate, per-deck + convenience setters, `init`.
- `engine/fx/reverb.h/.cpp` — **no change** (the room already takes one stereo send, returns one wet-only stereo with `kWetGain`).
- `tests/test_instrument.cpp` — one new per-deck independence test; existing global-mix tests stay green via the convenience overload.
- `host/vcv/res/gen_panel.py` — remove central `REV_MIX`, widen `FX_TOP` to 4 columns, append `REV_MIX_A/B` labelled `ROOM`.
- `host/vcv/src/generated_panel.hpp`, `host/vcv/res/Spotymod.svg` — regenerated (never hand-edited).
- `host/vcv/res/test_panel.py` — update the frozen param order, the FX/centre coordinate maps, add an appended-param guard.
- `host/vcv/src/Spotymod.cpp` — `defaultFor` cases; two per-deck `set_reverb_mix` calls.
- `docs/roadmap.md`, `docs/milestone-history.md` — describe the per-deck split.

---

## Task 1: Engine — per-deck reverb mix

**Files:**
- Modify: `engine/instrument.h` (member state ~207-211; setter decl line 78)
- Modify: `engine/instrument.cpp` (`init` ~30-34; `set_reverb_mix` 41-50; `process` 116-152)
- Test: `tests/test_instrument.cpp` (new TEST_CASE)

**Interfaces:**
- Consumes: `AmbientReverb::process(float,float,float&,float&)`, `AmbientReverb::clear()`, `OnePole::init/reset/process`, `Center::gain_a()/gain_b()`, `clampf`, `std::cos/std::sin`, `kHalfPi`, `kMixSmoothS`, `kDefaultReverbMix` (all already present).
- Produces:
  - `void Instrument::set_reverb_mix(int part, float n)` — per-deck (0..1 clamped, exact endpoints, wakes room if target > 0).
  - `void Instrument::set_reverb_mix(float n)` — convenience overload, sets both decks.
  - Members `_rev_dry_target[PART_COUNT]`, `_rev_wet_target[PART_COUNT]`, `_rev_dry[PART_COUNT]`, `_rev_wet[PART_COUNT]`.
  - `reverb_asleep()` unchanged (true only when BOTH decks are dry and settled).

- [ ] **Step 1: Write the failing test**

Add this TEST_CASE to `tests/test_instrument.cpp` (append after the equal-power case that ends near line 512). It relies on the existing `test_fx_mem()` helper and `FXT_REV_SEND`:

```cpp
TEST_CASE("instrument: reverb mix is per-deck (A wet-kills-dry while B stays dry)") {
    // Sends muted -> the shared room is silent, so we observe the DRY path only.
    // With A mix 1 (dry gain 0) and B mix 0 (dry gain 1) held SIMULTANEOUSLY:
    //   morph 0 (only A audible)  -> A's dry is killed  -> exact silence
    //   morph 1 (only B audible)  -> B's dry survives   -> non-zero energy
    // No single shared mix value could satisfy both at once (1 kills both, 0
    // keeps both), so this fails on the old shared-mix engine and passes on the
    // per-deck one.
    auto dry_energy = [](float morph) {
        Instrument fx;
        fx.init(48000.f, test_fx_mem());
        for (int p = 0; p < PART_COUNT; ++p)
            fx.set_fx_target_base(p, FXT_REV_SEND, 0.f);   // room stays silent
        fx.set_morph(morph);
        fx.set_reverb_mix(PART_A, 1.f);   // A fully wet -> A dry gone
        fx.set_reverb_mix(PART_B, 0.f);   // B fully dry
        double acc = 0.0;
        float l = 0.f, r = 0.f;
        for (int i = 0; i < 48000; ++i) {
            fx.process(nullptr, nullptr, &l, &r, 1);
            acc += (double)l * l;
        }
        return acc;
    };
    CHECK(dry_energy(0.f) == 0.0);   // morph 0: A's dry killed by an exact-0 gain
    CHECK(dry_energy(1.f) > 0.0);    // morph 1: B's dry survives untouched
}
```

- [ ] **Step 2: Run the test to verify it fails (compile error)**

Run (desktop test build — source the env first, per the desktop build-env note; the `build/` dir is already configured):

```bash
cd c:/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build --target spky_tests 2>&1 | tail -30
```

Expected: **compile error** — `Instrument` has no member `set_reverb_mix(int, float)` (only `set_reverb_mix(float)` exists), and/or no matching call. It must fail to compile here.

- [ ] **Step 3a: Make the mix state per-deck in `engine/instrument.h`**

Replace the setter declaration (line 78):

```cpp
    void set_reverb_mix(float n);   // 0..1 equal-power dry/wet at the master join
```

with:

```cpp
    // Per-deck equal-power reverb mix (spec 2026-07-23 per-deck-reverb-mix):
    // one shared room, each deck's dry rides cos, its SEND rides sin.
    void set_reverb_mix(int part, float n);   // 0..1, exact endpoints
    void set_reverb_mix(float n);             // convenience: both decks
```

Replace the member block (lines 207-211):

```cpp
    float   _rev_dry_target = 1.f;  // equal-power gain targets (exact endpoints)
    float   _rev_wet_target = 0.f;
    OnePole _rev_dry, _rev_wet;     // 10 ms glide at the master join
    bool    _rev_primed = false;    // first process() snaps the mix gains
    bool    _rev_asleep = false;    // MIX 0 gate: room cleared, process() skipped
```

with:

```cpp
    // Per-deck equal-power mix (spec 2026-07-23): dry rides cos, the wet SEND
    // rides sin, one shared room. Indexed by PART_A / PART_B.
    float   _rev_dry_target[PART_COUNT] = { 1.f, 1.f };  // exact endpoints
    float   _rev_wet_target[PART_COUNT] = { 0.f, 0.f };
    OnePole _rev_dry[PART_COUNT], _rev_wet[PART_COUNT];  // 10 ms glide per deck
    bool    _rev_primed = false;    // first process() snaps the mix gains
    bool    _rev_asleep = false;    // both decks dry: room cleared, process() skipped
```

- [ ] **Step 3b: Update `init` in `engine/instrument.cpp`**

Replace the smoother-init + default block (lines 30-34):

```cpp
    _rev_dry.init(sample_rate, kMixSmoothS);
    _rev_wet.init(sample_rate, kMixSmoothS);
    _rev_primed = false;
    _rev_asleep = false;
    set_reverb_mix(kDefaultReverbMix);
```

with:

```cpp
    for (int p = 0; p < PART_COUNT; ++p) {
        _rev_dry[p].init(sample_rate, kMixSmoothS);
        _rev_wet[p].init(sample_rate, kMixSmoothS);
    }
    _rev_primed = false;
    _rev_asleep = false;
    set_reverb_mix(kDefaultReverbMix);   // convenience overload -> both decks
```

- [ ] **Step 3c: Rewrite `set_reverb_mix` in `engine/instrument.cpp`**

Replace the whole setter (lines 41-50):

```cpp
void Instrument::set_reverb_mix(float n) {
    n = clampf(n, 0.f, 1.f);
    if (n <= 0.f)      { _rev_dry_target = 1.f; _rev_wet_target = 0.f; }
    else if (n >= 1.f) { _rev_dry_target = 0.f; _rev_wet_target = 1.f; }
    else {
        _rev_dry_target = std::cos(n * kHalfPi);   // equal-power crossfade
        _rev_wet_target = std::sin(n * kHalfPi);
    }
    if (_rev_wet_target > 0.f) _rev_asleep = false;   // wake into the cleared room
}
```

with:

```cpp
void Instrument::set_reverb_mix(int part, float n) {
    n = clampf(n, 0.f, 1.f);
    if (n <= 0.f)      { _rev_dry_target[part] = 1.f; _rev_wet_target[part] = 0.f; }
    else if (n >= 1.f) { _rev_dry_target[part] = 0.f; _rev_wet_target[part] = 1.f; }
    else {
        _rev_dry_target[part] = std::cos(n * kHalfPi);   // equal-power crossfade
        _rev_wet_target[part] = std::sin(n * kHalfPi);   // rides the SEND, not the return
    }
    if (_rev_wet_target[part] > 0.f) _rev_asleep = false;   // wake into the cleared room
}

void Instrument::set_reverb_mix(float n) {   // convenience: both decks together
    set_reverb_mix(PART_A, n);
    set_reverb_mix(PART_B, n);
}
```

- [ ] **Step 3d: Rewrite the master mix point in `engine/instrument.cpp::process`**

Replace lines 121-148 (from `const float ga = _center.gain_a();` through the closing `}` of the `if (_reverb)` block, i.e. just before `_limiter.process(l, r);`):

```cpp
        const float ga = _center.gain_a();
        const float gb = _center.gain_b();
        float l = al * ga + bl * gb;          // MORPH: equal-power A<->B blend
        float r = ar * ga + br * gb;
        if (_reverb) {
            if (!_rev_primed) {              // snap a mix set before the first block
                _rev_dry.reset(_rev_dry_target);
                _rev_wet.reset(_rev_wet_target);
                if (_rev_wet_target == 0.f) { _reverb->clear(); _rev_asleep = true; }
                _rev_primed = true;
            }
            const float dg = _rev_dry.process(_rev_dry_target);
            const float wg = _rev_wet.process(_rev_wet_target);
            if (!_rev_asleep) {
                // MORPH fades dry AND send together (M4 supersedes the M1.6
                // pre-morph-send rule): a fully morphed-away part injects no new
                // reverb; only its already-committed tail rings out.
                float wl, wr;
                _reverb->process(asl * ga + bsl * gb, asr * ga + bsr * gb, wl, wr);
                l = l * dg + wl * wg;
                r = r * dg + wr * wg;
                if (wg == 0.f && dg == 1.f && _rev_wet_target == 0.f) {
                    _reverb->clear();        // clear-on-sleep: waking starts empty
                    _rev_asleep = true;      // Oliverb CPU is off until MIX reopens
                }
            }
            // asleep: dry passes bit-exact (dg has snapped to 1), sends discarded
        }
```

with:

```cpp
        const float ga = _center.gain_a();
        const float gb = _center.gain_b();
        float l = al * ga + bl * gb;          // MORPH blend (null-reverb path keeps this)
        float r = ar * ga + br * gb;
        if (_reverb) {
            if (!_rev_primed) {              // snap the mix set before the first block
                for (int p = 0; p < PART_COUNT; ++p) {
                    _rev_dry[p].reset(_rev_dry_target[p]);
                    _rev_wet[p].reset(_rev_wet_target[p]);
                }
                if (_rev_wet_target[PART_A] == 0.f && _rev_wet_target[PART_B] == 0.f) {
                    _reverb->clear(); _rev_asleep = true;
                }
                _rev_primed = true;
            }
            const float dga = _rev_dry[PART_A].process(_rev_dry_target[PART_A]);
            const float dgb = _rev_dry[PART_B].process(_rev_dry_target[PART_B]);
            const float wga = _rev_wet[PART_A].process(_rev_wet_target[PART_A]);
            const float wgb = _rev_wet[PART_B].process(_rev_wet_target[PART_B]);
            // Per-deck dry: each deck's dry gain rides its own cos before the
            // MORPH sum, so one deck can be wet-only while the other stays dry.
            l = al * ga * dga + bl * gb * dgb;
            r = ar * ga * dga + br * gb * dgb;
            if (!_rev_asleep) {
                // Per-deck send: the equal-power wet curve (sin) rides the SEND
                // -- one shared room has only one return. MORPH fades the send
                // too (M4 rule): a morphed-away deck injects no new reverb.
                float wl, wr;
                _reverb->process(asl * ga * wga + bsl * gb * wgb,
                                 asr * ga * wga + bsr * gb * wgb, wl, wr);
                l += wl;   // wl already carries kWetGain; the return joins at unity
                r += wr;
                if (wga == 0.f && wgb == 0.f &&
                    _rev_wet_target[PART_A] == 0.f && _rev_wet_target[PART_B] == 0.f) {
                    _reverb->clear();        // clear-on-sleep: waking starts empty
                    _rev_asleep = true;      // Oliverb CPU is off until a MIX reopens
                }
            }
            // asleep: dga/dgb have snapped to 1 (both decks mix 0), so l/r stay full dry
        }
```

- [ ] **Step 4: Run the new test and the full instrument suite**

```bash
cd c:/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build --target spky_tests 2>&1 | tail -20 && ./build/spky_tests.exe -tc="instrument*" 2>&1 | tail -30
```

Expected: builds clean; the new case passes; every existing `instrument M4.8:` reverb case and `instrument: all FX off + send 0 …` / `boot reverb send is audible` case stays green (they drive `set_reverb_mix(float)`, now the convenience overload).

- [ ] **Step 5: Commit**

```bash
git -C c:/Users/bernd/Documents/AI/Spotykach add engine/instrument.h engine/instrument.cpp tests/test_instrument.cpp
git -C c:/Users/bernd/Documents/AI/Spotykach commit -m "$(cat <<'EOF'
feat(engine): per-deck reverb mix on the shared room

Each deck gets its own dry/wet: dry rides cos(mix), the send into the
one shared Oliverb rides sin(mix), the wet return joins at unity. Both
decks at the old 0.410 default reproduce today's sound below the bloom
regime (the reverb is linear there). Adds set_reverb_mix(int,float);
the old set_reverb_mix(float) stays as a both-decks convenience overload.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

## Task 2: Panel generator — remove central MIX, add per-deck ROOM knobs

**Files:**
- Modify: `host/vcv/res/gen_panel.py` (`FX_TOP` line 164; remove `REV_MIX` line 318; append `REV_MIX_A/B` after line 376)
- Regenerate: `host/vcv/src/generated_panel.hpp`, `host/vcv/res/Spotymod.svg`
- Modify + run: `host/vcv/res/test_panel.py`

**Interfaces:**
- Consumes: `FX_TOP`, `FX_BOT`, `ROW_V1`, `W`, `SMKNOB`, `Ctl` (all in `gen_panel.py`).
- Produces: enum ids `REV_MIX_A`, `REV_MIX_B` at the END of `ParamId` (trailing, after `REC_B`); `REV_MIX` removed; `PART_STRIDE` unchanged (23).

- [ ] **Step 1: Update the panel test's frozen expectations first (TDD)**

In `host/vcv/res/test_panel.py`:

Change `PARAM_ORDER` — remove `'REV_MIX'` from the SHARED block and append the two new ids at the end. The SHARED lines (47-48) currently read:

```python
    'MASTER_DRIVE', 'SETTLE', 'REV_SIZE', 'REV_DECAY', 'REV_MIX', 'REV_TONE',
    'REV_DIFF', 'REV_SMEAR', 'REV_MOD', 'CHOKE', 'FILT_A', 'FILT_B', 'TIDE',
```

Replace the first of those two lines (drop `'REV_MIX', `):

```python
    'MASTER_DRIVE', 'SETTLE', 'REV_SIZE', 'REV_DECAY', 'REV_TONE',
    'REV_DIFF', 'REV_SMEAR', 'REV_MOD', 'CHOKE', 'FILT_A', 'FILT_B', 'TIDE',
```

Change the final `PARAM_ORDER` line (51) from:

```python
    'REC_A', 'REC_B',
```

to:

```python
    'REC_A', 'REC_B', 'REV_MIX_A', 'REV_MIX_B',
```

Update `LOWER_A` — the FX top row re-spaces to 4 columns. Replace the FX-top line (242):

```python
    'FLUXRATE_A': (49.50, 77.30), 'FLUX_A': (62.75, 77.30), 'FLUXFB_A': (76.00, 77.30),
```

with (FLUX and FFB move in, ROOM added at the 4th column):

```python
    'FLUXRATE_A': (49.50, 77.30), 'FLUX_A': (58.333, 77.30), 'FLUXFB_A': (67.167, 77.30),
    'REV_MIX_A': (76.00, 77.30),
```

Update `CENTER` — remove `REV_MIX` from the ROOM row. Replace line 305:

```python
    'REV_SIZE': (-11.5, 82.5), 'REV_MIX': (0.0, 82.5), 'REV_DECAY': (11.5, 82.5),
```

with:

```python
    'REV_SIZE': (-11.5, 82.5), 'REV_DECAY': (11.5, 82.5),
```

Add a new guard (place it next to `test_rec_params`, e.g. after line 109):

```python
def test_reverb_mix_params():
    """REV_MIX_A/B are appended (not templated) so PART_STRIDE stays 23, and
    they carry the 'ROOM' label as the FX top row's 4th slot -- the shared
    centre REV_MIX is gone."""
    check(g.PART_STRIDE == 23, "PART_STRIDE must stay 23")
    ids = {c.enum: i for i, c in enumerate(g.PARAMS)}
    check('REV_MIX' not in ids, "the shared centre REV_MIX must be removed")
    for e in ("REV_MIX_A", "REV_MIX_B"):
        check(e in ids, f"{e} missing")
        check(ids[e] >= 2 * g.PART_STRIDE, f"{e} must be appended, not templated")
        check(ctl(e).label == "ROOM", f"{e} label must be 'ROOM'")
    h = g.header()
    for e in ("REV_MIX_A", "REV_MIX_B"):
        check(h.count(f"{{{e}, WK_SMKNOB,") == 1,
              f"{e} is not WK_SMKNOB in the generated header")
```

- [ ] **Step 2: Run the panel test to verify it fails**

```bash
cd c:/Users/bernd/Documents/AI/Spotykach/host/vcv && python res/test_panel.py
```

Expected: **FAIL** — the generator still has the old `REV_MIX` (PARAMS order mismatch, `REV_MIX_A/B` missing, `test_reverb_mix_params` failing).

- [ ] **Step 3a: Widen `FX_TOP` in `host/vcv/res/gen_panel.py`**

Replace line 164:

```python
FX_TOP   = [49.5, 62.75, 76.0]      # FRATE FLUX FFB -- the delay cluster
```

with:

```python
# 4-wide, aligned to FX_BOT so the FX box's two rows fluch: FRATE FLUX FFB ROOM.
FX_TOP   = [49.5, 58.333, 67.167, 76.0]   # FRATE FLUX FFB | ROOM (per-deck reverb mix)
```

- [ ] **Step 3b: Remove the central `REV_MIX` from `SHARED`**

Delete line 318 entirely:

```python
    Ctl("REV_MIX",   SMKNOB, CX, ROW_ROOM1, "MIX"),
```

(The ROOM box keeps its geometry; SIZE stays at `L`/row1, DECAY at `R`/row1, the centre slot is simply empty now.)

- [ ] **Step 3c: Append the per-deck `REV_MIX_A/B`**

In the `PARAMS = PART_A + PART_B + SHARED + [ ... ]` list, immediately after the `REC_A` / `REC_B` entries (lines 375-376) and before the closing `]`, add:

```python
    # Per-deck reverb mix (spec 2026-07-23 per-deck-reverb-mix). Appended LAST
    # like FILT/FLUXRATE/COLOR/DUST/REC so PART_STRIDE stays 23 and no id before
    # them moves. They fill the FX top row's 4th slot -- FRATE.FLUX.FFB.ROOM --
    # aligned to the FX bottom row. Label "ROOM" (not "MIX": FLUX beside it is
    # already the delay mix). The old shared centre REV_MIX is removed from
    # SHARED; its id and every id after it shift by one (accepted: old .vcv
    # patches load shifted reverb/CHOKE/tail params).
    Ctl("REV_MIX_A", SMKNOB, FX_TOP[3],     ROW_V1, "ROOM"),
    Ctl("REV_MIX_B", SMKNOB, W - FX_TOP[3], ROW_V1, "ROOM"),
```

- [ ] **Step 3d: Regenerate the header and SVG**

```bash
cd c:/Users/bernd/Documents/AI/Spotykach/host/vcv && python res/gen_panel.py
```

Expected output line: `params=... (stride=23) inputs=4 outputs=6 lights=4  panel=42HP` — **stride must be 23**. `src/generated_panel.hpp` and `res/Spotymod.svg` change: the header gains `REV_MIX_A`/`REV_MIX_B` in `ParamId` (after `REC_B`), loses `REV_MIX`, and the two new control-table rows read `{REV_MIX_A, WK_SMKNOB, {76.000f, 77.300f}, "ROOM", ...}` and its mirror.

- [ ] **Step 4: Run the panel test to verify it passes**

```bash
cd c:/Users/bernd/Documents/AI/Spotykach/host/vcv && python res/test_panel.py
```

Expected: `PASS -- panel guards ok`. In particular `test_no_overlap` (ROOM at 76.0 clears FFB at 67.167 by 8.83 mm), `test_enum_order` (stride 23), `test_lower_half_positions`, `test_center_positions`, and the new `test_reverb_mix_params` all pass.

- [ ] **Step 5: Commit**

```bash
git -C c:/Users/bernd/Documents/AI/Spotykach add host/vcv/res/gen_panel.py host/vcv/src/generated_panel.hpp host/vcv/res/Spotymod.svg host/vcv/res/test_panel.py
git -C c:/Users/bernd/Documents/AI/Spotykach commit -m "$(cat <<'EOF'
feat(vcv-panel): per-deck ROOM knob, drop the shared REV_MIX

Widen FX_TOP to 4 columns (FRATE.FLUX.FFB.ROOM), append REV_MIX_A/B
(label ROOM) at the end of PARAMS so PART_STRIDE stays 23, and remove
the central REV_MIX from the ROOM box. Panel guards updated; ids after
the old REV_MIX shift by one.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

## Task 3: VCV host wiring + plugin build

**Files:**
- Modify: `host/vcv/src/Spotymod.cpp` (`defaultFor` line 239; reverb forwarding line 547)
- Build: `host/vcv/` via `./build-local.sh`

**Interfaces:**
- Consumes: `Instrument::set_reverb_mix(int, float)` (Task 1); generated enums `REV_MIX_A`, `REV_MIX_B` (Task 2).
- Produces: two Rack params forwarded per deck; `defaultFor` returns 0.410 for both.

Note on the id shift: Initialize/default values come from `defaultFor(int id)` in code, keyed by symbolic enum name — so renaming the case is all that's needed; there is **no runtime `.vcvm` snapshot** to regenerate. The only consequence of the shift is that pre-existing user-saved `.vcv` patches load shifted reverb/CHOKE/tail params, which the spec accepts.

- [ ] **Step 1: Update `defaultFor`**

In `host/vcv/src/Spotymod.cpp`, replace line 239:

```cpp
            case REV_MIX:      return 0.410f;  // behind the parts — the chord already fills the width
```

with:

```cpp
            case REV_MIX_A:
            case REV_MIX_B:    return 0.410f;  // per-deck room send; was the shared REV_MIX
```

- [ ] **Step 2: Forward the two params per deck**

Replace line 547:

```cpp
        inst.set_reverb_mix(params[REV_MIX].getValue());
```

with:

```cpp
        inst.set_reverb_mix(spky::PART_A, params[REV_MIX_A].getValue());
        inst.set_reverb_mix(spky::PART_B, params[REV_MIX_B].getValue());
```

- [ ] **Step 3: Confirm no other `REV_MIX` reference remains**

```bash
cd c:/Users/bernd/Documents/AI/Spotykach && grep -rn "REV_MIX\b" host/vcv/src/Spotymod.cpp
```

Expected: **no output** (only `REV_MIX_A` / `REV_MIX_B` remain, which `\b` after `REV_MIX` will not match). If a bare `REV_MIX` is printed, fix that reference.

- [ ] **Step 4: Build the plugin**

```bash
cd c:/Users/bernd/Documents/AI/Spotykach/host/vcv && ./build-local.sh 2>&1 | tail -20
```

Expected: clean build, plugin installed into Rack's plugin dir (as `build-local.sh` does). Do NOT hand-roll g++.

- [ ] **Step 5: Commit**

```bash
git -C c:/Users/bernd/Documents/AI/Spotykach add host/vcv/src/Spotymod.cpp
git -C c:/Users/bernd/Documents/AI/Spotykach commit -m "$(cat <<'EOF'
feat(vcv): forward per-deck ROOM knobs to set_reverb_mix

REV_MIX_A/B default 0.410 and drive set_reverb_mix(PART_A/B, .). The
shared REV_MIX forwarding is gone. Initialize defaults come from
defaultFor() in code (no .vcvm to regenerate).

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

- [ ] **Step 6: Manual Rack smoke test (user-run)**

In VCV Rack, add/reload Spotymod and verify:
- Each deck's FX box shows a `ROOM` knob in the top row (FRATE·FLUX·FFB·ROOM), rows aligned; the centre ROOM box no longer has a MIX knob.
- Deck A `ROOM` at 100 %, deck B at 0 % → A is fully wet (its dry gone), B fully dry; swap → mirrored.
- Both decks at the 0.410 default sound like the pre-change build (no audible change below bloom).

This step is a human check; note the result in the task review.

---

## Task 4: Docs — roadmap & milestone history

**Files:**
- Modify: `docs/roadmap.md` (the reverb `set_reverb_mix` / VCV `REV_MIX` bullets, ~lines 278, 290-291)
- Modify: `docs/milestone-history.md`

**Interfaces:** none (documentation only).

- [ ] **Step 1: Update the roadmap reverb bullets**

Read `docs/roadmap.md` lines ~275-295, then update the two mix bullets to describe the per-deck split. Replace the `set_reverb_mix` description bullet so it reads (adapt to the surrounding prose):

```markdown
- `set_reverb_mix(int part, float)` (0..1): per-deck equal-power dry/wet on
  one shared room — the deck's dry rides cos, its send into the room rides sin,
  the wet return joins at unity. `set_reverb_mix(float)` sets both decks. 100 %
  = that deck fully wet.
```

And the VCV host bullet so it reads:

```markdown
- Hosts: VCV per-deck `ROOM` knob in each FX box (default 0.410), render
  action `set_reverb_mix` (global, both decks).
```

- [ ] **Step 2: Add a milestone-history entry**

Append a short entry to `docs/milestone-history.md` in the existing style (adapt heading/number to the file's convention):

```markdown
### Per-deck reverb mix (2026-07-23)

The shared reverb MIX becomes one knob per deck (`ROOM`, in each FX box) on a
single shared Oliverb: dry rides cos(mix), the send into the room rides
sin(mix), the wet return joins at unity. Both decks at the old 0.410 default
reproduce the previous sound below the bloom regime (the reverb is linear
there); only self-oscillating DECAY differs slightly. Central `REV_MIX`
removed — ids after it shift; `PART_STRIDE` stays 23. No CPU cost: the one
Oliverb still runs once per sample.
```

- [ ] **Step 3: Commit**

```bash
git -C c:/Users/bernd/Documents/AI/Spotykach add docs/roadmap.md docs/milestone-history.md
git -C c:/Users/bernd/Documents/AI/Spotykach commit -m "$(cat <<'EOF'
docs: per-deck reverb mix in roadmap + milestone history

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

## Final verification

- [ ] **Engine suite green:** `source env.sh && cmake --build build --target spky_tests && ./build/spky_tests.exe` — full suite passes (new per-deck case + all existing reverb cases).
- [ ] **Panel guards green:** `cd host/vcv && python res/test_panel.py` → `PASS`, stride 23.
- [ ] **Plugin builds:** `cd host/vcv && ./build-local.sh` clean.
- [ ] **Manual Rack smoke test** (Task 3 Step 6) signed off: A-wet/B-dry works, defaults sound unchanged.
- [ ] All four commits present; no bare `REV_MIX` left in engine or host.

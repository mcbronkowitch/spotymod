# Spotykach M4.9 — Reverb DIFFUSION Knob Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the reverb's DEPTH parameter with DIFFUSION (AP coefficient 0–0.90, weakly coupled line modulation) so the room can melt attacks into a full wash.

**Architecture:** One facade method swap behind the stable reverb API: `AmbientReverb::set_depth` → `set_diffusion`, which drives BOTH the Oliverb AP coefficient (`0.90·n`) and the line-mod amount (`(0.05+0.20·n)·450` samples). Same parameter slot in engine API, render host, and VCV panel. No core (`third_party/oliverb/`) changes.

**Tech Stack:** Portable C++17 engine (no heap, deterministic), doctest, CMake+clang+ninja desktop build, VCV Rack plugin host, Python panel generator.

**Spec:** `docs/superpowers/specs/2026-07-14-spotykach-reverb-diffusion-design.md` (residency repo)

## Global Constraints

- **Repo:** `c:\Users\bernd\Documents\AI\Spotykach`. Branch from `melody-engine-rework` (HEAD `b42327a`): `git checkout -b reverb-diffusion`.
- **Engine build/test** (Git Bash): `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build && ctest --test-dir build --output-on-failure`. Doctest filter: `./build/spky_tests.exe -tc="<filter>*"`.
- **Exact mapping (spec values, verbatim):** AP coefficient = `0.90f * norm`; mod amount = `(0.05f + 0.20f * norm) * 450.f` samples; **boot default 0.7** everywhere (engine, VCV `defaultFor`).
- **`Part::set_depth` / `Instrument::set_depth(int p, float n)` / scenario action `"set_depth"` are the UNRELATED per-part lane-depth macro — do NOT touch them.** Only the reverb facade `set_depth` and `set_reverb_depth` are removed.
- **Deletion is ersatzlos** (like M4.5's `set_shimmer`): no deprecation shims, no alias actions.
- No heap in the engine; `AmbientReverb` never stack-allocated (tests use statics).
- Determinism is an invariant: identical call sequence → bit-identical output.
- **Commit trailer** (every commit): `Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>` — never the default Claude/Anthropic trailer.

**Testing note (decided in planning):** the spec's "coupling" test idea (verify one call drives both coefficient and mod) has no clean behavioral observer — the vendored core exposes no getters and adding them for a test is not worth it. Coverage decision: the sparse-vs-dense difference test exercises both effects at once (coefficient dominates, mod contributes), and the 2-line coupling body is review-verified. No dedicated coupling test.

---

### Task 1: Facade + engine API swap (`set_diffusion`), test migration

**Files:**
- Modify: `engine/fx/reverb.h` (setter decl + class comment)
- Modify: `engine/fx/reverb.cpp` (setter body, boot defaults, `kDiffusion` const removed)
- Modify: `engine/instrument.h:66` (`set_reverb_depth` → `set_reverb_diffusion`)
- Test: `tests/test_reverb.cpp` (2 cases migrated by deletion, 1 replaced, 1 new, determinism call swapped), `tests/test_instrument.cpp:121` (one call swapped)

**Interfaces:**
- Consumes: `clouds::Oliverb::set_diffusion(float)` / `set_mod_amount(float)` (existing core setters), `clampf`.
- Produces: `void AmbientReverb::set_diffusion(float norm)` and `void Instrument::set_reverb_diffusion(float n)` — Tasks 2 and 3 forward to the latter. `set_depth`/`set_reverb_depth` cease to exist.

- [ ] **Step 1: Create the branch**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
git checkout -b reverb-diffusion
```

Expected: clean tree on `reverb-diffusion` at `b42327a`.

- [ ] **Step 2: Write the failing tests**

In `tests/test_reverb.cpp`:

**(a)** Delete the line `s_rev.set_depth(0.f);` in `"reverb: below 100% the impulse energy decays monotonically"` (line 51) and BOTH `s_rev.set_depth(0.f);` lines in `"reverb: tone closed removes high-frequency tail energy"` (lines 120, 125). (The boot diffusion default now carries a small fixed line-mod floor; these energy/ratio checks are robust to it.)

**(b)** Replace the entire `TEST_CASE("reverb: depth animates the tail")` (lines 131–144) with:

```cpp
TEST_CASE("reverb: diffusion reshapes the room (sparse vs dense)") {
    s_rev.init(48000.f);
    s_rev.set_decay(0.8f);
    s_rev.set_diffusion(0.f);            // discrete slap-echo cluster
    auto sparse = impulse_response(s_rev, 48000, true);
    s_rev.init(48000.f);
    s_rev.set_decay(0.8f);
    s_rev.set_diffusion(0.9f);           // dense wash
    auto dense = impulse_response(s_rev, 48000, true);
    int diff = 0;
    for (int i = 4800; i < 48000; ++i)
        if (std::fabs(sparse[i] - dense[i]) > 1e-6f) ++diff;
    CHECK(diff > 1000);
    // early-window crest: a sparse room concentrates energy in discrete
    // events; a diffused one spreads it -> lower peak-to-RMS
    auto crest = [](const std::vector<float>& x, int a, int b) {
        float pk = 0.f;
        double acc = 0.0;
        for (int i = a; i < b; ++i) {
            pk = std::max(pk, std::fabs(x[i]));
            acc += x[i] * x[i];
        }
        float rms = std::sqrt((float)(acc / (b - a))) + 1e-12f;
        return pk / rms;
    };
    CHECK(crest(sparse, 0, 9600) > crest(dense, 0, 9600));
}
```

**(c)** Append a sweep-safety case (mirrors the existing size-ride test's shape):

```cpp
TEST_CASE("reverb: diffusion ride stays bounded without clicks") {
    s_rev.init(48000.f);
    s_rev.set_decay(0.9f);
    // ring the room first
    for (int i = 0; i < 24000; ++i) {
        float in = (i == 0) ? 1.f : 0.2f * std::sin(6.2831853f * 330.f * i / 48000.f);
        float wl, wr;
        s_rev.process(in, in, wl, wr);
    }
    float prev = 0.f, max_step = 0.f;
    bool finite = true;
    for (int i = 0; i < 96000; ++i) {
        if (i % 480 == 0) {   // sweep 0 -> 1 -> 0 over 2 s in 200 steps
            float t = i / 96000.f;
            float n = t < 0.5f ? 2.f * t : 2.f * (1.f - t);
            s_rev.set_diffusion(n);
        }
        float wl, wr;
        s_rev.process(0.f, 0.f, wl, wr);
        if (!std::isfinite(wl)) { finite = false; break; }
        max_step = std::max(max_step, std::fabs(wl - prev));
        prev = wl;
    }
    CHECK(finite);
    CHECK(max_step < 1.f);   // density morph yes, discontinuities no
}
```

**(d)** In `"reverb: bit-deterministic across instances"` replace `rv->set_depth(0.6f);` (line 154) with:

```cpp
        rv->set_diffusion(0.6f);
```

In `tests/test_instrument.cpp` (line 121), in the null-safety case, replace `inst.set_reverb_depth(0.5f);` with:

```cpp
    inst.set_reverb_diffusion(0.5f);
```

- [ ] **Step 3: Run the tests to verify they fail**

```bash
source env.sh && cmake --build build 2>&1 | head -30
```

Expected: **compile error** — `AmbientReverb` has no member `set_diffusion` (and `Instrument` no `set_reverb_diffusion`).

- [ ] **Step 4: Implement the swap**

`engine/fx/reverb.h`: replace the line

```cpp
    void set_depth(float norm);   // delay-line mod amount (lush chorus)
```

with

```cpp
    void set_diffusion(float norm);  // room density: AP coeff 0..0.9 + weak line-mod coupling
```

and in the class comment (lines ~11–15), replace the sentence fragment `DEPTH chorus-modulates the
// lines.` with `DIFFUSION morphs the room from
// discrete slap echoes to a dense wash and drags a little line modulation
// along with it (the old DEPTH knob is gone).`

`engine/fx/reverb.cpp`:

1. Delete the constant `constexpr float kDiffusion = 0.625f;       // Oliverb stock` (line 15).
2. In `init`, delete the line `_verb.set_diffusion(kDiffusion);` and replace the boot-default line `set_depth(0.25f);` with `set_diffusion(0.7f);   // coeff 0.63 ~= the old stock 0.625 room` (keep it inside the boot-defaults block, comment alignment as neighbors).
3. Replace the whole `AmbientReverb::set_depth` definition with:

```cpp
void AmbientReverb::set_diffusion(float norm) {
    norm = clampf(norm, 0.f, 1.f);
    _verb.set_diffusion(0.90f * norm);
    // weak coupling: more smear = slightly more line motion (0.05..0.25 of
    // the old DEPTH range; the knob owns density, motion just rides along)
    _verb.set_mod_amount((0.05f + 0.20f * norm) * 450.f);
}
```

`engine/instrument.h` (line 66): replace

```cpp
    void set_reverb_depth(float n) { if (_reverb) _reverb->set_depth(n); }
```

with

```cpp
    void set_reverb_diffusion(float n) { if (_reverb) _reverb->set_diffusion(n); }
```

Note: `host/render/scenario.cpp:113` and `host/vcv/src/Spotymod.cpp:188` still call `set_reverb_depth` — they will break the build. That migration is Tasks 2/3, but the build must compile NOW: in THIS task, update those two call sites minimally as part of the rename (scenario.cpp: change the dispatch line to `else if (a == "set_reverb_diffusion") inst.set_reverb_diffusion(e.value);`; Spotymod.cpp line 188: `inst.set_reverb_diffusion(params[REV_DEPTH].getValue());` — the enum rename itself stays in Task 3). List both files in the commit.

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: all suites PASS (the migrated energy/tone cases must still hold with the small mod floor — if one fails, investigate the window sums before touching thresholds).

- [ ] **Step 6: Commit**

```bash
git add engine/fx/reverb.h engine/fx/reverb.cpp engine/instrument.h host/render/scenario.cpp host/vcv/src/Spotymod.cpp tests/test_reverb.cpp tests/test_instrument.cpp
git commit -m "feat(reverb): DIFFUSION replaces DEPTH — AP coeff 0..0.9 + weak mod coupling

set_diffusion drives the room density (0 = slap echoes, 0.63 ~= old
stock at boot 0.7, 0.9 = wash) and couples the line modulation weakly
(0.05..0.25 of the old DEPTH range). set_depth/set_reverb_depth are
gone ersatzlos, like set_shimmer in M4.5.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 2: Render-host action + `ambient_wash` migration

**Files:**
- Modify: `host/render/scenarios/ambient_wash.json:24`
- Test: `tests/test_scenario.cpp` (extend the fx-actions case)
- (The `scenario.cpp` dispatch line itself landed in Task 1 to keep the build green.)

**Interfaces:**
- Consumes: `Instrument::set_reverb_diffusion(float)` (Task 1), scenario action string `"set_reverb_diffusion"` (Task 1).
- Produces: migrated scenario surface — no JSON references `set_reverb_depth` anymore.

- [ ] **Step 1: Write the test addition**

In `tests/test_scenario.cpp`, inside `TEST_CASE("scenario: fx actions reach the instrument")`, after the `mix` event block (added in M4.8), add:

```cpp
    Event dif;     // global reverb action: no part, null-safe
    dif.action = "set_reverb_diffusion";
    dif.value = 0.7f;
    apply_event(inst, dif);
```

- [ ] **Step 2: Migrate the scenario JSON**

In `host/render/scenarios/ambient_wash.json` line 24, replace

```json
    {"action":"set_reverb_depth","value":0.348},
```

with

```json
    {"action":"set_reverb_diffusion","value":0.7},
```

- [ ] **Step 3: Run the suite and render the scenario**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
./build/render.exe host/render/scenarios/ambient_wash.json build/ambient_wash_m49.wav build/aw49.csv
```

Expected: all suites PASS; render exits 0 (the WAV is Task 4's by-ear asset — keep it).

- [ ] **Step 4: Commit**

```bash
git add tests/test_scenario.cpp host/render/scenarios/ambient_wash.json
git commit -m "feat(host): set_reverb_diffusion action; ambient_wash migrated off DEPTH

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 3: VCV — REV_DIFF knob, panel regen, plugin rebuild + install

**Files:**
- Modify: `host/vcv/res/gen_panel.py:104` (one `Ctl` line)
- Regenerate: `host/vcv/src/generated_panel.hpp`, `host/vcv/res/Spotymod.svg` (generator output, never hand-edit)
- Modify: `host/vcv/src/Spotymod.cpp` (default + forwarding param id)

**Interfaces:**
- Consumes: `Instrument::set_reverb_diffusion(float)`; generated enum `REV_DIFF`.
- Produces: usable DIFF knob in Rack; `REV_DEPTH` ceases to exist.

*(No engine-test cycle — the VCV host has no test suite; the gate is a clean build + install.)*

- [ ] **Step 1: Rename the control in the generator and regenerate**

In `host/vcv/res/gen_panel.py` line 104, replace

```python
    Ctl("REV_DEPTH", SMKNOB, R,  72.0, "DEPTH"),
```

with

```python
    Ctl("REV_DIFF",  SMKNOB, R,  72.0, "DIFF"),
```

Regenerate (Git Bash):

```bash
cd /c/Users/bernd/Documents/AI/Spotykach/host/vcv && python res/gen_panel.py
git diff --stat
```

Expected: `src/generated_panel.hpp` and `res/Spotymod.svg` change; header now has `REV_DIFF` (same enum position as the old `REV_DEPTH`, so no param-id shift) and table row `{REV_DIFF, WK_SMKNOB, {117.180f, 72.000f}, "DIFF"}`.

- [ ] **Step 2: Wire default and forwarding in Spotymod.cpp**

1. In `defaultFor(...)` (line ~98), replace `case REV_DEPTH:    return 0.25f;` with:

```cpp
            case REV_DIFF:     return 0.70f;   // coeff 0.63 ~= the old stock room
```

2. In the forwarding block (line ~188), replace the param id in the line Task 1 touched:

```cpp
        inst.set_reverb_diffusion(params[REV_DIFF].getValue());
```

- [ ] **Step 3: Build the plugin**

```bash
/c/msys64/usr/bin/bash.exe -lc 'export PATH="/c/Users/bernd/Documents/AI/mingw64/bin:$PATH"; cd "/c/Users/bernd/Documents/AI/Spotykach/host/vcv"; make -j4 CC=gcc CXX=g++'
```

Expected: clean compile, `host/vcv/plugin.dll` produced.

- [ ] **Step 4: Install into Rack**

```bash
/c/msys64/usr/bin/bash.exe -lc 'export PATH="/c/Users/bernd/Documents/AI/mingw64/bin:$PATH"; cd "/c/Users/bernd/Documents/AI/Spotykach/host/vcv"; make install CC=gcc CXX=g++ RACK_USER_DIR="/c/Users/bernd/AppData/Local/Rack2"'
```

Expected: `.vcvplugin` in `C:\Users\bernd\AppData\Local\Rack2\plugins-win-x64\`. (Both make calls need `CC=gcc CXX=g++` — WinLibs has no plain `cc`; learned in M4.8.)

- [ ] **Step 5: Commit**

```bash
git add host/vcv/res/gen_panel.py host/vcv/src/generated_panel.hpp host/vcv/res/Spotymod.svg host/vcv/src/Spotymod.cpp
git commit -m "feat(vcv): DIFF knob replaces DEPTH on the reverb row

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 4: Roadmap + grep gate + by-ear renders

**Files:**
- Modify: `docs/roadmap.md` (milestone table row + Done entry)
- Verify: grep gate, full suite, two renders

**Interfaces:**
- Consumes: everything above. Produces: nothing new — closes out M4.9.

- [ ] **Step 1: Add the roadmap entries**

Milestone table, directly under the M4.8 row:

```markdown
| **M4.9** | Reverb DIFFUSION knob (replaces DEPTH) — room density 0–0.9, weak line-mod coupling, full-wash first pass | ✅ **done** (engine + host; UI wiring deferred to M6) |
```

Done section, after the `### M4.8` entry:

```markdown
### M4.9 — Reverb DIFFUSION knob ✅

- `set_reverb_diffusion` (0..1) replaces `set_reverb_depth`: AP coefficient
  `0.90·n` (0 = discrete slap echoes, boot 0.7 → 0.63 ≈ the old stock 0.625
  room, 1.0 = dense wash that melts attacks), line modulation weakly coupled
  (`(0.05 + 0.20·n)·450` samples — motion rides the knob, never dominant).
  DEPTH is gone ersatzlos, like shimmer in M4.5.
- Motivation: at full MIX/DECAY/SIZE the Oliverb feeds the freshly diffused
  input straight to the output taps, so attacks punched through the wash;
  more diffusion smears the first pass (A/B verified by ear, 2026-07-14).
- Hosts: VCV `REV_DIFF` "DIFF" knob (same panel slot/param id as DEPTH),
  render action `set_reverb_diffusion`; `ambient_wash` migrated.
```

- [ ] **Step 2: Grep gate + full suite**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
grep -rn "set_reverb_depth\|REV_DEPTH" engine host tests docs --include="*" | grep -v Binary
grep -rn "set_depth" engine/fx engine/instrument.h tests/test_reverb.cpp
source env.sh && cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: first grep → **zero hits** (roadmap history prose that *narrates* the old knob is fine ONLY in past-tense Done sections; the M4.5/M4.8 entries mention DEPTH historically — those stay, so scope the grep judgment to code/tests/scenarios and flag any hit for assessment rather than blind deletion); second grep → zero hits (the per-part `set_depth` lives in `engine/parts/part.h`/`instrument.h:42` and other test files — NOT in these paths); suite green.

- [ ] **Step 3: By-ear renders**

Recreate the full-wash pluck experiment scenario (untracked) and render it at DIFF 1.0, plus keep the Task-2 `ambient_wash` render:

```bash
cat > build/wash_pluck_diff100.json <<'EOF'
{
  "sample_rate": 48000,
  "bpm": 110,
  "duration_s": 24,
  "init": [
    {"action":"set_sync_mode","part":0,"value":"sync"},
    {"action":"set_rate","part":0,"value":0.5},
    {"action":"set_step","part":0,"flag":true,"ivalue":8},
    {"action":"set_shape","part":0,"value":1.0},
    {"action":"set_range","part":0,"value":0.4},
    {"action":"set_target_active","part":0,"slot":2,"flag":true},
    {"action":"set_target_base","part":0,"slot":2,"value":0.25},
    {"action":"set_target_active","part":0,"slot":4,"flag":true},
    {"action":"set_target_base","part":0,"slot":4,"value":0.5},
    {"action":"set_voice_attack","part":0,"value":0.0},
    {"action":"set_voice_decay","part":0,"value":0.15},
    {"action":"set_target_active","part":1,"slot":4,"flag":false},
    {"action":"set_target_base","part":1,"slot":4,"value":0.0},
    {"action":"set_reverb_mix","value":1.0},
    {"action":"set_reverb_size","value":1.0},
    {"action":"set_reverb_decay","value":0.95},
    {"action":"set_reverb_tone","value":0.55},
    {"action":"set_reverb_diffusion","value":1.0},
    {"action":"set_fx_target_active","part":0,"slot":3,"flag":true},
    {"action":"set_fx_target_base","part":0,"slot":3,"value":0.85}
  ],
  "events": []
}
EOF
./build/render.exe build/wash_pluck_diff100.json build/wash_pluck_diff100.wav build/wpd.csv
```

Expected: render exits 0. Deliverables for the user's listen: `build/wash_pluck_diff100.wav` (attacks should melt beyond the old 0.85 experiment render, which is still at `build/wash_pluck_diff085.wav` for comparison) and `build/ambient_wash_m49.wav` (boot-character check ≈ pre-M4.9 `build/ambient_wash_m48.wav`). Do NOT judge the audio — hand the files to the controller/user.

- [ ] **Step 4: Commit**

```bash
git add docs/roadmap.md
git commit -m "docs(roadmap): M4.9 reverb DIFFUSION knob — done

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

## Post-plan notes

- **Branch finish:** after tasks + user by-ear sign-off (wash pluck at DIFF 1.0, ambient_wash boot character, Rack knob feel), use superpowers:finishing-a-development-branch (M4.8 precedent: merge into `melody-engine-rework`, user pushes).
- **Out of scope (per spec):** input swell on the send (parked as the next step if DIFF 1.0 isn't wash enough), independent mod-depth control, core (`third_party/oliverb/`) changes.

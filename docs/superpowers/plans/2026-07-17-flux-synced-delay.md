# FLUX Synced Delay + Tape-Bloom Feedback — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace FLUX's buried mod-only TIME/FEEDBACK with two direct panel knobs — a tempo-synced division `RATE` and a `FEEDBACK` reaching >100 % into a tanh-bounded self-oscillation bloom — and remove delay-time modulation.

**Architecture:** The engine core (`engine/`) gains a synced-time API on `Flux` fed by the existing `Transport` BPM and `kDivisions` ladder; the feedback loop swaps `SoftClip` for `tanh`. `PartFx`/`Instrument` forward BPM + rate down. The VCV host (`host/vcv/`) adds four appended params (two per part) wired to the new API. No new heap, no new per-sample cost beyond the tanh that already replaces SoftClip 1:1.

**Tech Stack:** C++17, clang + Ninja (engine host), doctest, Python panel generator, VCV Rack SDK 2.6.6 (plugin build, separate toolchain).

**Spec:** `docs/superpowers/specs/2026-07-17-flux-synced-delay-design.md`

## Global Constraints

- **Patch compatibility (hard):** new VCV params MUST be appended at the END of `PARAMS` in `gen_panel.py` (like FILT/TIDE/CHOKE), never inserted into `part_controls()` — inserting grows `PART_STRIDE` and shifts every existing param id, breaking saved `.vcv` patches. The `static_assert`s in `Spotymod.cpp` (RATE_B/TUNE_B/TRIGGER_B == *_A + PART_STRIDE) must still hold.
- **No heap in the engine:** echo buffers stay host-owned (`FxMem`), injected via `Flux::init`. No allocation in `Flux`/`PartFx`.
- **Portable core:** no hardware/host type crosses `instrument.h`. `Flux` sees only `float`s and ints.
- **CPU:** all division→time work is change-guarded (recompute only when rate or BPM changes), never per sample. `tanh` replaces `SoftClip` one-for-one.
- **FX target count unchanged:** keep `FXT_COUNT = 5` and the `FxTargetId` enum intact (lane↔pad mapping must not move). Only stop *consuming* `FXT_FLUX_TIME` in `PartFx`.
- **Build env:** engine tests build with `source env.sh` first (clang+Ninja, no MSVC). Test command: `source env.sh && cmake --build build --target spky_tests && ./build/spky_tests`. Filter a file's cases with `--test-case="flux*"`.
- **Commit trailer** on every commit:
  ```
  Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
  ```
- Work on `main` in the fork at `C:\Users\bernd\Documents\AI\Spotykach`.

## File Structure

- `engine/mod/divisions.h` — add FLUX division-slice constants + `flux_division_index()` (pure helpers, reuse `kDivisions`).
- `engine/fx/flux.h` / `flux.cpp` — synced-time API (`set_rate`, `set_bpm`, `delay_time()`), tanh feedback loop, extended feedback range; remove `set_time`.
- `engine/fx/part_fx.h` / `part_fx.cpp` — forward `set_bpm`/`set_flux_rate`; stop applying `FXT_FLUX_TIME`.
- `engine/instrument.h` / `instrument.cpp` — `set_flux_rate(p, idx)`; forward BPM to each part's FX in `set_tempo_bpm`.
- `host/vcv/res/gen_panel.py` + regenerated `host/vcv/src/generated_panel.hpp` (+ `res/Spotymod.svg`) — four appended params.
- `host/vcv/src/Spotymod.cpp` — tooltips (`FluxRateQuantity`, `FluxFbQuantity`), `configControls`, `defaultFor`, `pushParams`.
- `tests/test_flux.cpp`, `tests/test_part_fx.cpp` — synced-time, bloom, and forwarding cases.

---

### Task 1: FLUX division-slice helpers

**Files:**
- Modify: `engine/mod/divisions.h`
- Test: `tests/test_flux.cpp` (append cases)

**Interfaces:**
- Produces: `spky::kFluxRateOffset` (int, = 5), `spky::kFluxRateCount` (int, = 12), `spky::flux_division_index(float norm) -> int` in `[0, kFluxRateCount)`. Absolute ladder index = `kFluxRateOffset + flux_division_index(norm)`, indexing `kDivisions` from `"1/2"` (idx 5) through `"1/32"` (idx 16).

- [ ] **Step 1: Write the failing test** — append to `tests/test_flux.cpp` (it already `#include`s `fx/flux.h`; add the divisions include at the top of the file: `#include "mod/divisions.h"`):

```cpp
TEST_CASE("flux slice: norm endpoints hit 1/2 and 1/32") {
    CHECK(kFluxRateCount == 12);
    CHECK(kFluxRateOffset == 5);
    // norm 0 -> slice 0 -> kDivisions[5] == "1/2"
    CHECK(std::string(kDivisions[kFluxRateOffset + flux_division_index(0.f)].name) == "1/2");
    // norm 1 -> slice 11 -> kDivisions[16] == "1/32"
    CHECK(std::string(kDivisions[kFluxRateOffset + flux_division_index(1.f)].name) == "1/32");
    // norm ~0.273 -> slice 3 -> kDivisions[8] == "1/4"
    CHECK(std::string(kDivisions[kFluxRateOffset + flux_division_index(3.f/11.f)].name) == "1/4");
}
```

Add `#include <string>` at the top of `tests/test_flux.cpp` if not already present (it includes `<vector>`, `<cmath>`, `<algorithm>` — add `<string>`).

- [ ] **Step 2: Run test to verify it fails**

Run: `source env.sh && cmake --build build --target spky_tests 2>&1 | tail -5`
Expected: FAIL — compile error, `kFluxRateOffset`/`flux_division_index` not declared.

- [ ] **Step 3: Add the helpers** — in `engine/mod/divisions.h`, after the `nearest_division(...)` function (before the free-mode `kRateFreeMin` block), insert:

```cpp
// FLUX synced-delay rate: a slice of kDivisions starting at "1/2" (idx 5)
// through "1/32" (idx 16) — 12 rungs, incl. dotted & triplet. The shorter
// divisions above "1/2" keep every synced delay time inside the 5 s echo
// buffer down to ~24 BPM. Names come from kDivisions[kFluxRateOffset + i].
inline constexpr int kFluxRateOffset = 5;
inline constexpr int kFluxRateCount  = 12;

inline int flux_division_index(float norm) {
    return static_cast<int>(clampf(norm, 0.f, 1.f) * (kFluxRateCount - 1) + 0.5f);
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `source env.sh && cmake --build build --target spky_tests && ./build/spky_tests --test-case="flux slice*"`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add engine/mod/divisions.h tests/test_flux.cpp
git commit -m "$(cat <<'EOF'
feat(flux): division-slice helpers for the synced delay rate

kFluxRateOffset/Count + flux_division_index map a 0..1 knob onto the
"1/2".."1/32" slice of kDivisions (dotted & triplet included).

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 2: Flux synced time + BPM (remove free time)

**Files:**
- Modify: `engine/fx/flux.h`, `engine/fx/flux.cpp`
- Test: `tests/test_flux.cpp`

**Interfaces:**
- Consumes: Task 1 (`kFluxRateOffset`, `kFluxRateCount`, `division_hz`).
- Produces on `spky::Flux`: `void set_bpm(float bpm)`, `void set_rate(int slice_idx)` (slice in `[0, kFluxRateCount)`, clamped), `float delay_time() const` (current clamped delay in seconds). Removes `void set_time(float, bool)`.

- [ ] **Step 1: Write the failing tests** — in `tests/test_flux.cpp`, REPLACE the two free-time cases (`"flux: echo arrives at the mapped delay time (0.5 -> 0.5 s)"` and `"flux: time 0 maps to 50 ms"`) with synced-time cases, and update the `set_time` calls in `"flux: feedback produces decaying repeats"`:

```cpp
TEST_CASE("flux: synced 1/4 at 120 BPM = 0.5 s echo") {
    Flux f;
    f.init(48000.f, s_buf_l, s_buf_r);
    f.set_on(true, true);
    f.set_bpm(120.f);
    f.set_rate(3);                   // slice 3 -> kDivisions[8] "1/4" -> 0.5 s @120
    f.set_feedback(0.f);
    f.set_mix(1.f);
    CHECK(f.delay_time() == doctest::Approx(0.5f).epsilon(0.001));
    int idx = first_echo_index(f, 30000);
    CHECK(idx >= 23990);
    CHECK(idx <= 24100);
}

TEST_CASE("flux: synced 1/8 at 120 BPM = 0.25 s echo") {
    Flux f;
    f.init(48000.f, s_buf_l, s_buf_r);
    f.set_on(true, true);
    f.set_bpm(120.f);
    f.set_rate(6);                   // slice 6 -> kDivisions[11] "1/8" -> 0.25 s @120
    f.set_feedback(0.f);
    f.set_mix(1.f);
    CHECK(f.delay_time() == doctest::Approx(0.25f).epsilon(0.001));
}

TEST_CASE("flux: longest division clamps to the echo buffer at low BPM") {
    Flux f;
    f.init(48000.f, s_buf_l, s_buf_r);
    f.set_on(true, true);
    f.set_bpm(20.f);                 // "1/2" @20 BPM = 6 s > 5 s buffer
    f.set_rate(0);
    const float buf_s = (float)Flux::kMaxSamples / 48000.f;   // 5 s
    CHECK(f.delay_time() < buf_s);
    CHECK(f.delay_time() > buf_s - 0.1f);   // clamped just under the buffer
}
```

In `"flux: feedback produces decaying repeats"` replace `f.set_time(0.5f, true);` with:

```cpp
    f.set_bpm(120.f);
    f.set_rate(3);                   // 0.5 s echo, as before
```

- [ ] **Step 2: Run test to verify it fails**

Run: `source env.sh && cmake --build build --target spky_tests 2>&1 | tail -5`
Expected: FAIL — `set_bpm`/`set_rate`/`delay_time` not declared on `Flux`.

- [ ] **Step 3: Implement the synced API** — in `engine/fx/flux.h`:

Add the divisions include near the top (after the existing includes):
```cpp
#include "mod/divisions.h"
```

In `class Flux`, REPLACE the line `void set_time(float norm, bool immediate = false);   // 50 ms .. 5 s, exp` with:
```cpp
    void set_bpm(float bpm);              // recompute synced delay time on change
    void set_rate(int slice_idx);         // 0..kFluxRateCount-1 -> kDivisions slice
    float delay_time() const { return _delay_time; }   // seconds, clamped (test/meter)
```

Add these members to the `private:` block (alongside `_mix_lin`, `_buf_ok`):
```cpp
    float _sr = 48000.f;
    float _bpm = 120.f;
    int   _rate_idx = 3;         // "1/4"
    float _delay_time = 0.5f;
```

Add a private helper declaration in `private:`:
```cpp
    void recompute_time(bool immediate);
```

- [ ] **Step 4: Implement in `engine/fx/flux.cpp`**

In `Flux::init`, store the sample rate and replace the boot `set_time(...)` call. Change the lag time to 30 ms. The init block becomes:

```cpp
void Flux::init(float sample_rate, float* buf_l, float* buf_r) {
    _sw.init(sample_rate);
    _sr = sample_rate;
    _buf_ok = (buf_l != nullptr && buf_r != nullptr);
    if (!_buf_ok) return;
    _echo_l.Init(sample_rate, buf_l);
    _echo_r.Init(sample_rate, buf_r);
    _echo_l.SetLagTime(0.03f);   // short slew: click-free division changes, locks to grid
    _echo_r.SetLagTime(0.03f);
    _rate_idx = 3;               // boot "1/4"
    _bpm = 120.f;
    recompute_time(true);        // snap the boot time
    set_feedback(0.45f);
    set_mix(0.5f);
}
```

REMOVE the entire `Flux::set_time(...)` function and REPLACE it with:

```cpp
void Flux::set_bpm(float bpm) {
    if (bpm == _bpm) return;
    _bpm = bpm;
    recompute_time(false);
}

void Flux::set_rate(int slice_idx) {
    if (slice_idx == _rate_idx) return;
    _rate_idx = slice_idx;
    recompute_time(false);
}

void Flux::recompute_time(bool immediate) {
    if (!_buf_ok) return;
    int slice = _rate_idx < 0 ? 0
              : (_rate_idx >= kFluxRateCount ? kFluxRateCount - 1 : _rate_idx);
    float hz = division_hz(kFluxRateOffset + slice, _bpm);
    float t = (hz > 0.f) ? 1.f / hz : 0.5f;
    const float t_max = static_cast<float>(kMaxSamples - 2) / _sr;   // buffer safety
    _delay_time = clampf(t, 0.001f, t_max);
    _echo_l.SetDelayTime(_delay_time, immediate);
    _echo_r.SetDelayTime(_delay_time, immediate);
}
```

(`clampf` comes from `util/math.h`, already included in `flux.cpp`.)

- [ ] **Step 5: Run tests to verify they pass**

Run: `source env.sh && cmake --build build --target spky_tests && ./build/spky_tests --test-case="flux*"`
Expected: PASS (synced-time and clamp cases green; the decaying-repeats case still green).

- [ ] **Step 6: Commit**

```bash
git add engine/fx/flux.h engine/fx/flux.cpp tests/test_flux.cpp
git commit -m "$(cat <<'EOF'
feat(flux): tempo-synced delay time, free time removed

set_rate(slice)+set_bpm drive the echo from the kDivisions slice and the
master BPM; delay clamps to the 5 s buffer. Lag shortened to 30 ms so a
division change locks to the grid click-free. set_time() is gone.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 3: Feedback >100 % with tanh bloom

**Files:**
- Modify: `engine/fx/flux.h` (EchoDelay::Process), `engine/fx/flux.cpp` (set_feedback)
- Test: `tests/test_flux.cpp`

**Interfaces:**
- Produces: `Flux::set_feedback(float norm)` now maps `norm 0..1 -> 0..1.2` (≈120 %); the recirculating limiter is `std::tanh`, giving a bounded self-oscillation above unity.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_flux.cpp`:

```cpp
TEST_CASE("flux: feedback at max blooms but stays bounded") {
    Flux f;
    f.init(48000.f, s_buf_l, s_buf_r);
    f.set_on(true, true);
    f.set_bpm(120.f);
    f.set_rate(6);                   // 0.25 s
    f.set_feedback(1.f);             // -> 1.2 coefficient, self-oscillates
    f.set_mix(1.f);
    float peak = 0.f;
    for (int i = 0; i < 480000; ++i) {   // 10 s
        float l = (i == 0) ? 1.f : 0.f;
        float r = l;
        f.process(l, r);
        peak = std::max(peak, std::fabs(l));
        CHECK(std::isfinite(l));
    }
    CHECK(peak > 0.3f);              // it did bloom (sustained energy)
    CHECK(peak < 2.0f);              // but the tanh limiter kept it bounded
}

TEST_CASE("flux: feedback below unity decays to silence") {
    Flux f;
    f.init(48000.f, s_buf_l, s_buf_r);
    f.set_on(true, true);
    f.set_bpm(120.f);
    f.set_rate(3);                   // 0.5 s
    f.set_feedback(0.7f);            // -> 0.84 coefficient, below unity
    f.set_mix(1.f);
    std::vector<float> out(240000);
    for (int i = 0; i < (int)out.size(); ++i) {
        float l = (i == 0) ? 1.f : 0.f;
        float r = l;
        f.process(l, r);
        out[i] = l;
    }
    auto peak_around = [&](int c) {
        float p = 0.f;
        for (int i = c - 600; i < c + 600; ++i) p = std::max(p, std::fabs(out[i]));
        return p;
    };
    CHECK(peak_around(168000) < peak_around(24000));   // 7th repeat quieter than 1st
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `source env.sh && cmake --build build --target spky_tests && ./build/spky_tests --test-case="flux: feedback at max*"`
Expected: FAIL — at the old `*1.1` ceiling with `SoftClip` the bloom target/bounds differ (peak assertions miss).

- [ ] **Step 3: Swap the limiter and extend the range**

In `engine/fx/flux.h`, inside `EchoDelay::Process`, REPLACE:
```cpp
        out = daisysp::SoftClip(out);
```
with:
```cpp
        out = std::tanh(out);   // tape-warm limiter: transparent near unity,
                                // bounded self-oscillation above it (bloom)
```

In `engine/fx/flux.cpp`, change `Flux::set_feedback`:
```cpp
void Flux::set_feedback(float norm) {
    if (!_buf_ok) return;
    float fb = clampf(norm, 0.f, 1.f) * 1.2f;   // up to ~120%; tanh loop bounds the bloom
    _echo_l.SetFeedback(fb);
    _echo_r.SetFeedback(fb);
}
```

Also update the `set_feedback` doc comment in `flux.h` from `// 0 .. 1.1` to `// 0 .. 1.2 (tanh-bounded bloom)`.

- [ ] **Step 4: Run tests to verify they pass**

Run: `source env.sh && cmake --build build --target spky_tests && ./build/spky_tests --test-case="flux*"`
Expected: PASS (bloom bounded, sub-unity decays, earlier cases still green).

- [ ] **Step 5: Commit**

```bash
git add engine/fx/flux.h engine/fx/flux.cpp tests/test_flux.cpp
git commit -m "$(cat <<'EOF'
feat(flux): feedback to ~120% with a tanh-bounded tape bloom

tanh replaces SoftClip in the recirculating path: near-transparent at
unity, a bounded self-oscillation above it. Feedback knob now maps to
1.2 so the top of its travel reaches the bloom.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 4: PartFx — forward BPM/rate, stop applying TIME modulation

**Files:**
- Modify: `engine/fx/part_fx.h`, `engine/fx/part_fx.cpp`
- Test: `tests/test_part_fx.cpp`

**Interfaces:**
- Consumes: `Flux::set_bpm`, `Flux::set_rate` (Task 2).
- Produces on `spky::PartFx`: `void set_bpm(float bpm)`, `void set_flux_rate(int slice_idx)`. `PartFx::process` no longer routes `fxv[FXT_FLUX_TIME]` to the echo time.

- [ ] **Step 1: Write the failing test** — append to `tests/test_part_fx.cpp`:

```cpp
TEST_CASE("part_fx: synced rate + BPM place the echo, not FXT_FLUX_TIME") {
    PartFx fx;
    fx.init(48000.f, s_pf_l, s_pf_r);
    fx.set_fx_on(FxBlock::Flux, true, true);
    fx.set_flux_mix(1.f);              // 0 dB wet
    fx.set_bpm(120.f);
    fx.set_flux_rate(3);              // "1/4" @120 -> 0.5 s
    float v[FXT_COUNT];
    fill(v, 0.f, 0.99f, 1.f, 0.f, 0.f);   // FXT_FLUX_TIME = 0.99 must NOT move the echo
    int idx = -1;
    for (int i = 0; i < 30000; ++i) {
        float l = (i == 0) ? 1.f : 0.f;
        float r = l, sl, sr;
        fx.process(l, r, sl, sr, v);
        if (i > 100 && std::fabs(l) > 1e-3f) { idx = i; break; }
    }
    CHECK(idx >= 23900);
    CHECK(idx <= 24200);             // ~24000 (0.5 s), independent of v[FXT_FLUX_TIME]
}
```

(`PartFx::set_flux_mix` already exists.)

- [ ] **Step 2: Run test to verify it fails**

Run: `source env.sh && cmake --build build --target spky_tests 2>&1 | tail -5`
Expected: FAIL — `set_bpm`/`set_flux_rate` not declared on `PartFx`.

- [ ] **Step 3: Add the forwarders** — in `engine/fx/part_fx.h`, in the `public:` section near the other `set_flux_*` forwarders (`set_flux_mix`), add:

```cpp
    void set_bpm(float bpm)           { _flux.set_bpm(bpm); }
    void set_flux_rate(int slice_idx) { _flux.set_rate(slice_idx); }
```

- [ ] **Step 4: Stop applying the TIME modulation** — in `engine/fx/part_fx.cpp`, in `PartFx::process`, DELETE the line:

```cpp
        _flux.set_time(v[FXT_FLUX_TIME]);      // slewed inside EchoDelay (tape)
```

Leave the following `_flux.set_feedback(v[FXT_FLUX_FB]);` line intact.

- [ ] **Step 5: Run tests to verify they pass**

Run: `source env.sh && cmake --build build --target spky_tests && ./build/spky_tests --test-case="part_fx*"`
Expected: PASS (new case green; the existing part_fx cases — none of which assert echo timing — still green).

- [ ] **Step 6: Commit**

```bash
git add engine/fx/part_fx.h engine/fx/part_fx.cpp tests/test_part_fx.cpp
git commit -m "$(cat <<'EOF'
feat(part_fx): forward BPM/rate to FLUX; drop TIME modulation

set_bpm/set_flux_rate reach the echo directly. FXT_FLUX_TIME is no longer
consumed (the enum slot stays so lane indexing is unchanged).

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 5: Instrument wiring — rate setter + BPM fan-out

**Files:**
- Modify: `engine/instrument.h`, `engine/instrument.cpp`
- Test: `tests/test_part_fx.cpp` is sufficient for behavior; this task adds trivial forwarders verified by a full-suite green build.

**Interfaces:**
- Consumes: `PartFx::set_bpm`, `PartFx::set_flux_rate` (Task 4).
- Produces on `spky::Instrument`: `void set_flux_rate(int p, int slice_idx)`; `set_tempo_bpm` now also forwards BPM to each part's FX.

- [ ] **Step 1: Add the rate setter** — in `engine/instrument.h`, next to `set_flux_mix` (around line 59), add:

```cpp
    void set_flux_rate(int p, int slice_idx) { _parts[p].fx().set_flux_rate(slice_idx); }
```

- [ ] **Step 2: Fan BPM out to FX** — in `engine/instrument.cpp`, in `Instrument::set_tempo_bpm`, after the existing `for (auto& p : _parts) p.mod().set_tempo_bpm(bpm);` line, add:

```cpp
    for (auto& p : _parts) p.fx().set_bpm(bpm);
```

- [ ] **Step 3: Build and run the full suite**

Run: `source env.sh && cmake --build build --target spky_tests && ./build/spky_tests`
Expected: PASS — full suite green, no regressions.

- [ ] **Step 4: Commit**

```bash
git add engine/instrument.h engine/instrument.cpp
git commit -m "$(cat <<'EOF'
feat(instrument): set_flux_rate + BPM fan-out to FLUX

set_tempo_bpm now also drives each part's echo so synced delay times
track TEMPO/clock.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 6: VCV panel + Spotymod wiring

**Files:**
- Modify: `host/vcv/res/gen_panel.py`
- Regenerate: `host/vcv/src/generated_panel.hpp`, `host/vcv/res/Spotymod.svg`
- Modify: `host/vcv/src/Spotymod.cpp`

**Interfaces:**
- Consumes: `Instrument::set_flux_rate`, `Instrument::set_fx_target_base`, `spky::flux_division_index`, `spky::FXT_FLUX_FB`, `spky::kDivisions`, `spky::kFluxRateOffset`.
- Produces: four new params `FLUXRATE_A`, `FLUXRATE_B`, `FLUXFB_A`, `FLUXFB_B` (enums emitted by the generator), appended after `TIDE` so `PART_STRIDE` is unchanged.

- [ ] **Step 1: Append the params in the generator** — in `host/vcv/res/gen_panel.py`, in the `PARAMS = PART_A + PART_B + SHARED + [ ... ]` appended list, after the `Ctl("TIDE", ...)` entry, add:

```python
    # FLUX synced-delay controls (spec 2026-07-17 flux-synced-delay). Per part,
    # appended LAST like FILT/TIDE/CHOKE so existing .vcv patches keep their ids.
    # They fill the two free FX-row slots (x 9.5 and 74.5) on each side.
    Ctl("FLUXRATE_A", SMKNOB, 9.5,       88.9, "FRATE"),
    Ctl("FLUXRATE_B", SMKNOB, W - 9.5,   88.9, "FRATE"),
    Ctl("FLUXFB_A",   SMKNOB, 74.5,      88.9, "FFB"),
    Ctl("FLUXFB_B",   SMKNOB, W - 74.5,  88.9, "FFB"),
```

- [ ] **Step 2: Regenerate the header + SVG**

Run (from the repo root):
```bash
cd host/vcv && python res/gen_panel.py && cd ../..
```
Expected: rewrites `host/vcv/src/generated_panel.hpp` and `host/vcv/res/Spotymod.svg`. Confirm the new enums exist:
```bash
grep -n "FLUXRATE_A\|FLUXRATE_B\|FLUXFB_A\|FLUXFB_B" host/vcv/src/generated_panel.hpp
```
Expected: four enum entries present. (If `python` is not found, use `python3`.)

- [ ] **Step 3: Add the tooltips** — in `host/vcv/src/Spotymod.cpp`, after the `TideQuantity` struct (around line 32), add:

```cpp
// FLUX RATE tooltip: the synced division name (always synced).
struct FluxRateQuantity : ParamQuantity {
    std::string getDisplayValueString() override {
        int k = spky::kFluxRateOffset + spky::flux_division_index(getValue());
        return spky::kDivisions[k].name;
    }
};

// FLUX FB tooltip: percent, reaching >100% into the tanh bloom.
struct FluxFbQuantity : ParamQuantity {
    std::string getDisplayValueString() override {
        return string::f("%.0f%%", getValue() * 120.f);
    }
};
```

- [ ] **Step 4: Configure the new params** — in `Spotymod::configControls`, in the `WK_BIGKNOB`/`WK_SMKNOB` case, add two branches before the final `else` (which calls plain `configParam`):

```cpp
                    else if (c.id == FLUXRATE_A || c.id == FLUXRATE_B)
                        configParam<FluxRateQuantity>(c.id, 0.f, 1.f, defaultFor(c.id), lbl);
                    else if (c.id == FLUXFB_A || c.id == FLUXFB_B)
                        configParam<FluxFbQuantity>(c.id, 0.f, 1.f, defaultFor(c.id), lbl);
```

- [ ] **Step 5: Add the defaults** — in `Spotymod::defaultFor`, in the FIRST `switch (id)` (global/explicit ids, before the part-fold), add:

```cpp
            case FLUXRATE_A:   return 3.f / 11.f;   // "1/4" for part A's drone echo
            case FLUXRATE_B:   return 6.f / 11.f;   // "1/8" for part B's bass echo
            case FLUXFB_A:     return 0.45f;        // matches the retired FXT_FLUX_FB boot base
            case FLUXFB_B:     return 0.45f;
```

- [ ] **Step 6: Wire the knobs into the engine** — in `Spotymod::pushParams`, in the per-part `for (int p = 0; p < 2; ++p)` loop, right after `inst.set_flux_mix(p, pp(FLUX_A, p));`, add:

```cpp
            inst.set_flux_rate(p, spky::flux_division_index(
                params[p ? FLUXRATE_B : FLUXRATE_A].getValue()));
            inst.set_fx_target_base(p, spky::FXT_FLUX_FB,
                params[p ? FLUXFB_B : FLUXFB_A].getValue());
```

- [ ] **Step 7: Verify the engine suite still builds/passes** (guards the shared headers)

Run: `source env.sh && cmake --build build --target spky_tests && ./build/spky_tests`
Expected: PASS (the `static_assert`s on `PART_STRIDE` in `Spotymod.cpp` are host-side, but the engine suite confirms no shared-header breakage).

- [ ] **Step 8: Build the VCV plugin** (real host gate — separate toolchain, see `memory/spotykach-vcv-host-build-env.md`)

Run the documented VCV host build (Rack-SDK 2.6.6 + WinLibs mingw + MSYS2 make, `EXTRA_CXXFLAGS=-std=c++17`) for `host/vcv`. Expected: compiles clean; the four static_asserts hold; the module loads with `FRATE`/`FFB` knobs in the FX row and correct tooltips.

- [ ] **Step 9: Commit**

```bash
git add host/vcv/res/gen_panel.py host/vcv/src/generated_panel.hpp host/vcv/res/Spotymod.svg host/vcv/src/Spotymod.cpp
git commit -m "$(cat <<'EOF'
feat(vcv): FLUX RATE + FB knobs — synced delay, >100% feedback

Two per-part SMKNOBs appended after TIDE (patch ids preserved): FRATE
selects the synced division (tooltip shows the note value), FFB sets the
feedback base 0..120% (tooltip in percent). Wired to set_flux_rate and
set_fx_target_base(FXT_FLUX_FB).

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

## Self-Review

**Spec coverage:**
- §1 control surface (2 knobs/part, MIX unchanged, appended params) → Task 6.
- §2 synced time (slice from "1/2", `1/division_hz`, buffer clamp, 30 ms slew) → Tasks 1 & 2.
- §3 BPM plumbing → Tasks 4 & 5.
- §4 feedback >100 % + tanh bloom → Task 3.
- §5 modulation cleanup (drop TIME target, keep 5 lanes, FB base from knob) → Task 4 (drop) + Task 6 (FB base wiring).
- §6 CPU (change-guarded, tanh 1:1) → Tasks 2 & 3.
- Testing (synced time, clamp, bounded bloom, decay, forwarding) → Tasks 1–4.
- Note: the spec anticipated edits to `test_mod_tide.cpp`/`test_part.cpp`. On inspection those cases use `FXT_FLUX_TIME` only to exercise the generic `fx_target_value` combine logic (unchanged), so they need no edits — Task 5 Step 3 (full suite) confirms they stay green. If any fails, fix it there.

**Placeholder scan:** none — every step carries real code/commands.

**Type consistency:** `set_rate(int)`, `set_bpm(float)`, `delay_time()`, `flux_division_index(float)->int`, `kFluxRateOffset/Count`, `set_flux_rate(int)`, `FXT_FLUX_FB` used consistently across tasks. `flux_division_index` returns a slice index (0..11), consumed directly by `Flux::set_rate` and by `kFluxRateOffset + idx` for `kDivisions` lookups — consistent in Tasks 1, 2, 6.

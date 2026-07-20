# Spotykach M4.5 — Ambient Reverb v2 (Oliverb Port) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the shared `AmbientReverb` core (DaisySP `ReverbSc` + shimmer) with a vendored, MIT-licensed port of Oliverb (Clouds Parasite) — Doppler SIZE, DECAY past 100 % with in-loop soft bloom, TONE, DEPTH — and remove the DaisySP-LGPL dependency entirely.

**Architecture:** Oliverb (Griesinger loop: 4 input allpasses → 2 branches of 2 AP + 1 long delay, `SoftLimit` inside both branches, 9 random LFOs on the lines) is vendored into `third_party/oliverb/` with documented modifications: float32 buffer (32768 floats = 128 KB), delay constants ×1.5 for 48 kHz, pitch shifter removed, per-sample `Process`, deterministic injected `spky::Rng`. The existing `AmbientReverb` facade keeps its name, location, and injection point (shared wet-only room via `FxMem`); `set_shimmer` is deleted, `set_decay`/`set_depth` are added.

**Tech Stack:** C++17, doctest, CMake + clang + Ninja (desktop host; `source env.sh` first). Spec: `docs/superpowers/specs/2026-07-12-spotykach-ambient-reverb-v2-design.md` (residency repo).

## Global Constraints

- Repo: `C:\Users\bernd\Documents\AI\Spotykach` (work on `main`, as every milestone so far).
- Build/test cycle (run from the repo root, Git Bash):
  `source env.sh && cmake -B build -G Ninja && ninja -C build && ./build/spky_tests`
  (the `cmake` configure step is only needed when CMakeLists.txt changes; doctest filter: `./build/spky_tests -tc="<pattern>*"`).
- Engine purity (master spec): **no heap** in the audio path, **no libDaisy**, **no `<random>`**, no global RNG state — only `spky::Rng`, explicitly seeded. New vendored code must obey this too.
- **No libm in the per-sample audio path** (`expf`/`powf`/`fmap` only in setters / control-rate code, i.e. at most once per 96 samples). `spky::fast_sin` is the audio-path sine.
- **Bit-determinism:** identical scenario → bit-identical WAV; two identically-initialized reverb instances → bit-identical output.
- Firmware sources `src/`, `app.cpp`, `app.h`, `main.cpp` (repo root): **untouched**.
- License: only MIT/BSD-linked code after this milestone. The vendored files keep their upstream MIT copyright headers; our modifications are listed in a comment block directly under them.
- Commits end with: `Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>`
- 170 doctest cases currently green; existing tests may only change where this plan says so, and never get weaker.
- Current baseline to verify before starting: `git -C /c/Users/bernd/Documents/AI/Spotykach status` is clean, `./build/spky_tests` all green.

## File Structure

| File | Responsibility |
|---|---|
| `third_party/oliverb/stmlib_shim.h` | **create** — macros + `SoftLimit` + `CosineOscillator` (stmlib, MIT, Gillet), trimmed to what Oliverb needs, no ARM asm, no LUTs |
| `third_party/oliverb/random_oscillator.h` | **create** — Puech's smoothed random LFO (MIT), ported to injected `spky::Rng` + `fast_sin` raised-cosine |
| `third_party/oliverb/fx_engine.h` | **create** — Gillet's FxEngine (MIT), trimmed to FORMAT_32_BIT float |
| `third_party/oliverb/oliverb.h` | **create** — the reverb core (MIT), 48 kHz constants, no pitch shifter, per-sample Process |
| `engine/fx/reverb.h` / `.cpp` | **rewrite** — `AmbientReverb` facade: same class/name/process signature, new setter set + mappings, owns the 128 KB float buffer |
| `engine/instrument.h` | **modify** — reverb setter forwarding (decay/depth in, shimmer out) |
| `host/render/scenario.cpp` | **modify** — actions `set_reverb_decay`/`set_reverb_depth` in, `set_reverb_shimmer` out |
| `tests/test_oliverb.cpp` | **create** — shim- and core-level tests |
| `tests/test_reverb.cpp` | **rewrite** — facade behavior suite |
| `tests/test_instrument.cpp`, `tests/test_scenario.cpp`, `tests/test_fx_deps.cpp` | **modify** — migrate shimmer references, ReverbSc/PitchShifter removal |
| `CMakeLists.txt` | **modify** — third_party include for the engine, test file, LGPL removal |
| `THIRD_PARTY.md`, `docs/roadmap.md`, `README.md` | **modify** — Oliverb entry, LGPL exit, M4.5 milestone |
| `host/render/scenarios/*.json` | **modify** — shimmer→decay/depth migration; `ambient_wash.json` rebuilt as showcase |

**Semantic trap for every migration step:** the OLD `set_reverb_size` mapped to ReverbSc *feedback* (it WAS the decay). The NEW `set_reverb_size` is physical room size (delay lengths); tail length now lives on `set_reverb_decay`. Wherever old code/scenarios used `set_reverb_size` to make a tail short or long, that intent moves to `set_reverb_decay`.

---

### Task 1: Vendored shims + FxEngine (float) compile and behave

**Files:**
- Create: `third_party/oliverb/stmlib_shim.h`
- Create: `third_party/oliverb/random_oscillator.h`
- Create: `third_party/oliverb/fx_engine.h`
- Modify: `CMakeLists.txt:8-9` (engine include dirs), `CMakeLists.txt:39-80` (test source list)
- Test: `tests/test_oliverb.cpp`

**Interfaces:**
- Consumes: `spky::Rng` (`engine/mod/rng.h`: `seed(uint32_t)`, `next_unipolar()` → [0,1), `next_bipolar()` → [-1,1)), `spky::fast_sin(float phase_normalized)` (`engine/util/fast_sin.h`).
- Produces (used by Task 2): `namespace clouds`: `FxEngine<size, FORMAT_32_BIT>` with `Init(float*)`, `Clear()`, `SetLFOFrequency(LFOIndex, float)`, `Start(Context*)`, nested `Reserve<n, ...>` / `DelayLine<Memory, i>` / `Context` (methods `Load/Read/Write/WriteAllPass/Lp/Hp/SoftLimit/Interpolate/InterpolateHermite`); `clouds::RandomOscillator` with `Init(spky::Rng*)`, `set_slope(float)`, `Next()`; macros `ONE_POLE`, `CONSTRAIN`, `MAKE_INTEGRAL_FRACTIONAL`, `STATIC_ASSERT`, `DISALLOW_COPY_AND_ASSIGN`; `stmlib::SoftLimit(float)`.

- [ ] **Step 1: Write `third_party/oliverb/stmlib_shim.h`**

```cpp
// Minimal stand-in for the stmlib headers used by the vendored Oliverb code
// (fx_engine.h / oliverb.h / random_oscillator.h). SoftLimit and
// CosineOscillator are ported from stmlib — Copyright 2012-2014 Emilie Gillet,
// MIT License (https://github.com/pichenettes/stmlib) — trimmed to exactly
// what Oliverb needs: float only, approximate cosine mode only, no ARM
// intrinsics, no LUTs. Members are default-initialized (upstream left them
// uninitialized until Init).
#pragma once
#include <cstdint>

#define ONE_POLE(out, in, coefficient) out += (coefficient) * ((in) - out);
#define CONSTRAIN(var, min, max)                        \
  if (var < (min)) { var = (min); }                     \
  else if (var > (max)) { var = (max); }
#define MAKE_INTEGRAL_FRACTIONAL(x)                     \
  int32_t x ## _integral = static_cast<int32_t>(x);     \
  float x ## _fractional = x - static_cast<float>(x ## _integral);
#define STATIC_ASSERT(expression, message) static_assert(expression, #message)
#define DISALLOW_COPY_AND_ASSIGN(TypeName)              \
  TypeName(const TypeName&) = delete;                   \
  void operator=(const TypeName&) = delete

namespace stmlib {

inline float SoftLimit(float x) {
  return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}

// Magic-circle cosine oscillator, output ~0..1 (approximate mode).
class CosineOscillator {
 public:
  CosineOscillator() {}

  void Init(float frequency) {
    float sign = 16.0f;
    frequency -= 0.25f;
    if (frequency < 0.0f) {
      frequency = -frequency;
    } else if (frequency > 0.5f) {
      frequency -= 0.5f;
    } else {
      sign = -16.0f;
    }
    iir_coefficient_ = sign * frequency * (1.0f - 2.0f * frequency);
    initial_amplitude_ = iir_coefficient_ * 0.25f;
    Start();
  }

  void Start() {
    y1_ = initial_amplitude_;
    y0_ = 0.5f;
  }

  float value() const { return y1_ + 0.5f; }

  float Next() {
    float temp = y0_;
    y0_ = iir_coefficient_ * y0_ - y1_;
    y1_ = temp;
    return temp + 0.5f;
  }

 private:
  float y1_ = 0.0f;
  float y0_ = 0.5f;
  float iir_coefficient_ = 0.0f;
  float initial_amplitude_ = 0.0f;

  DISALLOW_COPY_AND_ASSIGN(CosineOscillator);
};

}  // namespace stmlib
```

- [ ] **Step 2: Write `third_party/oliverb/random_oscillator.h`**

Every vendored file in this plan starts with the upstream MIT notice. The
text is identical across them except for the copyright line — use this block
with the noted copyright holder (`random_oscillator.h`: "Copyright 2015
Matthias Puech." + "Author: Matthias Puech (matthias.puech@gmail.com)";
`fx_engine.h` / `oliverb.h`: "Copyright 2014 Emilie Gillet." + "Author:
Emilie Gillet (emilie.o.gillet@gmail.com)"):

```cpp
// Copyright <YEAR> <HOLDER>.
//
// Author: <AUTHOR LINE>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// See http://creativecommons.org/licenses/MIT/ for more information.
```

With Puech's notice at the top, `random_oscillator.h` continues:

```cpp
// Port modifications (Spotykach fork, spotymod, 2026-07-12):
//  - stmlib::Random (global RNG) replaced by an injected spky::Rng so two
//    reverb instances are bit-deterministic and independently seeded
//  - lut_raised_cos table lookup replaced by the engine's polynomial
//    fast_sin (raised cosine = 0.5 - 0.5*cos(pi*phase))
//  - phase_ / phase_increment_ / direction_ zeroed in Init (upstream left
//    them uninitialized)
#pragma once
#include "stmlib_shim.h"
#include "util/fast_sin.h"
#include "mod/rng.h"

namespace clouds {

const float kOscillationMinimumGap = 0.3f;

class RandomOscillator {
 public:
  void Init(spky::Rng* rng) {
    rng_ = rng;
    phase_ = 0.0f;
    phase_increment_ = 0.0f;
    direction_ = true;
    value_ = 0.0f;
    next_value_ = rng_->next_bipolar();
  }

  inline void set_slope(float slope) {
    float gap = next_value_ - value_;
    if (gap < 0.0f) gap = -gap;
    if (gap < 1e-6f) gap = 1e-6f;
    phase_increment_ = slope / gap;
    if (phase_increment_ > 1.0f) phase_increment_ = 1.0f;
  }

  float Next() {
    phase_ += phase_increment_;
    if (phase_ > 1.0f) {
      phase_ -= 1.0f;
      value_ = next_value_;
      direction_ = !direction_;
      float rnd = (1.0f - kOscillationMinimumGap) * rng_->next_unipolar()
                  + kOscillationMinimumGap;
      next_value_ = direction_ ? value_ + (1.0f - value_) * rnd
                               : value_ - (1.0f + value_) * rnd;
    }
    // raised cosine 0..1 over phase 0..1: 0.5 - 0.5*cos(pi*phase),
    // cos(pi*p) == sin(2*pi*(0.25 + 0.5*p))
    float rc = 0.5f - 0.5f * spky::fast_sin(0.25f + 0.5f * phase_);
    return value_ + (next_value_ - value_) * rc;
  }

 private:
  spky::Rng* rng_ = nullptr;
  float phase_ = 0.0f;
  float phase_increment_ = 0.0f;
  float value_ = 0.0f;
  float next_value_ = 0.0f;
  bool direction_ = true;
};

}  // namespace clouds
```

- [ ] **Step 3: Write `third_party/oliverb/fx_engine.h`**

Gillet's MIT notice (see Step 2) at the top, then this complete body — it is
upstream `clouds/dsp/fx/fx_engine.h` with exactly four changes: (a) includes
→ `stmlib_shim.h`, (b) 12/16-bit `DataType` specializations removed (they
need `stmlib::Clip16` / ARM `ssat`), (c) template default `FORMAT_32_BIT`,
(d) `SetLFOFrequency` calls the shim's plain `Init()`:

```cpp
// Port modifications (Spotykach fork, spotymod, 2026-07-12):
//  - stmlib includes replaced by stmlib_shim.h
//  - trimmed to the float path: FORMAT_12_BIT / FORMAT_16_BIT DataType
//    specializations removed (they need stmlib::Clip16 / ARM ssat)
//  - CosineOscillator::Init<mode> template call -> plain Init()
#ifndef CLOUDS_DSP_FX_FX_ENGINE_H_
#define CLOUDS_DSP_FX_FX_ENGINE_H_

#include <algorithm>
#include <cstdint>
#include <cstddef>

#include "stmlib_shim.h"

namespace clouds {

#define TAIL , -1

enum Format {
  FORMAT_32_BIT
};

enum LFOIndex {
  LFO_1,
  LFO_2
};

template<Format format>
struct DataType { };

template<>
struct DataType<FORMAT_32_BIT> {
  typedef float T;

  static inline float Decompress(T value) {
    return value;
  }

  static inline T Compress(float value) {
    return value;
  }
};

template<
    size_t size,
    Format format = FORMAT_32_BIT>
class FxEngine {
 public:
  typedef typename DataType<format>::T T;
  FxEngine() { }
  ~FxEngine() { }

  void Init(T* buffer) {
    buffer_ = buffer;
    Clear();
  }

  void Clear() {
    std::fill(&buffer_[0], &buffer_[size], 0);
    write_ptr_ = 0;
  }

  struct Empty { };

  template<int32_t l, typename T = Empty>
  struct Reserve {
    typedef T Tail;
    enum {
      length = l
    };
  };

  template<typename Memory, int32_t index>
  struct DelayLine {
    enum {
      length = DelayLine<typename Memory::Tail, index - 1>::length,
      base = DelayLine<Memory, index - 1>::base + DelayLine<Memory, index - 1>::length + 1
    };
  };

  template<typename Memory>
  struct DelayLine<Memory, 0> {
    enum {
      length = Memory::length,
      base = 0
    };
  };

  class Context {
   friend class FxEngine;
   public:
    Context() { }
    ~Context() { }

    inline void Load(float value) {
      accumulator_ = value;
    }

    inline void Read(float value, float scale) {
      accumulator_ += value * scale;
    }

    inline void Read(float value) {
      accumulator_ += value;
    }

    inline void Write(float& value) {
      value = accumulator_;
    }

    inline void Write(float& value, float scale) {
      value = accumulator_;
      accumulator_ *= scale;
    }

    template<typename D>
    inline void Write(D& d, int32_t offset, float scale) {
      STATIC_ASSERT(D::base + D::length <= size, delay_memory_full);
      T w = DataType<format>::Compress(accumulator_);
      if (offset == -1) {
        buffer_[(write_ptr_ + D::base + D::length - 1) & MASK] = w;
      } else {
        buffer_[(write_ptr_ + D::base + offset) & MASK] = w;
      }
      accumulator_ *= scale;
    }

    template<typename D>
    inline void Write(D& d, float scale) {
      Write(d, 0, scale);
    }

    template<typename D>
    inline void WriteAllPass(D& d, int32_t offset, float scale) {
      Write(d, offset, scale);
      accumulator_ += previous_read_;
    }

    template<typename D>
    inline void WriteAllPass(D& d, float scale) {
      WriteAllPass(d, 0, scale);
    }

    template<typename D>
    inline void Read(D& d, int32_t offset, float scale) {
      STATIC_ASSERT(D::base + D::length <= size, delay_memory_full);
      T r;
      if (offset == -1) {
        r = buffer_[(write_ptr_ + D::base + D::length - 1) & MASK];
      } else {
        r = buffer_[(write_ptr_ + D::base + offset) & MASK];
      }
      float r_f = DataType<format>::Decompress(r);
      previous_read_ = r_f;
      accumulator_ += r_f * scale;
    }

    template<typename D>
    inline void Read(D& d, float scale) {
      Read(d, 0, scale);
    }

    inline void Lp(float& state, float coefficient) {
      state += coefficient * (accumulator_ - state);
      accumulator_ = state;
    }

    inline void Hp(float& state, float coefficient) {
      state += coefficient * (accumulator_ - state);
      accumulator_ -= state;
    }

    inline void SoftLimit() {
      accumulator_ = stmlib::SoftLimit(accumulator_);
    }

    template<typename D>
    inline void Interpolate(D& d, float offset, float scale) {
      STATIC_ASSERT(D::base + D::length <= size, delay_memory_full);
      MAKE_INTEGRAL_FRACTIONAL(offset);
      float a = DataType<format>::Decompress(
          buffer_[(write_ptr_ + offset_integral + D::base) & MASK]);
      float b = DataType<format>::Decompress(
          buffer_[(write_ptr_ + offset_integral + D::base + 1) & MASK]);
      float x = a + (b - a) * offset_fractional;
      previous_read_ = x;
      accumulator_ += x * scale;
    }

    template<typename D>
      inline void InterpolateHermite(D& d, float offset, float scale) {
      STATIC_ASSERT(D::base + D::length <= size, delay_memory_full);
      MAKE_INTEGRAL_FRACTIONAL(offset);
      float xm1 = DataType<format>::Decompress(
        buffer_[(write_ptr_ + offset_integral + D::base - 1) & MASK]);
      float x0 = DataType<format>::Decompress(
        buffer_[(write_ptr_ + offset_integral + D::base + 0) & MASK]);
      float x1 = DataType<format>::Decompress(
        buffer_[(write_ptr_ + offset_integral + D::base + 1) & MASK]);
      float x2 = DataType<format>::Decompress(
        buffer_[(write_ptr_ + offset_integral + D::base + 2) & MASK]);

      float c = (x1 - xm1) * 0.5f;
      float v = x0 - x1;
      float w = c + v;
      float a = w + v + (x2 - x0) * 0.5f;
      float b_neg = w + a;
      float t = offset_fractional;
      float x = (((a * t) - b_neg) * t + c) * t + x0;
      previous_read_ = x;
      accumulator_ += x * scale;
    }

    template<typename D>
    inline void Interpolate(
        D& d, float offset, LFOIndex index, float amplitude, float scale) {
      STATIC_ASSERT(D::base + D::length <= size, delay_memory_full);
      offset += amplitude * lfo_value_[index];
      MAKE_INTEGRAL_FRACTIONAL(offset);
      float a = DataType<format>::Decompress(
          buffer_[(write_ptr_ + offset_integral + D::base) & MASK]);
      float b = DataType<format>::Decompress(
          buffer_[(write_ptr_ + offset_integral + D::base + 1) & MASK]);
      float x = a + (b - a) * offset_fractional;
      previous_read_ = x;
      accumulator_ += x * scale;
    }

    template<typename D>
    inline void InterpolateHermite(
        D& d, float offset, LFOIndex index, float amplitude, float scale) {
      STATIC_ASSERT(D::base + D::length <= size, delay_memory_full);
      offset += amplitude * lfo_value_[index];
      MAKE_INTEGRAL_FRACTIONAL(offset);
      float xm1 = DataType<format>::Decompress(
        buffer_[(write_ptr_ + offset_integral + D::base - 1) & MASK]);
      float x0 = DataType<format>::Decompress(
        buffer_[(write_ptr_ + offset_integral + D::base + 0) & MASK]);
      float x1 = DataType<format>::Decompress(
        buffer_[(write_ptr_ + offset_integral + D::base + 1) & MASK]);
      float x2 = DataType<format>::Decompress(
        buffer_[(write_ptr_ + offset_integral + D::base + 2) & MASK]);

      float c = (x1 - xm1) * 0.5f;
      float v = x0 - x1;
      float w = c + v;
      float a = w + v + (x2 - x0) * 0.5f;
      float b_neg = w + a;
      float t = offset_fractional;
      float x = (((a * t) - b_neg) * t + c) * t + x0;
      previous_read_ = x;
      accumulator_ += x * scale;
    }

   private:
    float accumulator_;
    float previous_read_;
    float lfo_value_[2];
    T* buffer_;
    int32_t write_ptr_;

    DISALLOW_COPY_AND_ASSIGN(Context);
  };

  inline void SetLFOFrequency(LFOIndex index, float frequency) {
    lfo_[index].Init(frequency * 32.0f);
  }

  inline void Start(Context* c) {
    --write_ptr_;
    if (write_ptr_ < 0) {
      write_ptr_ += size;
    }
    c->accumulator_ = 0.0f;
    c->previous_read_ = 0.0f;
    c->buffer_ = buffer_;
    c->write_ptr_ = write_ptr_;
    if ((write_ptr_ & 31) == 0) {
      c->lfo_value_[0] = lfo_[0].Next();
      c->lfo_value_[1] = lfo_[1].Next();
    } else {
      c->lfo_value_[0] = lfo_[0].value();
      c->lfo_value_[1] = lfo_[1].value();
    }
  }

 private:
  enum {
    MASK = size - 1
  };

  int32_t write_ptr_;
  T* buffer_;
  stmlib::CosineOscillator lfo_[2];

  DISALLOW_COPY_AND_ASSIGN(FxEngine);
};

}  // namespace clouds

#endif  // CLOUDS_DSP_FX_FX_ENGINE_H_
```

- [ ] **Step 4: Wire third_party into the engine include path and add the test file**

In `CMakeLists.txt` change:

```cmake
add_library(spky_engine INTERFACE)
target_include_directories(spky_engine INTERFACE engine)
```

to:

```cmake
add_library(spky_engine INTERFACE)
target_include_directories(spky_engine INTERFACE engine third_party)
```

and in the `render` target change `target_include_directories(render PRIVATE host engine)` to `target_include_directories(render PRIVATE host engine third_party)`.
Add `tests/test_oliverb.cpp` to the `spky_tests` source list (next to `tests/test_reverb.cpp`).

- [ ] **Step 5: Write the failing tests**

`tests/test_oliverb.cpp`:

```cpp
#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include "oliverb/stmlib_shim.h"
#include "oliverb/random_oscillator.h"
#include "oliverb/fx_engine.h"
#include "mod/rng.h"

TEST_CASE("oliverb shim: SoftLimit is odd and compressive") {
    CHECK(stmlib::SoftLimit(0.f) == 0.f);
    CHECK(stmlib::SoftLimit(1.f) == doctest::Approx(28.f / 36.f));
    CHECK(stmlib::SoftLimit(-1.f) == doctest::Approx(-28.f / 36.f));
    CHECK(stmlib::SoftLimit(0.5f) < 0.5f);   // compressive above 0
    CHECK(stmlib::SoftLimit(0.01f) == doctest::Approx(0.01f).epsilon(0.01));
}

TEST_CASE("oliverb shim: cosine oscillator oscillates in ~0..1") {
    stmlib::CosineOscillator osc;
    osc.Init(0.001f);
    float mn = 1.f, mx = 0.f;
    for (int i = 0; i < 4000; ++i) {
        float v = osc.Next();
        mn = std::min(mn, v);
        mx = std::max(mx, v);
    }
    CHECK(mn >= -0.1f);
    CHECK(mx <= 1.1f);
    CHECK(mx - mn > 0.5f);   // it actually swings
}

TEST_CASE("oliverb shim: random oscillator is deterministic and bounded") {
    spky::Rng ra, rb;
    ra.seed(0xfeed1u);
    rb.seed(0xfeed1u);
    clouds::RandomOscillator la, lb;
    la.Init(&ra);
    lb.Init(&rb);
    la.set_slope(0.001f);
    lb.set_slope(0.001f);
    bool identical = true, bounded = true;
    for (int i = 0; i < 20000; ++i) {
        float va = la.Next(), vb = lb.Next();
        if (va != vb) identical = false;
        if (va < -1.01f || va > 1.01f) bounded = false;
    }
    CHECK(identical);
    CHECK(bounded);
}

TEST_CASE("oliverb fx_engine: float delay line delays an impulse exactly") {
    typedef clouds::FxEngine<1024, clouds::FORMAT_32_BIT> E;
    static float buf[1024];
    static E eng;
    eng.Init(buf);
    typedef E::Reserve<100> Memory;
    E::DelayLine<Memory, 0> del;
    E::Context c;
    int peak_at = -1;
    for (int i = 0; i < 300; ++i) {
        eng.Start(&c);
        c.Read(i == 0 ? 1.f : 0.f, 1.f);
        c.Write(del, 0.f);
        c.Load(0.f);
        c.Read(del, 99, 1.f);
        float out;
        c.Write(out);
        if (out > 0.5f && peak_at < 0) peak_at = i;
    }
    CHECK(peak_at == 99);   // write at offset 0, read at offset 99 -> 99 samples
}
```

- [ ] **Step 6: Run tests to verify the new file fails to compile / tests fail before the headers exist, then passes after**

Run: `source env.sh && cmake -B build -G Ninja && ninja -C build && ./build/spky_tests -tc="oliverb*"`
Expected: all 4 new cases PASS; full suite `./build/spky_tests` stays green.

- [ ] **Step 7: Commit**

```bash
git add third_party/oliverb tests/test_oliverb.cpp CMakeLists.txt
git commit -m "feat(m4.5): vendor Oliverb support headers — stmlib shim, deterministic RandomOscillator, float FxEngine

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 2: The Oliverb core at 48 kHz, float, no pitch shifter

**Files:**
- Create: `third_party/oliverb/oliverb.h`
- Test: `tests/test_oliverb.cpp` (append)

**Interfaces:**
- Consumes: everything Task 1 produced.
- Produces (used by Task 3): `clouds::Oliverb` with
  `enum { kBufferSize = 32768 }`,
  `void Init(float* buffer, uint32_t seed)`,
  `void Prepare()` (control-rate LFO slope update),
  `void Process(float* left, float* right)` (per-sample; reads `*left`/`*right` as input, overwrites them with wet),
  setters `set_input_gain / set_decay / set_diffusion / set_lp / set_hp / set_size / set_mod_amount / set_mod_rate` (all `float`, raw DSP units — the facade maps normalized knobs onto them).

- [ ] **Step 1: Write `third_party/oliverb/oliverb.h`**

Keep Emilie Gillet's original MIT header (verbatim from upstream
`clouds/dsp/fx/oliverb.h`, "Copyright 2014 Emilie Gillet"), then:

```cpp
// Port modifications (Spotykach fork, spotymod, 2026-07-12):
//  - FORMAT_32_BIT float buffer (was FORMAT_16_BIT), kBufferSize 32768
//  - all delay-line lengths x1.5 (32 kHz -> 48 kHz), smear offsets too
//  - in-loop pitch shifter removed (shimmer dropped by design; kills the
//    lut_window dependency) — the two decay taps read the plain delays
//  - per-sample Process(left, right) replaces the FloatFrame block loop
//  - LFO slope computation moved to control-rate Prepare() (upstream did
//    it per Process call; per-sample that would be 9 divisions/sample)
//  - deterministic: injected spky::Rng seeds the 9 random line LFOs;
//    the two engine cosine LFOs get explicit frequencies (upstream never
//    initialized them for Oliverb)
//  - all loop/filter state zeroed in Init (upstream left lp/hp state and
//    smooth_size_ uninitialized until the first Process)
//  - size smoothing coefficient 0.0002 (upstream 0.01 at 32 kHz): stepped
//    control input (scenario events / M6 knob ticks) glides over ~100 ms,
//    which is what makes the Doppler warp audible as a ride, not a blip
#ifndef CLOUDS_DSP_FX_OLIVERB_H_
#define CLOUDS_DSP_FX_OLIVERB_H_

#include "stmlib_shim.h"
#include "fx_engine.h"
#include "random_oscillator.h"
#include "mod/rng.h"

namespace clouds {

class Oliverb {
 public:
  enum { kBufferSize = 32768 };

  Oliverb() { }
  ~Oliverb() { }

  void Init(float* buffer, uint32_t seed) {
    engine_.Init(buffer);
    engine_.SetLFOFrequency(LFO_1, 0.5f / 48000.0f);
    engine_.SetLFOFrequency(LFO_2, 0.3f / 48000.0f);
    rng_.seed(seed ? seed : 1u);
    diffusion_ = 0.625f;
    size_ = 0.5f;
    smooth_size_ = size_;
    mod_amount_ = 0.0f;
    mod_rate_ = 0.0f;
    input_gain_ = 0.5f;
    decay_ = 0.5f;
    lp_ = 0.7f;
    hp_ = 0.0f;
    lp_decay_1_ = lp_decay_2_ = 0.0f;
    hp_decay_1_ = hp_decay_2_ = 0.0f;
    for (int i = 0; i < 9; ++i) lfo_[i].Init(&rng_);
    Prepare();
  }

  // Control-rate (per ~96 samples): translate mod_rate_ into the 9 LFO
  // slopes (they depend on each LFO's current segment, so refresh often).
  void Prepare() {
    float slope = mod_rate_ * mod_rate_;
    slope *= slope * slope;
    slope /= 300.0f;   // upstream /200 at 32 kHz; x1.5 more ticks per second
    for (int i = 0; i < 9; ++i) lfo_[i].set_slope(slope);
  }

  void Process(float* left, float* right) {
    // Griesinger topology per the Dattorro paper: 4 AP input diffusers,
    // then a loop of 2x (2 AP + 1 long delay). All lengths are the
    // parasites 32 kHz values x1.5.
    typedef E::Reserve<170,      /* ap1    (113)  */
      E::Reserve<243,            /* ap2    (162)  */
      E::Reserve<362,            /* ap3    (241)  */
      E::Reserve<599,            /* ap4    (399)  */
      E::Reserve<1880,           /* dap1a  (1253) */
      E::Reserve<2607,           /* dap1b  (1738) */
      E::Reserve<5117,           /* del1   (3411) */
      E::Reserve<2270,           /* dap2a  (1513) */
      E::Reserve<2045,           /* dap2b  (1363) */
      E::Reserve<7173> > > > > > > > > > Memory;  /* del2 (4782) */
    E::DelayLine<Memory, 0> ap1;
    E::DelayLine<Memory, 1> ap2;
    E::DelayLine<Memory, 2> ap3;
    E::DelayLine<Memory, 3> ap4;
    E::DelayLine<Memory, 4> dap1a;
    E::DelayLine<Memory, 5> dap1b;
    E::DelayLine<Memory, 6> del1;
    E::DelayLine<Memory, 7> dap2a;
    E::DelayLine<Memory, 8> dap2b;
    E::DelayLine<Memory, 9> del2;
    E::Context c;

    const float kap = diffusion_;

    float lp_1 = lp_decay_1_;
    float lp_2 = lp_decay_2_;
    float hp_1 = hp_decay_1_;
    float hp_2 = hp_decay_2_;

    engine_.Start(&c);

    // Smooth size to avoid delay glitches; slow on purpose (Doppler ride).
    ONE_POLE(smooth_size_, size_, 0.0002f);

#define OLIVERB_INTERPOLATE_LFO(del, lfo, gain)              \
    {                                                        \
      float offset = (del.length - 1) * smooth_size_;        \
      offset += lfo.Next() * mod_amount_;                    \
      CONSTRAIN(offset, 1.0f, del.length - 1);               \
      c.InterpolateHermite(del, offset, gain);               \
    }

#define OLIVERB_INTERPOLATE(del, gain)                       \
    {                                                        \
      float offset = (del.length - 1) * smooth_size_;        \
      CONSTRAIN(offset, 1.0f, del.length - 1);               \
      c.InterpolateHermite(del, offset, gain);               \
    }

    // Smear AP1 inside the loop.
    c.Interpolate(ap1, 15.0f, LFO_1, 90.0f, 1.0f);
    c.Write(ap1, 150, 0.0f);

    c.Read(*left + *right, input_gain_);
    // Diffuse through 4 allpasses.
    OLIVERB_INTERPOLATE_LFO(ap1, lfo_[1], kap);
    c.WriteAllPass(ap1, -kap);
    OLIVERB_INTERPOLATE_LFO(ap2, lfo_[2], kap);
    c.WriteAllPass(ap2, -kap);
    OLIVERB_INTERPOLATE_LFO(ap3, lfo_[3], kap);
    c.WriteAllPass(ap3, -kap);
    OLIVERB_INTERPOLATE_LFO(ap4, lfo_[4], kap);
    c.WriteAllPass(ap4, -kap);

    float apout;
    c.Write(apout);

    // Main reverb loop, branch 1.
    OLIVERB_INTERPOLATE_LFO(del2, lfo_[5], decay_);
    c.Lp(lp_1, lp_);
    c.Hp(hp_1, hp_);
    c.SoftLimit();
    OLIVERB_INTERPOLATE_LFO(dap1a, lfo_[6], -kap);
    c.WriteAllPass(dap1a, kap);
    OLIVERB_INTERPOLATE(dap1b, kap);
    c.WriteAllPass(dap1b, -kap);
    c.Write(del1, 2.0f);
    c.Write(*left, 0.0f);

    c.Load(apout);

    // Main reverb loop, branch 2.
    OLIVERB_INTERPOLATE_LFO(del1, lfo_[7], decay_);
    c.Lp(lp_2, lp_);
    c.Hp(hp_2, hp_);
    c.SoftLimit();
    OLIVERB_INTERPOLATE_LFO(dap2a, lfo_[8], kap);
    c.WriteAllPass(dap2a, -kap);
    OLIVERB_INTERPOLATE(dap2b, -kap);
    c.WriteAllPass(dap2b, kap);
    c.Write(del2, 2.0f);
    c.Write(*right, 0.0f);

#undef OLIVERB_INTERPOLATE_LFO
#undef OLIVERB_INTERPOLATE

    lp_decay_1_ = lp_1;
    lp_decay_2_ = lp_2;
    hp_decay_1_ = hp_1;
    hp_decay_2_ = hp_2;
  }

  inline void set_input_gain(float input_gain) { input_gain_ = input_gain; }
  inline void set_decay(float decay) { decay_ = decay; }
  inline void set_diffusion(float diffusion) { diffusion_ = diffusion; }
  inline void set_lp(float lp) { lp_ = lp; }
  inline void set_hp(float hp) { hp_ = hp; }
  inline void set_size(float size) { size_ = size; }
  inline void set_mod_amount(float mod_amount) { mod_amount_ = mod_amount; }
  inline void set_mod_rate(float mod_rate) { mod_rate_ = mod_rate; }

 private:
  typedef FxEngine<kBufferSize, FORMAT_32_BIT> E;
  E engine_;

  spky::Rng rng_;
  float input_gain_;
  float decay_;
  float diffusion_;
  float lp_;
  float hp_;
  float size_, smooth_size_;
  float mod_amount_;
  float mod_rate_;

  float lp_decay_1_;
  float lp_decay_2_;
  float hp_decay_1_;
  float hp_decay_2_;

  RandomOscillator lfo_[9];

  DISALLOW_COPY_AND_ASSIGN(Oliverb);
};

}  // namespace clouds

#endif  // CLOUDS_DSP_FX_OLIVERB_H_
```

(Memory check: 170+243+362+599+1880+2607+5117+2270+2045+7173 = 22466 samples
+ 10 separator slots = 22476 ≤ 32768; the Context `STATIC_ASSERT` enforces it
at compile time. The dropped pitch-shifter code was upstream lines 110–119
and the four `InterpolateHermite(del*, phase/half, ...)` blend reads —
the two `OLIVERB_INTERPOLATE_LFO(del*, lfo_[5/7], decay_)` reads now carry
the full `decay_` gain, which is exactly upstream behavior at
`pitch_shift_amount_ == 0`.)

- [ ] **Step 2: Append the failing core tests to `tests/test_oliverb.cpp`**

```cpp
#include "oliverb/oliverb.h"

namespace {
// 128 KB buffer + core: statics, never on the stack.
float s_ob_buf[clouds::Oliverb::kBufferSize];
clouds::Oliverb s_ob;

void ob_defaults(clouds::Oliverb& ob) {
    ob.Init(s_ob_buf, 0x0BE21D5u);
    ob.set_size(0.5f);
    ob.set_decay(0.7f);
    ob.set_lp(0.6f);
    ob.set_hp(0.01f);
    ob.set_mod_amount(0.f);
    ob.set_mod_rate(0.5f);
    ob.Prepare();
}
} // namespace

TEST_CASE("oliverb core: silence in, exact silence out") {
    ob_defaults(s_ob);
    bool clean = true;
    for (int i = 0; i < 4000; ++i) {
        if (i % 96 == 0) s_ob.Prepare();
        float l = 0.f, r = 0.f;
        s_ob.Process(&l, &r);
        if (l != 0.f || r != 0.f) clean = false;
    }
    CHECK(clean);
}

TEST_CASE("oliverb core: impulse rings a decorrelated stereo tail") {
    ob_defaults(s_ob);
    float tail = 0.f, decorr = 0.f;
    for (int i = 0; i < 48000; ++i) {
        if (i % 96 == 0) s_ob.Prepare();
        float l = (i == 0) ? 1.f : 0.f;
        float r = l;
        s_ob.Process(&l, &r);
        if (i >= 12000) tail += l * l;
        float d = l - r;
        if (d < 0) d = -d;
        if (d > decorr) decorr = d;
    }
    CHECK(tail > 1e-6f);     // still ringing after 0.25 s at decay 0.7
    CHECK(decorr > 1e-4f);   // L and R differ
}

TEST_CASE("oliverb core: decay > 1 blooms but stays bounded") {
    ob_defaults(s_ob);
    s_ob.set_decay(1.05f);
    float peak = 0.f, late_energy = 0.f;
    bool finite = true;
    const int N = 48000 * 8;
    for (int i = 0; i < N; ++i) {
        if (i % 96 == 0) s_ob.Prepare();
        // 2 s of drive, then 6 s of nothing
        float in = (i < 96000) ? 0.4f * spky::fast_sin(220.f * i / 48000.f) : 0.f;
        float l = in, r = in;
        s_ob.Process(&l, &r);
        if (!std::isfinite(l) || !std::isfinite(r)) { finite = false; break; }
        float a = l < 0 ? -l : l;
        if (a > peak) peak = a;
        if (i >= N - 48000) late_energy += l * l;
    }
    CHECK(finite);
    CHECK(peak < 4.f);                          // SoftLimit holds the loop
    CHECK(late_energy / 48000.f > 0.0004f);     // RMS > 0.02 six seconds after input stopped
}

TEST_CASE("oliverb core: bit-deterministic across instances") {
    static float bufA[clouds::Oliverb::kBufferSize];
    static float bufB[clouds::Oliverb::kBufferSize];
    static clouds::Oliverb obA, obB;
    obA.Init(bufA, 0x1234u);
    obB.Init(bufB, 0x1234u);
    for (clouds::Oliverb* ob : { &obA, &obB }) {
        ob->set_size(0.6f);
        ob->set_decay(0.8f);
        ob->set_lp(0.5f);
        ob->set_hp(0.01f);
        ob->set_mod_amount(200.f);
        ob->set_mod_rate(0.6f);
        ob->Prepare();
    }
    bool identical = true;
    for (int i = 0; i < 48000; ++i) {
        if (i % 96 == 0) { obA.Prepare(); obB.Prepare(); }
        float in = (i == 0) ? 1.f : 0.2f * spky::fast_sin(110.f * i / 48000.f);
        float la = in, ra = in, lb = in, rb = in;
        obA.Process(&la, &ra);
        obB.Process(&lb, &rb);
        if (la != lb || ra != rb) { identical = false; break; }
    }
    CHECK(identical);
}
```

(Note `spky::fast_sin` takes normalized phase — `220.f * i / 48000.f` wraps
internally, which is all these drive signals need.)

- [ ] **Step 3: Run to verify fail (missing header), then build after writing the header**

Run: `source env.sh && ninja -C build && ./build/spky_tests -tc="oliverb*"`
Expected: all 8 `oliverb*` cases PASS; full suite green.

- [ ] **Step 4: Commit**

```bash
git add third_party/oliverb/oliverb.h tests/test_oliverb.cpp
git commit -m "feat(m4.5): port Oliverb core — float32, 48 kHz constants, no pitch shifter, deterministic LFOs

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 3: `AmbientReverb` facade rewrite + caller migration (one build unit)

`set_shimmer` disappears from the facade, and `instrument.h` / the tests call
it — the facade rewrite and the caller migration cannot compile separately.
They are one task and one commit.

**Files:**
- Rewrite: `engine/fx/reverb.h`, `engine/fx/reverb.cpp`
- Rewrite: `tests/test_reverb.cpp`
- Modify: `engine/instrument.h:58-60`
- Modify: `host/render/scenario.cpp:106-108`
- Modify: `tests/test_instrument.cpp` (~lines 117-119 and ~211-235)
- Modify: `tests/test_scenario.cpp` (~lines 88-100)
- Modify: `tests/test_fx_deps.cpp`

**Interfaces:**
- Consumes: `clouds::Oliverb` (Task 2), `daisysp::fmap` (`Utility/dsp.h`, MIT DaisySP core), `spky::clampf`/`TWO_PI` (`util/math.h`).
- Produces (used by Task 5 scenarios): `spky::AmbientReverb` with `init(float sample_rate)`, `set_size(float)`, `set_decay(float)`, `set_tone(float)`, `set_depth(float)` (all normalized 0..1), `process(float in_l, float in_r, float& out_l, float& out_r)`; `Instrument::set_reverb_size/decay/tone/depth(float)`; scenario actions `set_reverb_size`, `set_reverb_decay`, `set_reverb_tone`, `set_reverb_depth`. **`set_shimmer`, `shimmer()`, `set_reverb_shimmer` and the shimmer scenario action no longer exist** (unknown actions are ignored by design, so old third-party scenarios won't crash — they just lose the setting).

- [ ] **Step 1: Write `engine/fx/reverb.h`**

```cpp
#pragma once
#include "oliverb/oliverb.h"

namespace spky {

// The one shared room behind both parts. Input is the summed per-part sends
// (post-FX, morph-scaled in the Instrument mix) and joins the master AFTER
// the part mix as a wet-only signal.
//
// M4.5: the core is a vendored Oliverb (Clouds Parasite, MIT) — Erbe-Verb-
// style playable room. SIZE really rescales the delay reads (turning it
// Doppler-warps the tail), DECAY crosses 1.0 near the top of its travel
// into a soft-limited self-sustaining bloom, DEPTH chorus-modulates the
// lines. Shimmer is gone (so is the DaisySP-LGPL dependency).
//
// BIG object (~130 KB — the float delay buffer is an inline member). Never
// stack-allocate: the desktop host owns it as a static; the M6 firmware
// shell places it in SDRAM. Injected via FxMem.
class AmbientReverb {
public:
    void init(float sample_rate);
    void set_size(float norm);    // room size; smoothed inside -> Doppler ride
    void set_decay(float norm);   // loop gain; crosses 1.0 at ~0.9 (bloom above)
    void set_tone(float norm);    // loop LP damping 500 Hz .. 16 kHz, exp
    void set_depth(float norm);   // delay-line mod amount (lush chorus)
    void process(float in_l, float in_r, float& out_l, float& out_r);

private:
    clouds::Oliverb _verb;
    float _sr = 48000.f;
    int _ctrl = 0;   // control-rate divider for the LFO slope refresh
    float _buffer[clouds::Oliverb::kBufferSize];
};

} // namespace spky
```

- [ ] **Step 2: Write `engine/fx/reverb.cpp`**

```cpp
#include "fx/reverb.h"
#include "Utility/dsp.h"
#include "util/math.h"
#include <cmath>

using namespace spky;

namespace {
constexpr int kCtrlInterval = 96;          // engine control-rate raster
constexpr uint32_t kRngSeed = 0x0BE21D5u;  // fixed: bit-deterministic renders
constexpr float kModRate = 0.5f;           // internal LFO speed; DEPTH scales amount only
// ~80 Hz one-pole low-cut inside the loop: keeps the >100% bloom from
// accumulating DC / low-mid mud (parasites' anti-DC offset, same value).
constexpr float kHpPin = 0.01f;
constexpr float kDiffusion = 0.625f;       // Oliverb stock
constexpr float kInputGain = 0.5f;         // L+R sum -> mono average into the room
}

void AmbientReverb::init(float sample_rate) {
    _sr = sample_rate;
    _ctrl = 0;
    _verb.Init(_buffer, kRngSeed);
    _verb.set_diffusion(kDiffusion);
    _verb.set_input_gain(kInputGain);
    _verb.set_hp(kHpPin);
    _verb.set_mod_rate(kModRate);
    set_size(0.6f);     // boot defaults (spec: audible, nothing screams,
    set_decay(0.55f);   // nothing self-oscillates)
    set_tone(0.5f);
    set_depth(0.25f);
    _verb.Prepare();
}

void AmbientReverb::set_size(float norm) {
    // parasites mapping: keep the room inside the tuned sweet range
    _verb.set_size(0.05f + 0.94f * clampf(norm, 0.f, 1.f));
}

void AmbientReverb::set_decay(float norm) {
    // Linear to 1.0 at norm 0.9; the top 10% of travel pushes past unity
    // into the soft-limited bloom, capped at 1.05. (Ear-tunable knee.)
    float d = clampf(norm, 0.f, 1.f) * (1.f / 0.9f);
    _verb.set_decay(d > 1.05f ? 1.05f : d);
}

void AmbientReverb::set_tone(float norm) {
    float fc = daisysp::fmap(clampf(norm, 0.f, 1.f), 500.f, 16000.f,
                             daisysp::Mapping::LOG);
    // exact one-pole coefficient for that cutoff (control-rate libm is fine)
    _verb.set_lp(1.f - std::exp(-TWO_PI * fc / _sr));
}

void AmbientReverb::set_depth(float norm) {
    // parasites full-knob 300 samples at 32 kHz -> x1.5 at 48 kHz
    _verb.set_mod_amount(clampf(norm, 0.f, 1.f) * 450.f);
}

void AmbientReverb::process(float in_l, float in_r, float& out_l, float& out_r) {
    if (_ctrl == 0) {
        _verb.Prepare();
        _ctrl = kCtrlInterval;
    }
    --_ctrl;
    out_l = in_l;
    out_r = in_r;
    _verb.Process(&out_l, &out_r);
}
```

- [ ] **Step 3: Rewrite `tests/test_reverb.cpp`**

```cpp
#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include "fx/reverb.h"
using namespace spky;

// ~130 KB object: static, never on the stack. init() fully re-seeds all
// state (buffer, filters, LFOs, RNG), so sharing one instance is safe.
static AmbientReverb s_rev;

static std::vector<float> impulse_response(AmbientReverb& rv, int n,
                                           bool left_channel) {
    std::vector<float> out(n);
    for (int i = 0; i < n; ++i) {
        float wl = 0.f, wr = 0.f;
        float in = (i == 0) ? 1.f : 0.f;
        rv.process(in, in, wl, wr);
        out[i] = left_channel ? wl : wr;
    }
    return out;
}

TEST_CASE("reverb: silence in, exact silence out") {
    s_rev.init(48000.f);
    for (int i = 0; i < 2000; ++i) {
        float wl = 1.f, wr = 1.f;
        s_rev.process(0.f, 0.f, wl, wr);
        CHECK(wl == 0.f);
        CHECK(wr == 0.f);
    }
}

TEST_CASE("reverb: mono impulse produces a persistent stereo tail") {
    s_rev.init(48000.f);
    s_rev.set_decay(0.75f);
    auto l = impulse_response(s_rev, 48000, true);
    s_rev.init(48000.f);
    s_rev.set_decay(0.75f);
    auto r = impulse_response(s_rev, 48000, false);
    float tail = 0.f, decorr = 0.f;
    for (int i = 24000; i < 48000; ++i) tail += l[i] * l[i];
    for (int i = 0; i < 48000; ++i) decorr = std::max(decorr, std::fabs(l[i] - r[i]));
    CHECK(tail > 1e-6f);     // still ringing after 0.5 s
    CHECK(decorr > 1e-4f);   // L and R differ
}

TEST_CASE("reverb: below 100% the impulse energy decays monotonically") {
    s_rev.init(48000.f);
    s_rev.set_decay(0.4f);
    s_rev.set_depth(0.f);
    auto ir = impulse_response(s_rev, 48000, true);
    float w[4] = { 0.f, 0.f, 0.f, 0.f };
    for (int k = 0; k < 4; ++k)
        for (int i = k * 12000; i < (k + 1) * 12000; ++i) w[k] += ir[i] * ir[i];
    CHECK(w[0] > w[1]);
    CHECK(w[1] > w[2]);
    CHECK(w[2] > w[3]);
}

TEST_CASE("reverb: decay past 100% blooms, self-sustains, stays bounded") {
    s_rev.init(48000.f);
    s_rev.set_decay(1.f);    // internal loop gain 1.05 (capped)
    float peak = 0.f, late = 0.f;
    bool finite = true;
    const int N = 48000 * 8;
    for (int i = 0; i < N; ++i) {
        float in = (i < 96000) ? 0.3f * std::sin(6.2831853f * 220.f * i / 48000.f) : 0.f;
        float wl = 0.f, wr = 0.f;
        s_rev.process(in, in, wl, wr);
        if (!std::isfinite(wl) || !std::isfinite(wr)) { finite = false; break; }
        peak = std::max(peak, std::max(std::fabs(wl), std::fabs(wr)));
        if (i >= N - 48000) late += wl * wl;
    }
    CHECK(finite);                      // never runs away
    CHECK(peak < 4.f);                  // the in-loop SoftLimit holds it
    CHECK(late / 48000.f > 0.0004f);    // still singing 6 s after input stopped
}

TEST_CASE("reverb: size ride Doppler-warps without clicks") {
    s_rev.init(48000.f);
    s_rev.set_decay(0.9f);
    s_rev.set_size(0.7f);
    // ring the room first
    for (int i = 0; i < 24000; ++i) {
        float in = (i == 0) ? 1.f : 0.2f * std::sin(6.2831853f * 330.f * i / 48000.f);
        float wl, wr;
        s_rev.process(in, in, wl, wr);
    }
    float prev = 0.f, max_step = 0.f;
    bool finite = true;
    for (int i = 0; i < 96000; ++i) {
        if (i % 480 == 0) {  // sweep 0.7 -> 0.1 in 200 steps over 1 s, then back
            float t = i / 96000.f;
            float n = t < 0.5f ? 0.7f - 1.2f * t : 0.1f + 1.2f * (t - 0.5f);
            s_rev.set_size(n);
        }
        float wl, wr;
        s_rev.process(0.f, 0.f, wl, wr);
        if (!std::isfinite(wl)) { finite = false; break; }
        max_step = std::max(max_step, std::fabs(wl - prev));
        prev = wl;
    }
    CHECK(finite);
    CHECK(max_step < 1.f);   // Doppler yes, discontinuities no
}

TEST_CASE("reverb: tone closed removes high-frequency tail energy") {
    auto hf_ratio = [](const std::vector<float>& x) {
        float diff = 0.f, tot = 1e-12f;
        for (size_t i = 4801; i < x.size(); ++i) {
            float d = x[i] - x[i - 1];
            diff += d * d;
            tot += x[i] * x[i];
        }
        return diff / tot;   // first-difference energy ~ HF content proxy
    };
    s_rev.init(48000.f);
    s_rev.set_decay(0.7f);
    s_rev.set_depth(0.f);
    s_rev.set_tone(0.9f);
    auto bright = impulse_response(s_rev, 48000, true);
    s_rev.init(48000.f);
    s_rev.set_decay(0.7f);
    s_rev.set_depth(0.f);
    s_rev.set_tone(0.1f);
    auto dark = impulse_response(s_rev, 48000, true);
    CHECK(hf_ratio(bright) > hf_ratio(dark) * 1.5f);
}

TEST_CASE("reverb: depth animates the tail") {
    s_rev.init(48000.f);
    s_rev.set_decay(0.8f);
    s_rev.set_depth(0.f);
    auto still = impulse_response(s_rev, 48000, true);
    s_rev.init(48000.f);
    s_rev.set_decay(0.8f);
    s_rev.set_depth(0.9f);
    auto moving = impulse_response(s_rev, 48000, true);
    int diff = 0;
    for (int i = 4800; i < 48000; ++i)
        if (std::fabs(still[i] - moving[i]) > 1e-6f) ++diff;
    CHECK(diff > 1000);
}

TEST_CASE("reverb: bit-deterministic across instances") {
    static AmbientReverb rvA, rvB;
    rvA.init(48000.f);
    rvB.init(48000.f);
    for (AmbientReverb* rv : { &rvA, &rvB }) {
        rv->set_size(0.65f);
        rv->set_decay(0.85f);
        rv->set_tone(0.6f);
        rv->set_depth(0.6f);
    }
    bool identical = true;
    for (int i = 0; i < 48000; ++i) {
        float in = (i == 0) ? 1.f : 0.2f * std::sin(6.2831853f * 110.f * i / 48000.f);
        float la, ra, lb, rb;
        rvA.process(in, in, la, ra);
        rvB.process(in, in, lb, rb);
        if (la != lb || ra != rb) { identical = false; break; }
    }
    CHECK(identical);
}
```

- [ ] **Step 4: `engine/instrument.h` — replace the three reverb setters (lines 58-60) with four**

```cpp
    void set_reverb_size(float n)  { if (_reverb) _reverb->set_size(n); }
    void set_reverb_decay(float n) { if (_reverb) _reverb->set_decay(n); }
    void set_reverb_tone(float n)  { if (_reverb) _reverb->set_tone(n); }
    void set_reverb_depth(float n) { if (_reverb) _reverb->set_depth(n); }
```

- [ ] **Step 5: `host/render/scenario.cpp` — replace the shimmer action line (108) with**

```cpp
    else if (a == "set_reverb_decay")     inst.set_reverb_decay(e.value);
    else if (a == "set_reverb_depth")     inst.set_reverb_depth(e.value);
```

(keeping the existing `set_reverb_size` / `set_reverb_tone` lines directly above).

- [ ] **Step 6: `tests/test_instrument.cpp` — two edits**

(a) In the null-safety case (~line 117), replace

```cpp
    inst.set_reverb_size(0.9f);         // must not crash without a reverb
    inst.set_reverb_tone(0.2f);
    inst.set_reverb_shimmer(0.5f);
```

with

```cpp
    inst.set_reverb_size(0.9f);         // must not crash without a reverb
    inst.set_reverb_tone(0.2f);
    inst.set_reverb_decay(0.7f);
    inst.set_reverb_depth(0.5f);
```

(b) In `TEST_CASE("instrument M4: morph=1 injects no new reverb from part A (send isolated)")` (~line 219), the old line

```cpp
    x.set_reverb_size(0.1f); y.set_reverb_size(0.1f);   // short tail so 3 s covers full decay
```

used SIZE-as-feedback semantics. Tail length now lives on DECAY — replace with:

```cpp
    x.set_reverb_decay(0.15f); y.set_reverb_decay(0.15f);   // short tail so 3 s covers full decay
    x.set_reverb_size(0.2f);   y.set_reverb_size(0.2f);     // small room too
```

- [ ] **Step 7: `tests/test_scenario.cpp` — migrate the shimmer event (~line 95)**

Replace

```cpp
    Event shim;    // global reverb action: no part, null-safe
    shim.action = "set_reverb_shimmer";
    shim.value = 0.5f;
    apply_event(inst, shim);
```

with

```cpp
    Event dec;     // global reverb action: no part, null-safe
    dec.action = "set_reverb_decay";
    dec.value = 0.5f;
    apply_event(inst, dec);
```

- [ ] **Step 8: `tests/test_fx_deps.cpp` — drop the LGPL modules**

Remove the includes `Effects/pitchshifter.h` and `Effects/reverbsc.h`, and
delete the `static daisysp::ReverbSc rev; ... CHECK(energy > 0.f);` block and
the `static daisysp::PitchShifter ps; ...` block. The file keeps the
Overdrive / Decimator / SampleRateReducer checks (still used by GRIT) and its
top comment becomes:

```cpp
// Sanity: the DaisySP (MIT core) modules the FX chain needs compile and run
// on desktop (clang, no ARM). The reverb no longer uses DaisySP — see
// third_party/oliverb (M4.5).
```

- [ ] **Step 9: Full build + suite**

Run: `source env.sh && ninja -C build && ./build/spky_tests`
Expected: everything green (pre-M4.5 count 170 cases; Tasks 1-3 added 8
`oliverb*` cases and swapped the 5 old `reverb*` cases for 8 new ones —
expect 181).
Also: `grep -rn "set_shimmer\|set_reverb_shimmer\|shimmer()" engine host tests CMakeLists.txt` → no hits.

- [ ] **Step 10: Commit**

```bash
git add engine/fx/reverb.h engine/fx/reverb.cpp tests/test_reverb.cpp engine/instrument.h host/render/scenario.cpp tests/test_instrument.cpp tests/test_scenario.cpp tests/test_fx_deps.cpp
git commit -m "feat(m4.5): AmbientReverb on the Oliverb core — size/decay/tone/depth; shimmer deleted across API, actions, tests

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 4: LGPL exit + documentation

**Files:**
- Modify: `CMakeLists.txt:18-35` (daisysp_min)
- Modify: `THIRD_PARTY.md`
- Modify: `docs/roadmap.md`, `README.md`

**Interfaces:**
- Consumes: Task 3 (no code references ReverbSc/PitchShifter anymore).
- Produces: an MIT-clean build; docs that match reality.

- [ ] **Step 1: Remove the LGPL sources and include dirs from `daisysp_min`**

```cmake
add_library(daisysp_min STATIC
    lib/DaisySP/Source/Effects/overdrive.cpp
    lib/DaisySP/Source/Effects/decimator.cpp
    lib/DaisySP/Source/Effects/sampleratereducer.cpp
    lib/DaisySP/Source/Control/phasor.cpp
    lib/DaisySP/Source/Filters/svf.cpp
)
target_include_directories(daisysp_min PUBLIC
    lib/DaisySP/Source
    PRIVATE
    lib/DaisySP/Source/Utility
    lib/DaisySP/Source/Control
    lib/DaisySP/Source/Effects
)
```

- [ ] **Step 2: Fresh configure + full rebuild proves it**

Run: `source env.sh && rm -rf build && cmake -B build -G Ninja && ninja -C build && ./build/spky_tests`
Expected: clean rebuild, all tests green.
Then: `grep -rn "LGPL\|reverbsc\|ReverbSc\|pitchshifter\|PitchShifter" engine host tests CMakeLists.txt` → no hits.

- [ ] **Step 3: `THIRD_PARTY.md`**

(a) In the intro (lines 5-7), replace the sentence about the "one separately
licensed LGPL module set … linked as of M1.6" with:

```markdown
All linked components are permissively licensed (MIT). Between M1.6 and M4.5
the reverb linked DaisySP's separately-licensed LGPL `ReverbSc` module; as of
M4.5 the reverb is a vendored MIT Oliverb port and **no LGPL code is compiled
or linked** — the note below is retained for history.
```

(b) In the *Vendored* table add:

```markdown
| **Oliverb** (Clouds Parasite reverb) | `third_party/oliverb/` — from [mqtthiqs/parasites](https://github.com/mqtthiqs/parasites) `clouds/dsp/fx/` + [pichenettes/stmlib](https://github.com/pichenettes/stmlib) utilities | MIT | © 2014 Emilie Gillet, © 2015 Matthias Puech |
```

with a bullet below the table:

```markdown
- **Oliverb** — the shared ambient reverb core (M4.5): `oliverb.h`,
  `fx_engine.h` (Emilie Gillet), `random_oscillator.h` (Matthias Puech), and
  `stmlib_shim.h` (trimmed stmlib utilities). Vendored **with modifications**,
  each listed in a comment block under the original MIT notice in the
  respective file (float32 buffer, 48 kHz constants, pitch shifter removed,
  per-sample processing, deterministic injected RNG).
```

(c) Replace the "Note on DaisySP-LGPL" section's last paragraph (lines 50-55)
with:

```markdown
As of M4.5 nothing in this repository compiles or links DaisySP-LGPL code:
the reverb moved to the vendored MIT Oliverb port under `third_party/oliverb/`,
and `ReverbSc`/`PitchShifter` were removed. The `DaisySP-LGPL/` directory
still exists inside the `lib/DaisySP` submodule checkout but is not part of
any build target.
```

- [ ] **Step 4: `docs/roadmap.md` + `README.md`**

`docs/roadmap.md`: in the M1.6 summary (~line 92) change "a shared ambient
reverb (DaisySP `ReverbSc` + optional +12 st shimmer)" to "a shared ambient
reverb *(core replaced in M4.5 — Oliverb port, shimmer removed)*". After the
M4 section add:

```markdown
### M4.5 — Ambient reverb v2 (Oliverb port) ✅

The shared room becomes a playable instrument (spec:
`2026-07-12-spotykach-ambient-reverb-v2-design.md`, residency repo): vendored
MIT Oliverb core (Clouds Parasite) under `third_party/oliverb/` — float32,
48 kHz, deterministic. SIZE rescales the delay reads live (Doppler tail
warp), DECAY crosses 100 % at ~0.9 of its travel into a soft-limited bloom
(cap 1.05), TONE is the in-loop damping, DEPTH chorus-modulates the lines.
`set_shimmer` is gone (API + scenario action). Removing `ReverbSc` +
`PitchShifter` drops the DaisySP-LGPL dependency — the build is MIT-clean.
Facade, injection point (`FxMem`), and wet-only routing unchanged; the M6
shell places the ~130 KB object in SDRAM as before.
```

`README.md`: in the milestone table (line ~111-114 region) add after the M4 row:

```markdown
| **M4.5** | Ambient reverb v2 — Oliverb port: Doppler SIZE, DECAY > 100 % bloom, DEPTH; shimmer & LGPL removed | **done** (engine + host) |
```

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt THIRD_PARTY.md docs/roadmap.md README.md
git commit -m "feat(m4.5): drop DaisySP-LGPL from the build; document the Oliverb vendoring (MIT-clean)

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 5: Scenario migration + `ambient_wash` showcase + renders

**Files:**
- Rewrite: `host/render/scenarios/ambient_wash.json`
- Modify: `host/render/scenarios/capture_duet.json`, `capture_pentatonic.json`, `entropy_duet.json`, `flow_drone.json`
- Verify: every other scenario via grep

**Interfaces:**
- Consumes: scenario actions from Task 3.
- Produces: demo renders under `renders/` (gitignored — regenerable, do not commit).

- [ ] **Step 1: Rewrite `host/render/scenarios/ambient_wash.json` as the v2 showcase**

```json
{
  "sample_rate": 48000,
  "bpm": 60,
  "duration_s": 60,
  "init": [
    {"action":"set_engine","part":0,"value":"test_tone"},
    {"action":"set_engine","part":1,"value":"test_tone"},
    {"_comment":"Reverb v2 showcase. Act 1 (0-20s): Dorian melody over a breathing room. Act 2 (20-30s): DECAY rides past 100% - the room blooms and self-sustains, then falls back. Act 3 (36-46s): SIZE dives and swells - the tail Doppler-warps. Outro: settle."},
    {"action":"set_sync_mode","part":0,"value":"sync"},
    {"action":"set_rate","part":0,"value":0.12},
    {"action":"set_step","part":0,"flag":true,"ivalue":8},
    {"action":"set_shape","part":0,"value":0.3},
    {"action":"set_range","part":0,"value":0.45},
    {"action":"set_smooth","part":0,"value":0.5},
    {"action":"set_depth","part":0,"value":1.0},
    {"action":"set_target_active","part":0,"slot":2,"flag":true},
    {"action":"set_target_base","part":0,"slot":2,"value":0.55},
    {"action":"set_target_active","part":0,"slot":4,"flag":true},
    {"action":"set_target_base","part":0,"slot":4,"value":0.65},

    {"action":"set_reverb_size","value":0.55},
    {"action":"set_reverb_tone","value":0.5},
    {"action":"set_reverb_decay","value":0.7},
    {"action":"set_reverb_depth","value":0.35},
    {"action":"set_fx_target_active","part":0,"slot":3,"flag":true},
    {"action":"set_fx_target_base","part":0,"slot":3,"value":0.28},
    {"action":"set_fx_target_depth","part":0,"slot":3,"value":0.6},

    {"action":"set_fx_on","part":0,"value":"flux","flag":true},
    {"action":"set_flux_mix","part":0,"value":0.35},
    {"action":"set_fx_target_base","part":0,"slot":1,"value":0.55},
    {"action":"set_fx_target_base","part":0,"slot":4,"value":0.4},

    {"_comment":"PART B - low slow pad, breathing into the room on its own lane 3."},
    {"action":"set_sync_mode","part":1,"value":"free"},
    {"action":"set_rate","part":1,"value":0.05},
    {"action":"set_smooth","part":1,"value":0.7},
    {"action":"set_range","part":1,"value":0.2},
    {"action":"set_depth","part":1,"value":1.0},
    {"action":"set_target_active","part":1,"slot":2,"flag":true},
    {"action":"set_target_base","part":1,"slot":2,"value":0.2},
    {"action":"set_target_active","part":1,"slot":4,"flag":true},
    {"action":"set_target_base","part":1,"slot":4,"value":0.5},
    {"action":"set_fx_target_active","part":1,"slot":3,"flag":true},
    {"action":"set_fx_target_base","part":1,"slot":3,"value":0.24},
    {"action":"set_fx_target_depth","part":1,"slot":3,"value":0.5}
  ],
  "events": [
    {"t":20.0,"action":"set_reverb_decay","value":0.95,"_comment":"past 100% - the room blooms and carries itself"},
    {"t":30.0,"action":"set_reverb_decay","value":0.7,"_comment":"back below - the bloom decays out"},
    {"t":36.0,"action":"set_reverb_size","value":0.35,"_comment":"SIZE dive - the tail pitches UP (Doppler)"},
    {"t":38.5,"action":"set_reverb_size","value":0.15},
    {"t":41.0,"action":"set_reverb_size","value":0.5,"_comment":"SIZE swell - the tail pitches DOWN"},
    {"t":43.5,"action":"set_reverb_size","value":0.85},
    {"t":46.0,"action":"set_reverb_size","value":0.55},
    {"t":50.0,"action":"set_reverb_decay","value":0.5,"_comment":"settle for the outro"}
  ]
}
```

- [ ] **Step 2: Migrate the other scenarios (semantics: old size WAS decay — pick equivalent-tail decay values)**

- `capture_duet.json`: replace `{"action":"set_reverb_shimmer","value":0.0}` with
  `{"action":"set_reverb_decay","value":0.6},` and `{"action":"set_reverb_depth","value":0.2}` (size 0.5 / tone 0.55 lines stay).
- `capture_pentatonic.json`: replace `{"action":"set_reverb_shimmer","value":0.0}` with
  `{"action":"set_reverb_decay","value":0.65},` and `{"action":"set_reverb_depth","value":0.2}`.
- `entropy_duet.json`: replace `{"action":"set_reverb_shimmer","value":0.12}` with
  `{"action":"set_reverb_decay","value":0.72},` and `{"action":"set_reverb_depth","value":0.3}`.
- `flow_drone.json`: after the existing `{"action":"set_reverb_size","value":0.85}` line add
  `{"action":"set_reverb_decay","value":0.8},` and `{"action":"set_reverb_depth","value":0.3}` (long drone room — the old size 0.85 meant feedback 0.9).

Then verify nothing is left behind:
`grep -rn "set_reverb_shimmer" host/render/scenarios/` → no hits, and
`grep -ln "set_reverb_size" host/render/scenarios/*.json` → every listed file also sets `set_reverb_decay` (any scenario that sets size without decay inherits boot decay 0.55 — acceptable only if its old size was ≤ 0.6; otherwise add a matching decay line following the table above).

- [ ] **Step 3: Render everything + determinism check**

```bash
source env.sh && ninja -C build
mkdir -p renders
for s in host/render/scenarios/*.json; do
  n=$(basename "$s" .json)
  ./build/render "$s" "renders/$n.wav" "renders/$n.csv"
done
./build/render host/render/scenarios/ambient_wash.json renders/aw2.wav renders/aw2.csv
cmp renders/ambient_wash.wav renders/aw2.wav && echo DETERMINISTIC
rm renders/aw2.wav renders/aw2.csv
```

Expected: all scenarios render without error; `DETERMINISTIC` prints.
Sanity-check `renders/ambient_wash.wav` numerically: it must be non-silent in
the 20-30 s bloom window and clip-free (peak < 1.0) — a small python/ffprobe
peak check or the repo's established meter is fine.

- [ ] **Step 4: Run the full suite one last time**

Run: `./build/spky_tests`
Expected: all green.

- [ ] **Step 5: Commit**

```bash
git add host/render/scenarios
git commit -m "feat(m4.5): scenario migration + ambient_wash reverb-v2 showcase (bloom ride + Doppler size ride)

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

## Acceptance criteria (from the spec — verify at the end)

1. Fresh `rm -rf build` rebuild + all tests green with **no DaisySP-LGPL reference** in CMake/code (grep in Task 4 Step 2).
2. `set_shimmer` / `set_reverb_shimmer` grep-clean in `engine/ host/ tests/ CMakeLists.txt` (docs may mention it historically).
3. `ambient_wash.json` render: audible bloom crossing (20-30 s) and Doppler size ride (36-46 s), output bounded (no clipping, no runaway).
4. Determinism: double render byte-identical (Task 5 Step 3).
5. Render time per block does not exceed the M4 baseline noticeably (shimmer's pitch shifter left; the Oliverb loop arrived — spot-check with `time ./build/render host/render/scenarios/ambient_wash.json ...` vs. another scenario if in doubt).

## Deliberately NOT in this plan (controller follow-ups)

- **By-ear pass** over `renders/ambient_wash.wav` (bloom musicality, Doppler feel, decay knee position, size-smoothing speed 0.0002, depth range 450, mod rate 0.5 — all flagged ear-tunable constants live in `engine/fx/reverb.cpp`'s anonymous namespace and `third_party/oliverb/oliverb.h`).
- Residency-repo updates: superseded-notes in the M1.6 FX spec (reverb core + shimmer UX), dev-diary entry, milestone memory.
- M6 UX knob map for the 4th axis (SMOOTH → DECAY suggestion in the spec).

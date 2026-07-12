// Copyright 2015 Matthias Puech.
//
// Author: Matthias Puech (matthias.puech@gmail.com)
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
//
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

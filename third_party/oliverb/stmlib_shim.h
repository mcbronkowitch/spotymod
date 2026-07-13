// Copyright 2012-2014 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
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

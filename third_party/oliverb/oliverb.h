// Copyright 2014 Emilie Gillet.
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
//  - Clear() added: empties buffer + loop filter state, params survive
//    (backs the engine's M4.8 dry/wet clear-on-sleep bypass)
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
    diffuser_mod_amount_ = 0.0f;
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

  // Spotykach port addition: empty the room without touching parameters —
  // zeroes the delay buffer and the loop damping filter state. Used by the
  // engine's dry/wet clear-on-sleep bypass (M4.8).
  void Clear() {
    engine_.Clear();
    lp_decay_1_ = lp_decay_2_ = 0.0f;
    hp_decay_1_ = hp_decay_2_ = 0.0f;
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

#define OLIVERB_INTERPOLATE_LFO(del, lfo, gain, amt)         \
    {                                                        \
      float offset = (del.length - 1) * smooth_size_;        \
      offset += lfo.Next() * (amt);                          \
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
    OLIVERB_INTERPOLATE_LFO(ap1, lfo_[1], kap, diffuser_mod_amount_);
    c.WriteAllPass(ap1, -kap);
    OLIVERB_INTERPOLATE_LFO(ap2, lfo_[2], kap, diffuser_mod_amount_);
    c.WriteAllPass(ap2, -kap);
    OLIVERB_INTERPOLATE_LFO(ap3, lfo_[3], kap, diffuser_mod_amount_);
    c.WriteAllPass(ap3, -kap);
    OLIVERB_INTERPOLATE_LFO(ap4, lfo_[4], kap, diffuser_mod_amount_);
    c.WriteAllPass(ap4, -kap);

    float apout;
    c.Write(apout);

    // Main reverb loop, branch 1.
    OLIVERB_INTERPOLATE_LFO(del2, lfo_[5], decay_, mod_amount_);
    c.Lp(lp_1, lp_);
    c.Hp(hp_1, hp_);
    c.SoftLimit();
    OLIVERB_INTERPOLATE_LFO(dap1a, lfo_[6], -kap, mod_amount_);
    c.WriteAllPass(dap1a, kap);
    OLIVERB_INTERPOLATE(dap1b, kap);
    c.WriteAllPass(dap1b, -kap);
    c.Write(del1, 2.0f);
    c.Write(*left, 0.0f);

    c.Load(apout);

    // Main reverb loop, branch 2.
    OLIVERB_INTERPOLATE_LFO(del1, lfo_[7], decay_, mod_amount_);
    c.Lp(lp_2, lp_);
    c.Hp(hp_2, hp_);
    c.SoftLimit();
    OLIVERB_INTERPOLATE_LFO(dap2a, lfo_[8], kap, mod_amount_);
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
  // Separate depth for the input diffusers (ap1..ap4): their smear is what
  // melts slap-echoes into a dense wet wash, independent of the tail-delay
  // wobble that set_mod_amount() controls.
  inline void set_diffuser_mod_amount(float a) { diffuser_mod_amount_ = a; }
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
  float diffuser_mod_amount_;
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

#pragma once
#include <cmath>
#include "util/math.h"

namespace spky {

inline float wave_sine(float ph)     { return std::sin(ph * TWO_PI); }
inline float wave_triangle(float ph) { return ph < 0.5f ? (-1.f + 4.f * ph) : (3.f - 4.f * ph); }
inline float wave_ramp(float ph)     { return 2.f * ph - 1.f; }
inline float wave_pulse(float ph)    { return ph < 0.5f ? 1.f : -1.f; }

// Continuous morph across the bank as `shape` sweeps 0..1:
//   sine -> triangle -> ramp -> pulse -> sample&hold(random).
// `ph` is lane phase in [0,1); `sh_hold` is the per-cycle random value used at
// the S&H end. Returns bipolar [-1, 1].
inline float shape_value(float ph, float shape, float sh_hold) {
    shape = clampf(shape, 0.f, 1.f);
    float seg = shape * 4.f;
    int   i = static_cast<int>(seg);
    if (i > 3) i = 3;                    // shape == 1 -> f == 1: pure S&H, no pulse bleed
    float f = seg - i;
    switch (i) {
        case 0:  return lerpf(wave_sine(ph),     wave_triangle(ph), f);
        case 1:  return lerpf(wave_triangle(ph), wave_ramp(ph),     f);
        case 2:  return lerpf(wave_ramp(ph),     wave_pulse(ph),    f);
        default: return lerpf(wave_pulse(ph),    sh_hold,           f);
    }
}

} // namespace spky

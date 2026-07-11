#pragma once
#include <cmath>

namespace spky {

// Polynomial sine approximation on NORMALIZED phase: returns sin(2*pi*phase).
// Parabola + odd cubic refinement (the classic devmaster/Capens fast sine);
// max abs error < 1.2e-3, exact at 0 / 0.25 / 0.5 / 0.75.
//
// This is the audio-path sine for the M2 synth voice (MorphOsc core, sub
// oscillator, pan-law + drift-LFO evaluation): ~10-15 cycles on the M7 vs
// ~80-120 for libm sinf, which is what keeps the 8-voice worst case inside
// the CPU budget (spec "CPU budget"). One shared implementation so desktop
// renders and firmware output stay bit-identical. std::floor is the only
// libm call (cheap, and typically a single instruction).
inline float fast_sin(float phase) {
    float p = phase - std::floor(phase);                 // wrap to [0, 1)
    float q = p < 0.5f ? p : p - 1.f;                    // [-0.5, 0.5)
    float aq = q < 0.f ? -q : q;
    float y = 8.f * q * (1.f - 2.f * aq);                // parabola through the anchors
    float ay = y < 0.f ? -y : y;
    return 0.225f * (y * ay - y) + y;                    // refine toward true sine
}

} // namespace spky

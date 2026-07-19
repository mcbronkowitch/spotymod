#pragma once

namespace spky {

// Pade [5/4] rational approximation of tanh, hard-clamped to +-1.
//
//                x * (945 + x^2*(105 + x^2))
// fast_tanh(x) = ---------------------------   for |x| < 3.646739, else +-1
//                945 + x^2*(420 + 15*x^2)
//
// max abs error 1.36e-3 over [-8, 8]; exact at 0; f'(0) == 1; monotonic;
// odd-symmetric exactly; and |y| <= 1.0 strictly, never above.
//
// That last property is a contract, not a side effect. EchoDelay (fx/flux.h)
// runs this inside a feedback loop whose coefficient reaches 1.2 -- the loop
// stays bounded ONLY because this function does. The master limiter
// (fx/limiter.h) uses the asymptote as its ceiling. The raw Pade runs to x/15
// for large x, so the clamp is load-bearing; 3.646739 is exactly where the raw
// form reaches 1.0, which makes the join very nearly C1 (raw slope there
// ~0.005).
//
// ~30 cycles on the M7 against ~200 for libm tanhf. Chosen over the usual
// Pade [3/2] (17x less accurate for ~8 fewer cycles) because at feedback 1.2
// the echo self-oscillates and a curve error compounds over loop repeats:
// [3/2]'s 2.4 % would move the limit cycle audibly, this 0.14 % does not.
// Spec: docs/superpowers/specs/2026-07-19-fast-tanh-design.md.
//
// One shared implementation so desktop renders and firmware output stay
// bit-identical -- the same rule fast_sin.h states for itself. No libm call.
inline float fast_tanh(float x) {
    const float ax = x < 0.f ? -x : x;
    if (ax >= 3.646739f) return x < 0.f ? -1.f : 1.f;
    const float x2 = x * x;
    return x * (945.f + x2 * (105.f + x2))
             / (945.f + x2 * (420.f + 15.f * x2));
}

} // namespace spky

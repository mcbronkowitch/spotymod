#pragma once

namespace spky {

// Pade [5/4] rational approximation of tanh, hard-clamped to +-1.
//
//                x * (945 + x^2*(105 + x^2))
// fast_tanh(x) = ---------------------------   for |x| < 3.646739, else +-1
//                945 + x^2*(420 + 15*x^2)
//
// max abs error 1.36e-3 over [-8, 8]; exact at 0; f'(0) == 1; monotonic;
// odd-symmetric exactly; and |y| <= 1.0, enforced on the return value below.
//
// That last property is a contract, not a side effect. EchoDelay (fx/flux.h)
// runs this inside a feedback loop whose coefficient reaches 1.2 -- the loop
// stays bounded ONLY because this function does. The master limiter
// (fx/limiter.h) uses the asymptote as its ceiling. The raw Pade runs to x/15
// for large x, so the clamp is load-bearing; 3.646739 is meant to be exactly
// where the raw form reaches 1.0, which would make the join very nearly C1
// (raw slope there ~0.005) -- but the threshold alone does not guarantee the
// bound (see the comment on the clamp below), so the result is clamped too.
//
// ~30 cycles on the M7 against ~200 for libm tanhf. Chosen over the usual
// Pade [3/2] (17x less accurate for ~8 fewer cycles) because at feedback 1.2
// the echo self-oscillates and a curve error compounds over loop repeats:
// [3/2]'s 2.4 % would move the limit cycle audibly, this 0.14 % does not.
// Spec: docs/superpowers/specs/2026-07-19-fast-tanh-design.md.
//
// One shared implementation so the two targets run the same curve -- the same
// rule fast_sin.h states for itself. Firmware and bench build with
// -ffast-math -funroll-loops; the desktop CMake build sets neither, so FMA
// contraction and reciprocal division can differ in the last bit between the
// two. No libm call.
inline float fast_tanh(float x) {
    const float ax = x < 0.f ? -x : x;
    if (ax >= 3.646739f) return x < 0.f ? -1.f : 1.f;
    const float x2 = x * x;
    const float y = x * (945.f + x2 * (105.f + x2))
                      / (945.f + x2 * (420.f + 15.f * x2));
    // The clamp constant sits 4.1e-7 above the true root (3.6467385950), so a
    // 9.3e-6-wide band of floats just below it evaluates the rational form
    // marginally above 1.0 -- worst case 1.0000001192. Bounding the RESULT
    // rather than trusting the threshold makes |y| <= 1 hold regardless of the
    // constant's rounding, FMA contraction under -ffast-math, or the target's
    // division semantics. Two compares, ~2 cycles.
    return y > 1.f ? 1.f : (y < -1.f ? -1.f : y);
}

} // namespace spky

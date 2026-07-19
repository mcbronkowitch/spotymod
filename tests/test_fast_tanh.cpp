#include <doctest/doctest.h>
#include <cmath>
#include "util/fast_tanh.h"
using namespace spky;

// Dense sweep shared by the contract cases below. [-8, 8] covers well past the
// clamp point (3.646739) in both directions.
namespace {
constexpr int   kN  = 200000;
constexpr float kLo = -8.f;
constexpr float kSpan = 16.f;
inline float sweep_x(int i) { return kLo + i * (kSpan / kN); }
}

TEST_CASE("fast_tanh: bounded error vs std::tanh over [-8, 8]") {
    float max_err = 0.f;
    for (int i = 0; i <= kN; ++i) {
        float x   = sweep_x(i);
        float ref = static_cast<float>(std::tanh(static_cast<double>(x)));
        float err = std::fabs(fast_tanh(x) - ref);
        if (err > max_err) max_err = err;
    }
    CHECK(max_err < 2e-3f);          // measured 1.359e-3
}

TEST_CASE("fast_tanh: |y| <= 1 everywhere -- the loop-stability contract") {
    // EchoDelay runs this inside a feedback loop at coefficient 1.2, and the
    // master limiter uses the asymptote as its ceiling. Asserted as a hard
    // bound, NOT doctest::Approx: exceeding 1.0 by any amount diverges the
    // echo and clips the master. The unclamped Pade runs to x/15 and fails
    // this case -- that is what the clamp is for.
    float max_abs = 0.f;
    for (int i = 0; i <= kN; ++i) {
        float a = std::fabs(fast_tanh(sweep_x(i)));
        if (a > max_abs) max_abs = a;
    }
    CHECK(max_abs <= 1.0f);          // measured exactly 1.0000000000

    const float ext[] = {3.646739f, 3.7f, 1e3f, 1e6f, 1e30f};
    for (float x : ext) {
        CHECK(fast_tanh( x) <=  1.0f);
        CHECK(fast_tanh( x) >= -1.0f);
        CHECK(fast_tanh(-x) <=  1.0f);
        CHECK(fast_tanh(-x) >= -1.0f);
    }
}

TEST_CASE("fast_tanh: monotonic non-decreasing") {
    // A non-monotonic saturator folds the transfer curve; inside the echo loop
    // that reads as a bug, not as character.
    float prev = fast_tanh(kLo);
    bool  mono = true;
    for (int i = 1; i <= kN; ++i) {
        float y = fast_tanh(sweep_x(i));
        if (y < prev) mono = false;
        prev = y;
    }
    CHECK(mono);
}

TEST_CASE("fast_tanh: odd symmetry is exact") {
    // Same x2, same denominator, and IEEE754 negation of the numerator --
    // this is exact equality, not an approximation.
    for (int i = 0; i <= kN; ++i) {
        float x = sweep_x(i);
        CHECK(fast_tanh(-x) == -fast_tanh(x));
    }
}

TEST_CASE("fast_tanh: origin value and slope") {
    CHECK(fast_tanh(0.f) == 0.f);                    // exact, not Approx

    // Limiter::shape is C1-continuous at the knee only because tanh'(0) == 1.
    const float h = 1e-3f;
    const float d = (fast_tanh(h) - fast_tanh(-h)) / (2.f * h);
    CHECK(std::fabs(d - 1.f) < 1e-4f);               // measured 3.6e-7
}

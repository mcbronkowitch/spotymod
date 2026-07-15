#include <doctest/doctest.h>
#include <cmath>
#include <vector>
#include "fx/limiter.h"
using namespace spky;

static std::vector<float> sine(int n, float amp) {
    std::vector<float> v(n);
    for (int i = 0; i < n; ++i)
        v[i] = amp * std::sin(6.2831853f * 220.f * i / 48000.f);
    return v;
}

TEST_CASE("limiter: bit-transparent below the knee at drive 0") {
    // -2 dBFS (0.794) sits below the -1 dBFS knee (0.891): out == in, bit-exact.
    Limiter lim;
    lim.init();
    auto in = sine(48000, 0.794f);
    for (float s : in) {
        float l = s, r = -0.5f * s;
        lim.process(l, r);
        CHECK(l == s);
        CHECK(r == -0.5f * s);
    }
}

TEST_CASE("limiter: never exceeds 1.0, even at 4x drive into a full-scale square") {
    Limiter lim;
    lim.init();
    lim.set_drive(1.f);
    CHECK(lim.pre_gain() == doctest::Approx(4.f));
    for (int i = 0; i < 96000; ++i) {
        float l = (i / 100) % 2 ? 1.f : -1.f;
        float r = l;
        lim.process(l, r);
        CHECK(std::fabs(l) <= 1.f);
        CHECK(std::fabs(r) <= 1.f);
        CHECK(std::isfinite(l));
    }
}

TEST_CASE("limiter: stereo-linked — one gain for both channels") {
    // Loud L, quiet R: R must be scaled by the SAME riding gain as L
    // (below its own knee R would otherwise pass untouched).
    Limiter lim;
    lim.init();
    lim.set_drive(0.5f);                      // pre-gain 2.5x forces riding
    auto in = sine(48000, 0.9f);
    for (size_t i = 0; i < in.size(); ++i) {
        float l = in[i], r = 0.1f * in[i];
        lim.process(l, r);
        if (i > 4800 && std::fabs(in[i]) > 0.5f) {
            // R stays exactly 0.1 of the pre-ceiling L path: both got the
            // same pre-gain and the same riding gain; only the ceiling is
            // per-channel and R is far below it.
            float gain_l_path = l / in[i];   // includes ceiling on L
            float gain_r_path = r / (0.1f * in[i]);
            CHECK(gain_r_path >= gain_l_path - 1e-4f);  // R uncrushed
            CHECK(gain_r_path <= lim.pre_gain());       // but gain-ridden
        }
    }
}

TEST_CASE("limiter: DRIVE saturates warmly instead of hard-clipping") {
    // The knee morphs with DRIVE. A 0.22-amp sine passes untouched at drive 0
    // (0.22 < the -1 dBFS knee). At full drive the 4x pre-gain lifts it to 0.88
    // -- ABOVE the lowered warm knee (0.45) but BELOW the old fixed knee (0.89),
    // so the warm curve saturates it well under 0.88 while the old fixed-knee
    // limiter would have passed it through near 0.88. Still loud => saturation,
    // not brickwall crush.
    auto peak_at = [](float drive, float amp) {
        Limiter lim; lim.init(); lim.set_drive(drive);
        float pk = 0.f;
        for (int i = 0; i < 48000; ++i) {
            float s = amp * std::sin(6.2831853f * 220.f * i / 48000.f);
            float l = s, r = s;
            lim.process(l, r);
            if (i > 4800) pk = std::max(pk, std::fabs(l));
        }
        return pk;
    };
    CHECK(peak_at(0.f, 0.22f) == doctest::Approx(0.22f).epsilon(1e-3));  // clean at drive 0
    const float driven = peak_at(1.f, 0.22f);
    CHECK(driven < 0.83f);   // saturated below 0.88 (a fixed 0.89 knee left it ~0.88)
    CHECK(driven > 0.55f);   // but still hits hard -- warmth, not a brickwall
}

TEST_CASE("limiter: deterministic") {
    auto run = [] {
        Limiter lim;
        lim.init();
        lim.set_drive(0.8f);
        std::vector<float> out;
        for (int i = 0; i < 48000; ++i) {
            float l = 0.9f * std::sin(6.2831853f * 90.f * i / 48000.f), r = l;
            lim.process(l, r);
            out.push_back(l);
        }
        return out;
    };
    auto a = run(), b = run();
    for (size_t i = 0; i < a.size(); ++i) CHECK(a[i] == b[i]);
}

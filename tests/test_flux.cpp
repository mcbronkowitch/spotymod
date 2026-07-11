#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include "fx/flux.h"
using namespace spky;

// 5 s stereo of echo memory, shared by all cases in this file; Flux::init
// resets the lines, so every TEST_CASE starts from silence.
static float s_buf_l[Flux::kMaxSamples];
static float s_buf_r[Flux::kMaxSamples];

// Feed a unit impulse, return the index of the first echo arrival.
static int first_echo_index(Flux& f, int n) {
    for (int i = 0; i < n; ++i) {
        float l = (i == 0) ? 1.f : 0.f;
        float r = l;
        f.process(l, r);
        if (i > 100 && std::fabs(l) > 1e-3f) return i;
    }
    return -1;
}

TEST_CASE("flux: echo arrives at the mapped delay time (0.5 -> 0.5 s)") {
    Flux f;
    f.init(48000.f, s_buf_l, s_buf_r);
    f.set_on(true, true);
    f.set_time(0.5f, true);          // fmap LOG 0.05..5 -> exactly 0.5 s
    f.set_feedback(0.f);
    f.set_mix(1.f);                  // 0 dB
    int idx = first_echo_index(f, 30000);
    CHECK(idx >= 23990);
    CHECK(idx <= 24100);
}

TEST_CASE("flux: time 0 maps to 50 ms") {
    Flux f;
    f.init(48000.f, s_buf_l, s_buf_r);
    f.set_on(true, true);
    f.set_time(0.f, true);
    f.set_feedback(0.f);
    f.set_mix(1.f);
    int idx = first_echo_index(f, 5000);
    CHECK(idx >= 2390);              // 0.05 s = 2400 samples
    CHECK(idx <= 2500);
}

TEST_CASE("flux: feedback produces decaying repeats") {
    Flux f;
    f.init(48000.f, s_buf_l, s_buf_r);
    f.set_on(true, true);
    f.set_time(0.5f, true);
    f.set_feedback(0.45f);           // -> 0.495 linear
    f.set_mix(1.f);
    std::vector<float> out(80000);
    for (int i = 0; i < (int)out.size(); ++i) {
        float l = (i == 0) ? 1.f : 0.f;
        float r = l;
        f.process(l, r);
        out[i] = l;
    }
    auto peak_around = [&](int center) {
        float p = 0.f;
        for (int i = center - 600; i < center + 600; ++i)
            p = std::max(p, std::fabs(out[i]));
        return p;
    };
    float p1 = peak_around(24000);
    float p2 = peak_around(48000);
    float p3 = peak_around(72000);
    CHECK(p1 > 1e-3f);
    CHECK(p2 < p1);
    CHECK(p3 < p2);
}

TEST_CASE("flux: off is bit-exact dry") {
    Flux f;
    f.init(48000.f, s_buf_l, s_buf_r);
    for (int i = 0; i < 2000; ++i) {
        float s = std::sin(0.01f * i) * 0.4f;
        float l = s, r = s;
        f.process(l, r);
        CHECK(l == s);
        CHECK(r == s);
    }
}

TEST_CASE("flux: null buffers never engage") {
    Flux f;
    f.init(48000.f, nullptr, nullptr);
    f.set_on(true, true);
    CHECK(!f.has_buffers());
    CHECK(!f.engaged());
    float l = 0.5f, r = 0.5f;
    f.process(l, r);
    CHECK(l == 0.5f);
}

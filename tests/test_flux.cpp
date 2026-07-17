#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include "fx/flux.h"
#include "mod/divisions.h"
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

TEST_CASE("flux: synced 1/4 at 120 BPM = 0.5 s echo") {
    Flux f;
    f.init(48000.f, s_buf_l, s_buf_r);
    f.set_on(true, true);
    f.set_bpm(120.f);
    f.set_rate(3);                   // slice 3 -> kDivisions[8] "1/4" -> 0.5 s @120
    f.set_feedback(0.f);
    f.set_mix(1.f);
    CHECK(f.delay_time() == doctest::Approx(0.5f).epsilon(0.001));
    int idx = first_echo_index(f, 30000);
    CHECK(idx >= 23990);
    CHECK(idx <= 24100);
}

TEST_CASE("flux: synced 1/8 at 120 BPM = 0.25 s echo") {
    Flux f;
    f.init(48000.f, s_buf_l, s_buf_r);
    f.set_on(true, true);
    f.set_bpm(120.f);
    f.set_rate(6);                   // slice 6 -> kDivisions[11] "1/8" -> 0.25 s @120
    f.set_feedback(0.f);
    f.set_mix(1.f);
    CHECK(f.delay_time() == doctest::Approx(0.25f).epsilon(0.001));
}

TEST_CASE("flux: longest division clamps to the echo buffer at low BPM") {
    Flux f;
    f.init(48000.f, s_buf_l, s_buf_r);
    f.set_on(true, true);
    f.set_bpm(20.f);                 // "1/2" @20 BPM = 6 s > 5 s buffer
    f.set_rate(0);
    const float buf_s = (float)Flux::kMaxSamples / 48000.f;   // 5 s
    CHECK(f.delay_time() < buf_s);
    CHECK(f.delay_time() > buf_s - 0.1f);   // clamped just under the buffer
}

TEST_CASE("flux: feedback produces decaying repeats") {
    Flux f;
    f.init(48000.f, s_buf_l, s_buf_r);
    f.set_on(true, true);
    f.set_bpm(120.f);
    f.set_rate(3);                   // 0.5 s echo, as before
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

TEST_CASE("flux slice: norm endpoints hit 1/2 and 1/32") {
    CHECK(kFluxRateCount == 12);
    CHECK(kFluxRateOffset == 5);
    // norm 0 -> slice 0 -> kDivisions[5] == "1/2"
    CHECK(std::string(kDivisions[kFluxRateOffset + flux_division_index(0.f)].name) == "1/2");
    // norm 1 -> slice 11 -> kDivisions[16] == "1/32"
    CHECK(std::string(kDivisions[kFluxRateOffset + flux_division_index(1.f)].name) == "1/32");
    // norm ~0.273 -> slice 3 -> kDivisions[8] == "1/4"
    CHECK(std::string(kDivisions[kFluxRateOffset + flux_division_index(3.f/11.f)].name) == "1/4");
}

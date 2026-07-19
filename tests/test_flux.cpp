#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <cstring>
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

TEST_CASE("flux: feedback at max blooms but stays bounded") {
    Flux f;
    f.init(48000.f, s_buf_l, s_buf_r);
    f.set_on(true, true);
    f.set_bpm(120.f);
    f.set_rate(6);                   // 0.25 s
    f.set_feedback(1.f);             // -> 1.2 coefficient, self-oscillates
    f.set_mix(1.f);
    float peak = 0.f;
    double late_sum_sq = 0.0;
    int late_n = 0;
    for (int i = 0; i < 480000; ++i) {   // 10 s
        float l = (i == 0) ? 1.f : 0.f;
        float r = l;
        f.process(l, r);
        peak = std::max(peak, std::fabs(l));
        CHECK(std::isfinite(l));
        if (i >= 432000) {               // last ~1 s (432000..480000)
            late_sum_sq += (double)l * (double)l;
            ++late_n;
        }
    }
    float late_rms = (float)std::sqrt(late_sum_sq / late_n);
    CHECK(peak > 0.3f);              // it did bloom (sustained energy)
    CHECK(peak < 2.0f);              // but the tanh limiter kept it bounded
    // Sustain check: this is what actually distinguishes self-oscillation
    // from mere boundedness. Measured late_rms ~0.095 here (feedback=1.0,
    // coefficient 1.2); the sub-unity decay case (feedback=0.7) measures
    // ~0.0005 in an equivalent late window, i.e. it has decayed to silence.
    // 0.03 sits with comfortable margin below the sustained value and two
    // orders of magnitude above the decayed one.
    CHECK(late_rms > 0.03f);
}

TEST_CASE("flux: feedback below unity decays to silence") {
    Flux f;
    f.init(48000.f, s_buf_l, s_buf_r);
    f.set_on(true, true);
    f.set_bpm(120.f);
    f.set_rate(3);                   // 0.5 s
    f.set_feedback(0.7f);            // -> 0.84 coefficient, below unity
    f.set_mix(1.f);
    std::vector<float> out(240000);
    for (int i = 0; i < (int)out.size(); ++i) {
        float l = (i == 0) ? 1.f : 0.f;
        float r = l;
        f.process(l, r);
        out[i] = l;
    }
    auto peak_around = [&](int c) {
        float p = 0.f;
        for (int i = c - 600; i < c + 600; ++i) p = std::max(p, std::fabs(out[i]));
        return p;
    };
    CHECK(peak_around(168000) < peak_around(24000));   // 7th repeat quieter than 1st
}

TEST_CASE("echo: zero writeback is bit-exact with the one-arg store") {
    static float a_buf[Flux::kMaxSamples];
    static float b_buf[Flux::kMaxSamples];
    EchoDelay<Flux::kMaxSamples> a, b;
    a.Init(48000.f, a_buf);
    b.Init(48000.f, b_buf);
    a.SetFeedback(0.6f);
    b.SetFeedback(0.6f);
    // EchoDelay has no sample rate or smoother of its own since 8723bc5 -- the
    // caller passes an already-slewed length in samples.
    const float ds = 0.25f * 48000.f;
    for (int i = 0; i < 60000; ++i) {
        float in = std::sin(0.013f * i) * 0.7f;
        float ya = a.Process(in, ds);
        float yb = b.Process(in, ds, 0.f);
        REQUIRE(ya == yb);
    }
}

TEST_CASE("echo: freeze stops writing but keeps the pointer moving") {
    static float buf[Flux::kMaxSamples];
    EchoDelay<Flux::kMaxSamples> e;
    e.Init(48000.f, buf);
    e.SetFeedback(0.5f);
    const float ds = 0.25f * 48000.f;
    for (int i = 0; i < 48000; ++i) e.Process(std::sin(0.01f * i) * 0.5f, ds);

    // snapshot the whole line, then freeze and hammer it with loud input
    static float snap[Flux::kMaxSamples];
    std::memcpy(snap, e.line(), sizeof(snap));
    const int32_t p0 = e.write_ptr();
    e.set_freeze(true);
    e.set_wear(1.f);
    for (int i = 0; i < 24000; ++i) e.Process(1.f, ds);

    CHECK(std::memcmp(snap, e.line(), sizeof(snap)) == 0);   // nothing stored
    const int32_t expect = (p0 - 24000 + 2 * (int32_t)Flux::kMaxSamples)
                         % (int32_t)Flux::kMaxSamples;
    CHECK(e.write_ptr() == expect);                          // but it advanced
}

TEST_CASE("echo: frozen with wear < 1 decays the loop, bounded") {
    static float buf[Flux::kMaxSamples];
    EchoDelay<Flux::kMaxSamples> e;
    e.Init(48000.f, buf);
    e.SetFeedback(0.5f);
    const float ds = 0.25f * 48000.f;
    for (int i = 0; i < 48000; ++i) e.Process(std::sin(0.01f * i) * 0.5f, ds);

    auto rms = [&]() {
        double s = 0.0;
        for (size_t i = 0; i < Flux::kMaxSamples; ++i)
            s += (double)e.line()[i] * (double)e.line()[i];
        return std::sqrt(s / (double)Flux::kMaxSamples);
    };
    const double before = rms();
    e.set_freeze(true);
    e.set_wear(1.f - 4.0e-6f);
    for (size_t i = 0; i < Flux::kMaxSamples; ++i) e.Process(1.f, ds);   // one full pass
    const double after = rms();

    CHECK(after < before);          // it eroded
    CHECK(after > 0.0);             // but did not vanish in one pass
    CHECK(std::isfinite(after));
}

TEST_CASE("echo: writeback stays bounded under sustained full scale") {
    static float buf[Flux::kMaxSamples];
    EchoDelay<Flux::kMaxSamples> e;
    e.Init(48000.f, buf);
    e.SetFeedback(1.2f);
    const float ds = 0.1f * 48000.f;
    float peak = 0.f;
    for (int i = 0; i < 480000; ++i) {
        float y = e.Process(1.f, ds, 0.9f);
        peak = std::max(peak, std::fabs(y));
        REQUIRE(std::isfinite(y));
    }
    CHECK(peak < 4.f);
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

#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include "fx/grit.h"
using namespace spky;

static std::vector<float> sine(int n, float amp = 0.5f) {
    std::vector<float> v(n);
    for (int i = 0; i < n; ++i)
        v[i] = amp * std::sin(6.2831853f * 220.f * i / 48000.f);
    return v;
}

TEST_CASE("grit: off leaves the signal bit-exact") {
    Grit g;
    g.init(48000.f);
    auto in = sine(2000);
    for (float s : in) {
        float l = s, r = s;
        g.process(l, r);
        CHECK(l == s);
        CHECK(r == s);
    }
    CHECK(!g.engaged());
}

TEST_CASE("grit: drive distorts, reduce decimates, and they differ") {
    auto in = sine(4800);
    auto run = [&](GritMode m) {
        Grit g;
        g.init(48000.f);
        g.set_mode(m);
        g.set_intensity(0.8f);
        g.set_mix(1.f);
        g.set_on(true, true);
        std::vector<float> out;
        out.reserve(in.size());
        for (float s : in) {
            float l = s, r = s;
            g.process(l, r);
            out.push_back(l);
        }
        return out;
    };
    auto drive = run(GritMode::Drive);
    auto reduce = run(GritMode::Reduce);
    int drive_diff = 0, mode_diff = 0;
    for (size_t i = 0; i < in.size(); ++i) {
        if (std::fabs(drive[i] - in[i]) > 1e-4f) ++drive_diff;
        if (std::fabs(drive[i] - reduce[i]) > 1e-4f) ++mode_diff;
        CHECK(std::fabs(drive[i]) <= 1.5f);
        CHECK(std::fabs(reduce[i]) <= 1.5f);
    }
    CHECK(drive_diff > 1000);
    CHECK(mode_diff > 1000);
}

TEST_CASE("grit: mix 0 returns the dry signal") {
    Grit g;
    g.init(48000.f);
    g.set_intensity(0.9f);
    g.set_mix(0.f);
    g.set_on(true, true);
    auto in = sine(2000);
    for (float s : in) {
        float l = s, r = s;
        g.process(l, r);
        CHECK(l == doctest::Approx(s).epsilon(1e-6));
    }
}

TEST_CASE("grit: switching on mid-signal is click-free") {
    // A click-free ramp must not introduce any sample-to-sample jump larger
    // than the effect already produces in steady state. We run the SAME
    // stimulus and params twice: a baseline that is on from sample 0 (no
    // ramp), and a ramped run that switches on mid-buffer. Comparing their
    // max sample-to-sample deltas isolates the switch transient from the
    // effect's own inherent fuzz. A hard switch would jump ~0.3 (the dry/wet
    // difference), far above baseline; the 4 ms Hann ramp must not.
    auto in = sine(9600);

    Grit g_base;
    g_base.init(48000.f);
    g_base.set_intensity(0.7f);
    g_base.set_mix(1.f);
    g_base.set_on(true, true);   // immediate, on from sample 0 — no ramp
    float prev = 0.f, max_delta_baseline = 0.f;
    for (int i = 0; i < (int)in.size(); ++i) {
        float l = in[i], r = in[i];
        g_base.process(l, r);
        if (i > 0) max_delta_baseline = std::max(max_delta_baseline, std::fabs(l - prev));
        prev = l;
    }

    Grit g_ramp;
    g_ramp.init(48000.f);
    g_ramp.set_intensity(0.7f);
    g_ramp.set_mix(1.f);
    prev = 0.f;
    float max_delta_ramped = 0.f;
    for (int i = 0; i < (int)in.size(); ++i) {
        if (i == 4800) g_ramp.set_on(true);   // ramped, not immediate
        float l = in[i], r = in[i];
        g_ramp.process(l, r);
        if (i > 0) max_delta_ramped = std::max(max_delta_ramped, std::fabs(l - prev));
        prev = l;
    }

    CHECK(max_delta_ramped <= max_delta_baseline + 1e-4f);
}

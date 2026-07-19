#include <doctest/doctest.h>
#include <cmath>
#include <vector>
#include <algorithm>
#include "mod/super_modulator.h"
#include "mod/divisions.h"
#include "parts/part.h"
#include "instrument.h"
#include "render/scenario.h"
using namespace spky;

TEST_CASE("tide: ladder is reciprocal-symmetric with x1 at centre") {
    CHECK(kTideCount == 9);
    CHECK(tide_index(0.5f) == 4);
    CHECK(kTideRatios[4] == 1.f);
    for (int i = 0; i < kTideCount; ++i)
        CHECK(kTideRatios[i] * kTideRatios[kTideCount - 1 - i]
              == doctest::Approx(1.f));
    CHECK(tide_free(0.5f) == 1.f);                    // exakt: 2^0 (IEEE-754)
    CHECK(tide_free(0.f) == doctest::Approx(0.25f));
    CHECK(tide_free(1.f) == doctest::Approx(4.f));
}

TEST_CASE("tide: 0.5 is a bit-identical no-op (free and synced)") {
    for (int synced = 0; synced <= 1; ++synced) {
        SuperModulator a; a.init(48000.f, 42u);
        SuperModulator b; b.init(48000.f, 42u);
        if (synced) {
            a.set_tempo_bpm(120.f); b.set_tempo_bpm(120.f);
            a.set_synced(true);     b.set_synced(true);
        }
        a.set_rate(0.5f); b.set_rate(0.5f);
        b.set_tide(0.5f);
        bool same = true;
        for (int i = 0; i < 48000; ++i) {
            a.process(); b.process();
            for (int s = 0; s < LANE_COUNT; ++s)
                if (a.lane_output(s) != b.lane_output(s)) same = false;
        }
        CHECK(same);
    }
}

TEST_CASE("tide: free scaling drives texture lanes only") {
    SuperModulator m; m.init(48000.f, 42u);
    m.set_rate(0.3f);
    const float base = m.master_hz();
    m.set_tide(1.f);                                  // frei: x4
    CHECK(m.master_hz() == doctest::Approx(base));    // Melodie-Clock steht
    // Texture lanes advance on the 96-sample raster (Task 4, spec
    // 2026-07-19 mod-plane-control-rate): 96 samples give the pitch lane the
    // same elapsed time as the texture lanes' single tick(), so the ratio
    // still reads cleanly on that grid instead of after 1 sample.
    for (int i = 0; i < ModLane::kTickInterval; ++i) m.process();
    float pitch = m.lane_phase(LANE_PITCH);
    CHECK(m.lane_phase(LANE_SOURCE) == doctest::Approx(pitch * 2.00f * 4.f));
    CHECK(m.lane_phase(LANE_SIZE)   == doctest::Approx(pitch * 0.50f * 4.f));
    CHECK(m.lane_phase(LANE_MOTION) == doctest::Approx(pitch * 0.75f * 4.f));
    CHECK(m.lane_phase(LANE_LEVEL)  == doctest::Approx(pitch * 1.50f * 4.f));
}

TEST_CASE("tide: synced snaps to the ratio ladder, free is continuous") {
    SuperModulator m; m.init(48000.f, 1u);
    m.set_tempo_bpm(120.f);
    m.set_tide(0.3f);                                 // frei: 2^(4*(-0.2))
    CHECK(m.tide_mult() == doctest::Approx(std::pow(2.f, -0.8f)));
    m.set_synced(true);                               // rechnet um: Index 2 = x1/2
    CHECK(m.tide_mult() == doctest::Approx(0.5f));
    m.set_tide(0.f);  CHECK(m.tide_mult() == doctest::Approx(0.25f));
    m.set_tide(1.f);  CHECK(m.tide_mult() == doctest::Approx(4.f));
    m.set_tide(0.5f); CHECK(m.tide_mult() == 1.f);
}

TEST_CASE("tide: composes with the center rate_scale") {
    SuperModulator m; m.init(48000.f, 42u);
    m.set_rate(0.3f);
    m.set_rate_scale(1.f, 2.f);                       // COUPLE/DRIFT-Hook
    m.set_tide(1.f);                                  // x4 obendrauf
    // Texture lanes advance on the 96-sample raster (Task 4): read the
    // ratio on that grid (see the comment above for why).
    for (int i = 0; i < ModLane::kTickInterval; ++i) m.process();
    float pitch = m.lane_phase(LANE_PITCH);
    CHECK(m.lane_phase(LANE_SOURCE)
          == doctest::Approx(pitch * 2.f * 2.f * 4.f));
}

TEST_CASE("mod: depth sweep leaves melody and gates bit-identical") {
    auto run = [](float depth, std::vector<float>& cv, std::vector<char>& gate,
                  float& tex_spread) {
        Part p; p.init(48000.f, 5);
        p.set_depth(depth);
        p.set_target_active(LANE_SOURCE, true);
        p.set_target_base(LANE_SOURCE, 0.5f);
        p.mod().set_range(1.f);
        p.mod().set_rate(0.6f);
        float l, r, mn = 1.f, mx = 0.f;
        for (int i = 0; i < 48000; ++i) {
            p.process(l, r);
            cv.push_back(p.pitch_cv());
            gate.push_back(p.gate() ? 1 : 0);
            float t = p.target_value(LANE_SOURCE);
            mn = std::min(mn, t); mx = std::max(mx, t);
        }
        tex_spread = mx - mn;
    };
    std::vector<float> cv0, cv1; std::vector<char> g0, g1;
    float tex0 = 0.f, tex1 = 0.f;
    run(0.f, cv0, g0, tex0);
    run(1.f, cv1, g1, tex1);
    CHECK(cv0 == cv1);                         // Melodie bit-identisch
    CHECK(g0 == g1);                           // Rhythmus bit-identisch
    CHECK(tex0 == doctest::Approx(0.f));       // Textur steht bei MOD 0 ...
    CHECK(tex1 > 0.05f);                       // ... und lebt bei MOD 1
    float cv_min = *std::min_element(cv0.begin(), cv0.end());
    float cv_max = *std::max_element(cv0.begin(), cv0.end());
    CHECK(cv_max - cv_min > 0.01f);            // Melodie moduliert trotz MOD 0
}

TEST_CASE("mod: fx targets still follow the master depth") {
    Part p; p.init(48000.f, 5);
    p.set_fx_target_active(FXT_FLUX_TIME, true);
    p.set_fx_target_base(FXT_FLUX_TIME, 0.5f);
    p.set_depth(0.f);
    p.mod().set_range(1.f);
    float l, r;
    for (int i = 0; i < 5000; ++i) {
        p.process(l, r);
        CHECK(p.fx_target_value(FXT_FLUX_TIME) == doctest::Approx(0.5f));
    }
}

TEST_CASE("range-melody: RANGE narrows the pitch lane, texture stays bit-identical") {
    SuperModulator a; a.init(48000.f, 42u); a.set_rate(0.5f); a.set_range(1.f);
    SuperModulator b; b.init(48000.f, 42u); b.set_rate(0.5f); b.set_range(0.2f);
    bool tex_same = true;
    float amp_a = 0.f, amp_b = 0.f;
    for (int i = 0; i < 48000; ++i) {
        a.process(); b.process();
        for (int s = 0; s < LANE_COUNT; ++s)
            if (s != LANE_PITCH && a.lane_output(s) != b.lane_output(s))
                tex_same = false;
        amp_a = std::max(amp_a, std::fabs(a.lane_output(LANE_PITCH)));
        amp_b = std::max(amp_b, std::fabs(b.lane_output(LANE_PITCH)));
    }
    CHECK(tex_same);            // Textur-Lanes von RANGE unberührt
    CHECK(amp_b < amp_a);       // Ambitus schrumpft mit RANGE
}

TEST_CASE("tide: instrument fans out to both parts; melody stays bit-identical") {
    Instrument a; a.init(48000.f);
    Instrument b; b.init(48000.f);
    b.set_tide(1.f);                                  // frei: x4
    float l = 0.f, r = 0.f, l2 = 0.f, r2 = 0.f;
    bool pitch_same = true, tex_differ = false;
    for (int i = 0; i < 48000; ++i) {
        a.process(nullptr, nullptr, &l, &r, 1);
        b.process(nullptr, nullptr, &l2, &r2, 1);
        for (int p = 0; p < PART_COUNT; ++p) {
            if (a.lane_output(p, LANE_PITCH) != b.lane_output(p, LANE_PITCH))
                pitch_same = false;
            if (a.lane_output(p, LANE_SOURCE) != b.lane_output(p, LANE_SOURCE))
                tex_differ = true;
        }
    }
    CHECK(pitch_same);       // Melodie-Lane unbeeindruckt, beide Parts
    CHECK(tex_differ);       // Textur läuft woanders
}

TEST_CASE("tide: scenario action reaches the instrument") {
    Instrument a; a.init(48000.f);
    Instrument b; b.init(48000.f);
    Event e; e.action = "set_tide"; e.value = 1.f;
    apply_event(b, e);
    float l = 0.f, r = 0.f, l2 = 0.f, r2 = 0.f;
    bool tex_differ = false;
    for (int i = 0; i < 48000; ++i) {
        a.process(nullptr, nullptr, &l, &r, 1);
        b.process(nullptr, nullptr, &l2, &r2, 1);
        if (a.lane_output(0, LANE_SOURCE) != b.lane_output(0, LANE_SOURCE))
            tex_differ = true;
    }
    CHECK(tex_differ);       // der Dispatch beweist sich als Rate-Änderung
}

TEST_CASE("level floor: modulation never ducks LEVEL below 40% of its base") {
    Part p; p.init(48000.f, 5);          // LEVEL target is boot-active
    p.set_depth(1.f);
    p.set_target_base(LANE_LEVEL, 0.8f);
    p.mod().set_shape(0.5f);
    p.mod().set_rate(0.6f);
    float l, r, mn = 1.f;
    for (int i = 0; i < 96000; ++i) {
        p.process(l, r);
        mn = std::min(mn, p.target_value(LANE_LEVEL));
    }
    CHECK(mn >= 0.4f * 0.8f - 1e-6f);    // floor holds ...
    CHECK(mn < 0.5f);                    // ... and actually engaged (full swing)
}

TEST_CASE("level floor: only the LEVEL slot is floored; base 0 stays free") {
    Part p; p.init(48000.f, 5);
    p.set_depth(1.f);
    p.set_target_active(LANE_SIZE, true);
    p.set_target_base(LANE_SIZE, 0.8f);
    p.set_target_depth(LANE_SIZE, 1.f);   // full swing (boot tdepth is 0.55)
    p.mod().set_shape(0.5f);
    p.mod().set_rate(0.6f);
    float l, r, mn_size = 1.f;
    for (int i = 0; i < 96000; ++i) {
        p.process(l, r);
        mn_size = std::min(mn_size, p.target_value(LANE_SIZE));
    }
    CHECK(mn_size < 0.2f);               // texture slots may still dive freely

    Part q; q.init(48000.f, 5);          // a hand-muted part stays mutable
    q.set_depth(0.f);
    q.set_target_base(LANE_LEVEL, 0.f);
    float mx = 0.f;
    for (int i = 0; i < 4000; ++i) {
        q.process(l, r);
        mx = std::max(mx, q.target_value(LANE_LEVEL));
    }
    CHECK(mx == doctest::Approx(0.f));   // floor of base 0 is 0, not 0.4
}

// Boot-targets spec (2026-07-17): modulation first is the shipped state —
// a fresh Part moves all five targets, and MOD 0 stills the texture ones.
TEST_CASE("boot targets: all five slots modulate out of the box") {
    Part p; p.init(48000.f, 5);
    p.set_depth(0.5f);                   // half swing: keeps SIZE/SOURCE off
    p.mod().set_shape(0.5f);             // the [0,1] clamp so the staggered
    p.mod().set_rate(0.6f);              // boot depths stay observable
    float l, r;
    float mn[LANE_COUNT], mx[LANE_COUNT];
    for (int s = 0; s < LANE_COUNT; ++s) { mn[s] = 1.f; mx[s] = 0.f; }
    for (int i = 0; i < 96000; ++i) {
        p.process(l, r);
        for (int s = 0; s < LANE_COUNT; ++s) {
            float t = p.target_value(s);
            mn[s] = std::min(mn[s], t);
            mx[s] = std::max(mx[s], t);
        }
    }
    for (int s = 0; s < LANE_COUNT; ++s)
        CHECK(mx[s] - mn[s] > 0.02f);    // every slot is alive at boot

    // FILTER breathes shallower than TIMBRE (staggered boot depths 0.55 vs 1)
    CHECK((mx[LANE_SIZE] - mn[LANE_SIZE])
          < 0.8f * (mx[LANE_SOURCE] - mn[LANE_SOURCE]));

    Part q; q.init(48000.f, 5);          // MOD 0: texture stands on its bases
    q.set_depth(0.f);
    for (int i = 0; i < 5000; ++i) {
        q.process(l, r);
        CHECK(q.target_value(LANE_SOURCE) == doctest::Approx(0.5f));
        CHECK(q.target_value(LANE_SIZE)   == doctest::Approx(0.5f));
        CHECK(q.target_value(LANE_MOTION) == doctest::Approx(0.5f));
    }
}

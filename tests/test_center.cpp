#include <doctest/doctest.h>
#include <cmath>
#include "center/center.h"
#include "mod/super_modulator.h"
#include "parts/part.h"
using namespace spky;

// Small fixture: two banks + two parts + a Center, all deterministically seeded.
namespace {
struct Rig {
    Center c; SuperModulator a, b; Part pa, pb;
    void init(uint32_t cseed = 123u) {
        c.init(48000.f, cseed);
        a.init(48000.f, 1u); b.init(48000.f, 2u);
        pa.init(48000.f, 1u); pb.init(48000.f, 2u);
    }
    void ticks(int n) { for (int k = 0; k < n; ++k) c.update(a, b, pa, pb); }
};
} // namespace

TEST_CASE("center morph: equal-power law holds across the sweep") {
    Rig r; r.init();
    for (float m = 0.f; m <= 1.001f; m += 0.05f) {
        r.c.set_morph(m);
        r.ticks(600);                                  // settle the smoother
        float ga = r.c.gain_a(), gb = r.c.gain_b();
        CHECK(ga * ga + gb * gb == doctest::Approx(1.f).epsilon(0.005));
    }
}

TEST_CASE("center morph: 0 is full A, 1 is full B") {
    Rig r; r.init();
    r.c.set_morph(0.f); r.ticks(2000);
    CHECK(r.c.gain_a() == doctest::Approx(1.f).epsilon(0.005));
    CHECK(r.c.gain_b() == doctest::Approx(0.f).epsilon(0.005));
    r.c.set_morph(1.f); r.ticks(2000);
    CHECK(r.c.gain_a() == doctest::Approx(0.f).epsilon(0.005));
    CHECK(r.c.gain_b() == doctest::Approx(1.f).epsilon(0.005));
}

TEST_CASE("center morph: smoothing — no click-sized step per control tick after a jump") {
    Rig r; r.init();
    r.c.set_morph(0.f); r.ticks(2000);
    r.c.set_morph(1.f);                                // hard jump
    float prev = r.c.gain_a();
    for (int k = 0; k < 2000; ++k) {
        r.ticks(1);
        float g = r.c.gain_a();
        CHECK(std::fabs(g - prev) < 0.05f);
        prev = g;
    }
}

TEST_CASE("center drift: drift 0 leaves the rate hook at unity, weather at 0") {
    Rig r; r.init(7u);
    r.a.set_rate(0.5f); r.b.set_rate(0.5f);
    float base = r.a.base_hz();
    r.c.set_couple(0.f); r.c.set_drift(0.f);
    r.ticks(5000);
    CHECK(r.a.master_hz() == doctest::Approx(base));
    CHECK(r.c.weather()   == doctest::Approx(0.f));
}

TEST_CASE("center drift: weather stays in (-1,1) and mean-reverts around 0") {
    Rig r; r.init(999u);
    r.c.set_drift(1.f);
    double sum = 0; int n = 0;
    for (int k = 0; k < 40000; ++k) {
        r.ticks(1);
        CHECK(r.c.weather() >  -1.f);
        CHECK(r.c.weather() <   1.f);
        sum += r.c.weather(); ++n;
    }
    CHECK(std::fabs(sum / n) < 0.5);
}

TEST_CASE("center drift: deterministic per seed") {
    auto run = [](uint32_t seed) {
        Rig r; r.init(seed);
        r.c.set_drift(1.f);
        float last = 0.f;
        for (int k = 0; k < 3000; ++k) { r.ticks(1); last = r.c.weather(); }
        return last;
    };
    CHECK(run(42u) == run(42u));
    CHECK(run(42u) != run(43u));
}

TEST_CASE("center drift: at full drift the two banks' rates move apart (opposite polarity)") {
    Rig r; r.init(321u);
    r.a.set_rate(0.5f); r.b.set_rate(0.5f);
    r.c.set_drift(1.f);
    bool a_moved = false, b_moved = false;
    float ba = r.a.base_hz(), bb = r.b.base_hz();
    for (int k = 0; k < 6000; ++k) {
        r.ticks(1);
        if (std::fabs(r.a.master_hz() - ba) > 1e-3f) a_moved = true;
        if (std::fabs(r.b.master_hz() - bb) > 1e-3f) b_moved = true;
    }
    CHECK(a_moved);
    CHECK(b_moved);
}

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

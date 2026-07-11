#include <doctest/doctest.h>
#include <cmath>
#include <vector>
#include "synth/morph_osc.h"
#include "util/fast_sin.h"
using namespace spky;

static int rising_crossings(const std::vector<float>& v) {
    int n = 0;
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i - 1] <= 0.f && v[i] > 0.f) ++n;
    return n;
}

static std::vector<float> run_osc(float freq, float morph, float detune_ct, int n) {
    MorphOsc o;
    o.init(48000.f);
    o.set_morph(morph);
    o.set_detune_cents(detune_ct);
    o.set_freq(freq);
    std::vector<float> out(n);
    for (auto& s : out) s = o.process();
    return out;
}

TEST_CASE("morph_osc: frequency accuracy via zero crossings at every anchor shape") {
    for (float m : { 0.f, 1.f / 3.f, 2.f / 3.f, 1.f }) {
        auto v = run_osc(220.f, m, 0.f, 48000 * 5);
        int n = rising_crossings(v);
        CHECK(n >= 220 * 5 - 3);
        CHECK(n <= 220 * 5 + 3);
    }
}

TEST_CASE("morph_osc: anchor shapes match the ideal waveforms away from blep edges") {
    // 100 Hz -> exactly 480 samples per cycle; sample i sits at phase (i+1)/480.
    const int cyc = 480;
    auto sine = run_osc(100.f, 0.f, 0.f, cyc);
    auto tri  = run_osc(100.f, 1.f / 3.f, 0.f, cyc);
    auto saw  = run_osc(100.f, 2.f / 3.f, 0.f, cyc);
    auto pul  = run_osc(100.f, 1.f, 0.f, cyc);
    for (int i = 0; i < cyc - 1; ++i) {
        float t = (i + 1) / static_cast<float>(cyc);
        CHECK(sine[i] == doctest::Approx(fast_sin(t)).epsilon(0.01));
        float tri_ref = t < 0.25f ? 4.f * t : (t < 0.75f ? 2.f - 4.f * t : 4.f * t - 4.f);
        CHECK(tri[i] == doctest::Approx(tri_ref).epsilon(0.02));
        bool near_wrap = t < 0.02f || t > 0.98f;              // polyblep regions
        if (!near_wrap)
            CHECK(saw[i] == doctest::Approx(2.f * t - 1.f).epsilon(0.02));
        bool near_step = near_wrap || std::fabs(t - 0.5f) < 0.02f;
        if (!near_step)
            CHECK(pul[i] == doctest::Approx(t < 0.5f ? 1.f : -1.f).epsilon(0.02));
    }
}

TEST_CASE("morph_osc: output bounded across the full morph sweep") {
    for (int mi = 0; mi <= 20; ++mi) {
        auto v = run_osc(880.f, mi / 20.f, 0.f, 9600);
        for (float s : v) {
            CHECK(s >= -1.05f);
            CHECK(s <=  1.05f);
        }
    }
}

TEST_CASE("morph_osc: detune in cents shifts frequency by the expected ratio") {
    auto base = run_osc(220.f, 0.f, 0.f, 48000 * 10);
    auto det  = run_osc(220.f, 0.f, 50.f, 48000 * 10);
    int n0 = rising_crossings(base);
    int n1 = rising_crossings(det);
    // 50 ct -> ratio 2^(50/1200) = 1.02930 -> 226.45 Hz -> ~64 extra cycles / 10 s
    // (this cycle-count difference IS the beat frequency between the two).
    CHECK(n1 - n0 >= 58);
    CHECK(n1 - n0 <= 70);
}

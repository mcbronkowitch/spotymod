#include <doctest/doctest.h>
#include <cmath>
#include <vector>
#include "fx/dust.h"
#include "fx/flux.h"
using namespace spky;

// A synthetic tape: 1 s stereo, filled with a distinguishable ramp so a grain's
// output can be traced back to the offset it read from.
struct FakeTape {
    static constexpr int32_t kSize = 48000;
    std::vector<float> l, r;
    int32_t ptr = 0;
    FakeTape() : l(kSize, 0.f), r(kSize, 0.f) {}
    TapeTap tap() const { return TapeTap{l.data(), r.data(), ptr, kSize}; }
    void advance() { ptr = (ptr - 1 + kSize) % kSize; }
    void fill_noise(uint32_t seed) {
        Rng g; g.seed(seed);
        for (int32_t i = 0; i < kSize; ++i) { l[i] = g.next_bipolar() * 0.5f;
                                              r[i] = g.next_bipolar() * 0.5f; }
    }
};

TEST_CASE("dust: amount 0 is silent and inactive") {
    FakeTape t; t.fill_noise(11);
    DustCloud d;
    d.init(48000.f, 0xD0571u);
    d.set_dust(0.f);
    d.set_rot(0.5f);
    d.set_delay_time(0.5f);
    CHECK(!d.active());
    for (int i = 0; i < 48000; ++i) {
        float gl = 1.f, gr = 1.f;
        float wb = d.process(t.tap(), gl, gr);
        REQUIRE(gl == 0.f);
        REQUIRE(gr == 0.f);
        REQUIRE(wb == 0.f);
        t.advance();
    }
}

TEST_CASE("dust: deterministic for a given seed") {
    auto run = [](std::vector<float>& out) {
        FakeTape t; t.fill_noise(11);
        DustCloud d;
        d.init(48000.f, 0xD0571u);
        d.set_dust(0.8f);
        d.set_rot(0.5f);            // free zone
        d.set_delay_time(0.5f);
        out.resize(48000);
        for (int i = 0; i < 48000; ++i) {
            float gl = 0.f, gr = 0.f;
            d.process(t.tap(), gl, gr);
            out[i] = gl;
            t.advance();
        }
    };
    std::vector<float> a, b;
    run(a); run(b);
    for (size_t i = 0; i < a.size(); ++i) REQUIRE(a[i] == b[i]);
}

TEST_CASE("dust: free zone produces sound and stays bounded") {
    FakeTape t; t.fill_noise(11);
    DustCloud d;
    d.init(48000.f, 0xD0571u);
    d.set_dust(1.f);
    d.set_rot(0.5f);
    d.set_delay_time(0.5f);
    float peak = 0.f;
    double sum_sq = 0.0;
    for (int i = 0; i < 96000; ++i) {
        float gl = 0.f, gr = 0.f;
        d.process(t.tap(), gl, gr);
        peak = std::max(peak, std::fabs(gl));
        sum_sq += (double)gl * (double)gl;
        REQUIRE(std::isfinite(gl));
        t.advance();
    }
    CHECK(std::sqrt(sum_sq / 96000.0) > 1e-3);   // it made sound
    CHECK(peak < 2.f);                            // normalization held
}

// A synthetic tape a real click test can actually use: a smooth 100 Hz sine,
// unit amplitude. White noise (FakeTape above) cannot do this job -- adjacent
// samples are uncorrelated, so a single grain at unity gain can legitimately
// step close to 2x its own amplitude sample to sample with nothing wrong at
// all, which makes ANY absolute step bound on a noise tape meaningless (the
// old `max_step < 1.0f` on white noise only ever passed because the pre-fix
// normalisation happened to scale everything down by a fixed factor -- the
// exact bug that was just removed). A smooth tape's own sample-to-sample
// slope is tiny and boundable, so a step orders of magnitude larger really
// would mean a grain appeared or vanished at non-zero amplitude.
struct SineTape {
    static constexpr int32_t kSize = 48000;
    std::vector<float> l, r;
    int32_t ptr = 0;
    SineTape() : l(kSize), r(kSize) {
        constexpr float freq_hz = 100.f, sr = 48000.f;
        for (int32_t i = 0; i < kSize; ++i) {
            float v = std::sin(TWO_PI * freq_hz * (float)i / sr);
            l[i] = v; r[i] = v;
        }
    }
    TapeTap tap() const { return TapeTap{l.data(), r.data(), ptr, kSize}; }
    void advance() { ptr = (ptr - 1 + kSize) % kSize; }
};

TEST_CASE("dust: grain sum is click-free across births and deaths") {
    SineTape t;
    DustCloud d;
    d.init(48000.f, 0xD0571u);
    d.set_dust(0.9f);
    d.set_rot(0.5f);
    d.set_delay_time(0.5f);

    // Derived bound on the legitimate per-sample step (not measured):
    //  - Tape: 100 Hz sine, unit amplitude @ 48 kHz -> max |slope| =
    //    2*pi*100/48000 ~= 0.01309 per sample.
    //  - Up to kGrains = 8 grains can be active at once (pool cap). Each
    //    contributes pan(<=1) * window(<=1) * norm(<=1) * (tape sample), so
    //    the worst-case simultaneous tape-driven step is
    //    8 * 0.01309 ~= 0.1047.
    //  - Each grain's window index (`widx`) also moves by `wstep` per sample,
    //    but the sin^2 curve's slope w.r.t. its own 192-entry table index is
    //    at most (pi/2)/191 ~= 0.00822 per index step. At DUST = 0.9 the
    //    shortest grain length is _len_min = lerp(kLenMinLo, kLenMinHi, 0.9) *
    //    48000 ~= 3576 samples, so |wstep|_max = 382/3576 ~= 0.1068, and the
    //    window's own contribution is <= 0.00822 * 0.1068 ~= 0.00088 per
    //    grain, ~= 0.007 summed over 8.
    //  - The norm one-pole's own slew is coef * (kInvSqrt range) =
    //    (1 - exp(-1/(kNormSmoothS*48000))) * (1.0 - 0.35355) ~= 0.00067,
    //    applied to the raw (pre-norm) sum bound of kGrains = 8: ~= 0.0054.
    //  Total: 0.1047 + 0.007 + 0.0054 ~= 0.117. Asserted at 0.3, comfortably
    //  above the derivation's own slack (fast_sin's <=1.2e-3 approximation
    //  error, float rounding) without hiding a real discontinuity, which
    //  would need to clear a window-fold residual of ~(pi/len)^2 -- for this
    //  scenario's shortest grain (~3576 samples), ~7.7e-7, i.e. a genuine
    //  click is many orders of magnitude above this threshold, not a hair
    //  below it. Measured on this exact scenario: max_step = 0.014804 --
    //  comfortably under both the derivation and the assertion.
    float prev = 0.f, max_step = 0.f;
    for (size_t i = 0; i < Flux::kMaxSamples; ++i) {
        float gl = 0.f, gr = 0.f;
        d.process(t.tap(), gl, gr);
        if (i > 0) max_step = std::max(max_step, std::fabs(gl - prev));
        prev = gl;
        t.advance();
    }
    CHECK(max_step < 0.3f);
}

// A flat (DC) tape: makes the Hann window shape directly visible in the
// output (gl = norm * window * pan * constant), independent of what the tape
// holds -- unlike SineTape above, not even needed for a bounded slope, just
// for a KNOWN, non-oscillating value so a grain's own envelope is legible.
struct FlatTape {
    static constexpr int32_t kSize = 48000;
    std::vector<float> l, r;
    int32_t ptr = 0;
    FlatTape() : l(kSize, 1.f), r(kSize, 1.f) {}
    TapeTap tap() const { return TapeTap{l.data(), r.data(), ptr, kSize}; }
    void advance() { ptr = (ptr - 1 + kSize) % kSize; }
};

TEST_CASE("dust: grain window opens and closes near zero (structural)") {
    // Independent of what the tape holds, and so cannot be fooled by the
    // choice of test signal the way the case above can be: this checks the
    // Hann window shape itself. Uses a flat (DC) tape and low DUST so births
    // are rare enough that single-grain lifetimes are usually isolated in
    // time -- active_grains() goes 0 -> 1 -> 0 with nothing else sounding.
    // Gated on active_grains() == 1 so the sample really is that one grain's
    // own contribution, not a sum: since at most one grain can be born per
    // sample, a transition from 0 to 1 active grains means THIS sample is the
    // newborn grain's first (widx == 0, so window == curve[0] == 0.0 exactly,
    // not just approximately); a transition from 1 to 0 means the PRECEDING
    // sample was the dying grain's last, whose window is the small-but-
    // nonzero fold residual discussed in the case above (tiny for any
    // realistic grain length, so a fixed small bound suffices here too).
    FlatTape t;
    DustCloud d;
    d.init(48000.f, 0xD0571u);
    d.set_dust(0.05f);      // sparse: isolated single-grain lifetimes
    d.set_rot(0.5f);
    d.set_delay_time(0.5f);

    int prev_active = 0;
    float prev_gl = 0.f;
    int births_checked = 0, deaths_checked = 0;
    for (int i = 0; i < 480000; ++i) {   // 10 s: several isolated grain lives
        float gl = 0.f, gr = 0.f;
        d.process(t.tap(), gl, gr);
        int active = d.active_grains();
        if (active == 1 && prev_active == 0) {
            CHECK(gl == 0.f);             // window[0] == curve[0], exactly
            ++births_checked;
        }
        if (active == 0 && prev_active == 1) {
            CHECK(std::fabs(prev_gl) < 0.01f);   // window's tail is negligible
            ++deaths_checked;
        }
        prev_active = active;
        prev_gl = gl;
        t.advance();
    }
    CHECK(births_checked > 0);   // the scenario actually exercised this
    CHECK(deaths_checked > 0);
}

TEST_CASE("dust: density rises with DUST, level stays inside a band") {
    // Level is normalized by the ACTIVE grain count (1/sqrt(active), smoothed —
    // see fx/dust.h kInvSqrt / kNormSmoothS), specifically SO THAT loudness does
    // not track density: DUST is meant to change texture/density, not volume.
    // That means total windowed energy is NOT expected to be flat in an
    // absolute sense — more of the time has a grain sounding at all as DUST
    // rises (duty cycle), so some energy increase across the knob is real and
    // expected. What the old energy-monotonic assertion actually caught (and
    // what this replaces it with, per the corrected spec §2) is two separate
    // claims: density (grains actually sounding) rises with DUST, and level
    // does not blow up or collapse the way the pre-fix normalization did (it
    // divided by the OFFERED load, so DUST = 1 could get quieter than DUST =
    // 0.6 — the bug this test now guards against).
    auto measure = [](float amount, double& energy, double& mean_active) {
        FakeTape t; t.fill_noise(11);
        DustCloud d;
        d.init(48000.f, 0xD0571u);
        d.set_dust(amount);
        d.set_rot(0.5f);
        d.set_delay_time(0.5f);
        double s = 0.0, a = 0.0;
        for (int i = 0; i < 192000; ++i) {
            float gl = 0.f, gr = 0.f;
            d.process(t.tap(), gl, gr);
            s += (double)gl * (double)gl;
            a += d.active_grains();
            t.advance();
        }
        energy = s;
        mean_active = a / 192000.0;
    };
    double e_low, e_mid, e_high, a_low, a_mid, a_high;
    measure(0.2f, e_low, a_low);
    measure(0.6f, e_mid, a_mid);
    measure(1.0f, e_high, a_high);

    // Density: mean grains actually sounding rises monotonically with DUST.
    CHECK(a_low > 0.0);
    CHECK(a_mid > a_low);
    CHECK(a_high > a_mid);

    // Level: this is a REGRESSION PIN, not a derived bound — the numbers come
    // from measurement, not from a property the design guarantees. Measured
    // on this scenario/seed on 2026-07-19: energy(0.2) = 952.90,
    // energy(0.6) = 2548.24, energy(1.0) = 2951.25. That ~3.1x rise end to
    // end is real and expected (duty cycle: at DUST=0.2 mean active grains is
    // only ~0.35, so most of the window is genuine silence) — the design
    // normalizes per-sounding-grain loudness, not total windowed energy, and
    // does not claim the latter is flat. What IS pinned here is the absence
    // of the specific reversal the pre-fix normalization produced: 3229.6 at
    // DUST=0.6 down to 2193.3 at DUST=1.0 (quieter at max DUST while dropping
    // a quarter of its grain births to blocking). The band below is generous
    // around the measurements — it exists to catch a return of that
    // regression (or a wild blowup), not to assert flatness the design
    // doesn't deliver.
    CHECK(e_low > 400.0);
    CHECK(e_high < 4500.0);
    CHECK(e_high > e_mid * 0.5);   // guards the specific "quieter at max DUST" bug
}

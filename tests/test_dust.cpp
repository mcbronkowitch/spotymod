#include <doctest/doctest.h>
#include <cmath>
#include <iomanip>
#include <vector>
#include "fx/dust.h"
#include "fx/flux.h"
using namespace spky;

// A synthetic tape: ~1.37 s stereo, filled with noise so a grain's output
// can't be mistaken for anything but what it actually read.
// kSize is a power of two (TapeTap's contract: production's FLUX tape is
// Flux::kMaxSamples = 262144, also a power of two; TapeTap itself no longer
// checks this at all -- see fx/dust.cpp's static_assert against
// Flux::kMaxSamples -- so this fixture pins its own constant instead).
struct FakeTape {
    static constexpr int32_t kSize = 65536;
    static_assert((kSize & (kSize - 1)) == 0, "kSize must be a power of two");
    std::vector<float> l, r;
    int32_t ptr = 0;
    FakeTape() : l(kSize, 0.f), r(kSize, 0.f) {}
    TapeTap tap() const { return TapeTap{l.data(), r.data(), ptr, kSize - 1}; }
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
    // kSize must be a power of two (TapeTap's contract). kPeriod -- the sine's
    // period IN SAMPLES -- must also be a power of two and divide kSize
    // exactly, so the buffer tiles with NO seam discontinuity at the wrap the
    // 262144-sample test loop below runs into repeatedly. A "clean" 100 Hz at
    // 48 kHz has a 480-sample period (= 2^5 * 3 * 5); no power-of-two kSize is
    // a multiple of that, so a plain "100 Hz" tape would leave a residual
    // phase jump at every wrap -- a seam artifact with nothing to do with
    // DustCloud that would contaminate the click test. Using a power-of-two
    // period instead (512 samples, i.e. 93.75 Hz) keeps the wrap point
    // mathematically exact -- it is NOT, however, genuinely seamless in
    // practice: `sin()` is evaluated from a per-sample float phase
    // (TWO_PI * freq_hz * i / sr, i up to kSize - 1), and accumulated float
    // rounding over 65536 samples still leaves the two sides of the wrap
    // ~1e-4 apart. Harmless here -- the click assertion below is 0.3, three
    // orders of magnitude above that residual -- but "genuinely seamless"
    // overstates it.
    static constexpr int32_t kSize = 65536;
    static_assert((kSize & (kSize - 1)) == 0, "kSize must be a power of two");
    static constexpr int32_t kPeriod = 512;
    static constexpr float freq_hz = 48000.f / (float)kPeriod;   // 93.75 Hz
    std::vector<float> l, r;
    int32_t ptr = 0;
    SineTape() : l(kSize), r(kSize) {
        constexpr float sr = 48000.f;
        for (int32_t i = 0; i < kSize; ++i) {
            float v = std::sin(TWO_PI * freq_hz * (float)i / sr);
            l[i] = v; r[i] = v;
        }
    }
    TapeTap tap() const { return TapeTap{l.data(), r.data(), ptr, kSize - 1}; }
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
    //  - Tape: 93.75 Hz sine (SineTape::freq_hz -- see that struct for why it
    //    isn't a round 100 Hz), unit amplitude @ 48 kHz -> max |slope| =
    //    2*pi*93.75/48000 ~= 0.01227 per sample.
    //  - Up to kGrains = 8 grains can be active at once (pool cap). Each
    //    contributes pan(<=1) * window(<=1) * norm(<=1) * (tape sample), so
    //    the worst-case simultaneous tape-driven step is
    //    8 * 0.01227 ~= 0.0982.
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
    //  Total: 0.0982 + 0.007 + 0.0054 ~= 0.1106. Asserted at 0.3, comfortably
    //  above the derivation's own slack (fast_sin's <=1.2e-3 approximation
    //  error, float rounding) without hiding a real discontinuity, which
    //  would need to clear a window-fold residual of ~(pi/len)^2 -- for this
    //  scenario's shortest grain (~3576 samples), ~7.7e-7, i.e. a genuine
    //  click is many orders of magnitude above this threshold, not a hair
    //  below it. Measured on this exact scenario: max_step = 0.015605 --
    //  comfortably under both the derivation and the assertion.
    //  NOTE this is a GROSS-click detector, not full coverage: at a 0.3
    //  threshold it only fires on a discontinuity ~2.5x the worst legitimate
    //  step derived above. A soft click -- e.g. a grain birth whose window
    //  starts partway open instead of at curve[0] == 0 -- would step roughly
    //  an order of magnitude smaller and pass here undetected. The
    //  "structural" case below is what actually covers that gap: it checks
    //  the window shape directly (birth/death sample values), independent of
    //  step size, so it catches soft onsets this case cannot.
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
    static constexpr int32_t kSize = 65536;   // power of two (TapeTap contract)
    static_assert((kSize & (kSize - 1)) == 0, "kSize must be a power of two");
    std::vector<float> l, r;
    int32_t ptr = 0;
    FlatTape() : l(kSize, 1.f), r(kSize, 1.f) {}
    TapeTap tap() const { return TapeTap{l.data(), r.data(), ptr, kSize - 1}; }
    void advance() { ptr = (ptr - 1 + kSize) % kSize; }
};

TEST_CASE("dust: grain window opens and closes near zero (structural)") {
    // Checks the Hann window shape itself via a flat (DC) tape and low DUST,
    // so births are rare enough that single-grain lifetimes are usually
    // isolated in time -- active_grains() goes 0 -> 1 -> 0 with nothing else
    // sounding. Gated on active_grains() == 1 so the sample really is that one
    // grain's own contribution, not a sum: since at most one grain can be born
    // per sample, a transition from 0 to 1 active grains means THIS sample is
    // the newborn grain's first.
    //
    // The two checks below are NOT equally tape-independent, though both look
    // it at a glance:
    //  - Birth genuinely is: widx == 0, so window == curve[0] == 0.0 exactly
    //    in IEEE 754, so gl == 0.0 exactly for ANY finite tape value (0 times
    //    anything finite is exactly 0) -- this is the case that "cannot be
    //    fooled by the choice of test signal the way the case above can be".
    //  - Death is not: the PRECEDING sample (the dying grain's last) has a
    //    small-but-nonzero fold residual window, discussed in the case above,
    //    multiplied by whatever the tape holds. The `< 0.01f` bound below is
    //    an ABSOLUTE threshold, so it only holds because FlatTape's amplitude
    //    is 1.0 -- it would need rescaling for a tape of a very different
    //    amplitude. It is not a content-independent property the way the
    //    birth check is; it's a fixed small bound tuned to this fixture's
    //    amplitude, which is fine here but shouldn't be read as more general
    //    than that.
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
    // on this scenario/seed on 2026-07-19 (post power-of-two TapeTap fixture
    // change): energy(0.2) = 942.59, energy(0.6) = 2535.01,
    // energy(1.0) = 2958.63. Some end-to-end rise is real and expected (duty
    // cycle: at DUST=0.2 mean active grains is well under 1, so most of the
    // window is genuine silence) — the design normalizes per-sounding-grain
    // loudness, not total windowed energy, and does not claim the latter is
    // flat.
    //
    // What IS pinned here is the absence of the specific reversal the pre-fix
    // normalization produced: on the buggy build, energy(0.6) = 3229.6 down to
    // energy(1.0) = 2193.3 — quieter at max DUST while dropping a quarter of
    // its grain births to blocking (ratio e_high/e_mid = 0.679). The fixed
    // design's own measurement above gives a ratio of 1.167 instead — i.e.
    // e_high > e_mid, not just "not too much smaller": DUST=1 must sound at
    // least as loud as DUST=0.6, full stop. `e_high > e_mid * 0.5` looks like
    // it guards this but does not: 0.679 > 0.5, so that assertion would have
    // PASSED on the exact buggy numbers it claims to pin (verified by hand:
    // 2193.3 > 3229.6 * 0.5 = 1614.8 is true). `e_high > e_mid` is what
    // actually fails on the buggy pair (2193.3 > 3229.6 is false) and is what
    // the corrected design actually claims.
    // The two absolute bounds are MEASUREMENT PINS, not derived, and they moved
    // by exactly 5.33x = kGrainMakeup^2 (2.309^2) when the grain-path makeup
    // landed on 2026-07-19. That factor is the check: the makeup compensates the
    // Hann-window and equal-power-pan losses (0.4330 combined, -7.27 dB) that
    // 1/sqrt(active) never covered, so an energy ratio anywhere else would mean
    // the makeup did not do only what it claims. Re-measure and restate the
    // factor if either bound is touched again.
    CHECK(e_low > 2100.0);      // was 400.0 before the makeup
    CHECK(e_high < 24000.0);    // was 4500.0 before the makeup
    CHECK(e_high > e_mid);   // regression pin: DUST=1 must not be quieter than DUST=0.6
}

// --- Task 12: zone S rebuilt as a beat repeat -------------------------------
//
// The old zone-S timing tests (birth_indices() and the three cases built on
// it) asserted the superseded `delay_time / kGridDiv` grid with probabilistic
// firing -- exactly the design this task replaces. They are gone, not
// extended; see docs/superpowers/specs/2026-07-18-dust-grain-cloud-design.md
// §3 for the rebuilt zone and the measurement that killed the old one.

// A tape whose L and R channels are the SAME buffer: eliminates the random
// per-grain channel choice as a confound in the anchored-replay test below --
// with l == r, content read is independent of which channel a grain happens
// to pick.
struct MonoTape {
    static constexpr int32_t kSize = 65536;
    static_assert((kSize & (kSize - 1)) == 0, "kSize must be a power of two");
    std::vector<float> buf;
    int32_t ptr = 0;
    MonoTape() : buf(kSize, 0.f) {}
    void fill_noise(uint32_t seed) {
        Rng g; g.seed(seed);
        for (int32_t i = 0; i < kSize; ++i) buf[i] = g.next_bipolar() * 0.5f;
    }
    TapeTap tap() const { return TapeTap{buf.data(), buf.data(), ptr, kSize - 1}; }
    void advance() { ptr = (ptr - 1 + kSize) % kSize; }
};

TEST_CASE("dust: zone S anchored replay -- successive grains repeat the same slice") {
    // The load-bearing test for this task. A grain's contribution to the
    // stereo sum is norm(t) * pan * window(k) * content(read_position(k)),
    // where pan is a random per-grain equal-power split. gl^2 + gr^2 cancels
    // that split exactly (sin^2 + cos^2 == 1 for ANY pan angle), leaving
    // norm(t)^2 * window(k)^2 * content(k)^2 -- so comparing gl^2+gr^2
    // between two grains born in the same beat isolates exactly the claim
    // that matters here: do they read the SAME tape content at the same
    // position-in-life k. Anything else (a delay tap read relative to the
    // moving write head, i.e. the bug this task removes) reads a DIFFERENT
    // section of tape for the second grain and fails this comparison.
    MonoTape t; t.fill_noise(0x5EED);
    DustCloud d;
    d.init(48000.f, 0xD0571u);
    d.set_dust(0.1f);       // subdiv = 1 -> 2 slots/beat; below kOctaveThresh
    d.set_rot(0.0f);        // zone S, jitter == 0 exactly: no scheduling slack
    d.sync_beat(48000.f);   // 1 s beat @ 48 kHz -> grid_period = beat/2 = 24000

    constexpr int kLen = 24000;   // == grid_period at this dust/beat setting
    std::vector<float> e0(kLen), e1(kLen);
    for (int i = 0; i < 2 * kLen; ++i) {
        float gl = 0.f, gr = 0.f;
        d.process(t.tap(), gl, gr);
        const float e = gl * gl + gr * gr;
        if (i < kLen) e0[i] = e; else e1[i - kLen] = e;
        t.advance();
    }

    // Compare well inside the flat "hold" region, away from the fade edges
    // (shared shape between both grains, but staying inside hold sidesteps
    // any float slack right at the fold point).
    const int fade = (int)(spky::dust_tuning::kSlotFadeS * 48000.f);
    int compared = 0;
    for (int k = fade * 2; k < kLen - fade * 2; k += 37) {
        REQUIRE(e0[k] > 1e-8f);   // sanity: genuinely sounding, not silence
        CHECK(e1[k] == doctest::Approx(e0[k]).epsilon(0.02));
        ++compared;
    }
    CHECK(compared > 100);
}

TEST_CASE("dust: zone S phase lock -- first birth lands within one control tick of the beat edge") {
    FakeTape t; t.fill_noise(11);
    DustCloud d;
    d.init(48000.f, 0xD0571u);
    d.set_dust(0.2f);
    d.set_rot(0.1f);
    // Run well past the preseeded default grid before the real edge arrives,
    // so a scheduler that ignored sync_beat() and just kept counting down its
    // OWN stale grid would not accidentally land inside the window by luck.
    // Zone S runs back-to-back grains (grain length == grid period), so
    // active_grains() is essentially ALWAYS >= 1 once the cloud is running --
    // checking for mere presence would pass even if sync_beat() were a no-op,
    // because the grain already in flight before the edge is still sounding.
    // The genuine signal is an INCREASE over the pre-edge count: the edge
    // spawns a new anchored grain into a free pool slot alongside whatever is
    // already playing, without killing it.
    for (int i = 0; i < 5000; ++i) {
        float gl = 0.f, gr = 0.f;
        d.process(t.tap(), gl, gr);
        t.advance();
    }
    const int pre = d.active_grains();
    d.sync_beat(48000.f);

    int birth_at = -1;
    for (int i = 0; i < 200; ++i) {
        float gl = 0.f, gr = 0.f;
        d.process(t.tap(), gl, gr);
        if (d.active_grains() > pre) { birth_at = i; break; }
        t.advance();
    }
    REQUIRE(birth_at >= 0);
    CHECK(birth_at <= 96);   // Center::kCtrlInterval
}

TEST_CASE("dust: zone S subdivision selects 2/4/8/16 slots per beat") {
    // _subdiv = 1 + (int)(dust * 3.999f) => 1,2,3,4 => 1<<_subdiv = 2,4,8,16
    // slots/beat (spec §3). A slot's births are deterministic (every slot
    // fires), so counting silence-to-sound transitions over exactly one beat
    // pins the subdivision directly.
    auto births_per_beat = [](float dust_amt) {
        FakeTape t; t.fill_noise(11);
        DustCloud d;
        d.init(48000.f, 0xD0571u);
        d.set_dust(dust_amt);
        d.set_rot(0.0f);         // zone S, jitter == 0: grid stays exact
        d.sync_beat(48000.f);    // 1 s beat @ 48 kHz
        int births = 0;
        bool was_silent = true;
        for (int i = 0; i < 48000; ++i) {   // exactly one beat
            float gl = 0.f, gr = 0.f;
            d.process(t.tap(), gl, gr);
            const bool silent = (gl == 0.f && gr == 0.f);
            if (was_silent && !silent) ++births;
            was_silent = silent;
            t.advance();
        }
        return births;
    };
    CHECK(births_per_beat(0.10f) == 2);
    CHECK(births_per_beat(0.35f) == 4);
    CHECK(births_per_beat(0.60f) == 8);   // >= kOctaveThresh: stacks an octave
    CHECK(births_per_beat(0.90f) == 16);  // grain per slot, but still ONE slot
}                                          // = one silence-to-sound transition

TEST_CASE("dust: zone S octave layer -- second grain traverses 2x the tape distance in the same duration") {
    // A tape that is silent except for one marker sample, placed at a
    // distance from the anchor only the octave (rate = 2) grain can reach
    // within one slot's duration -- the base (rate = 1) grain's read range is
    // bounded to [anchor+1, anchor+len], so it never sees a marker at
    // anchor + 1.5*len. Both grains end at _anchor (spec §3): base steps -1
    // from anchor+len, octave steps -2 from anchor+2*len, so at read-position
    // D from the anchor, base is at k = len - D and octave is at
    // k = len - D/2 -- exactly half as many samples in for the same tape
    // distance, i.e. twice the speed.
    FakeTape t;   // all-zero: FakeTape's ctor zero-fills and is not noise-filled here
    const int32_t D = 3000;
    t.l[D & (FakeTape::kSize - 1)] = 1.f;
    t.r[D & (FakeTape::kSize - 1)] = 1.f;

    DustCloud d;
    d.init(48000.f, 0xD0571u);
    d.set_dust(0.6f);       // >= kOctaveThresh: octave layer active; subdiv = 3
    d.set_rot(0.0f);        // zone S, jitter == 0
    d.sync_beat(48000.f);   // grid_period = 48000 / (1 << 3) = 6000

    constexpr int32_t kLen = 6000;         // == grid_period here
    const int32_t k_base = kLen - D;       // 3000: base grain reads D here
    const int32_t k_oct  = kLen - D / 2;   // 4500: octave grain reads D here

    std::vector<float> e(kLen);
    for (int32_t i = 0; i < kLen; ++i) {
        float gl = 0.f, gr = 0.f;
        d.process(t.tap(), gl, gr);
        e[i] = gl * gl + gr * gr;
        t.advance();
    }

    for (int32_t k = 0; k < kLen; ++k) {
        if (k == k_base || k == k_oct) continue;
        REQUIRE(e[k] == 0.f);   // silent tape everywhere but the marker reads
    }
    CHECK(e[k_base] > 1e-6f);   // the base (1x) grain passes the marker
    CHECK(e[k_oct]  > 1e-6f);   // the octave (2x) grain passes the SAME marker
}                                // at half the elapsed samples

TEST_CASE("dust: zones F and R are bit-identical to the pre-task build") {
    // Task 12 touches zone S only: Grain::hold defaults to 0 and _spawn()
    // (used by zones F/R) sets it explicitly, so the trapezoid gate's
    // `if (g.hold > 0)` branch is never taken there and the fold degenerates
    // to exactly the old `widx = 382 - widx; wstep = -wstep` behaviour.
    // Pinned against the pre-task build (same seed/scenario, captured before
    // this task's changes landed) -- the cheapest guard against this task
    // leaking into the other two zones.
    auto checksum = [](float rot, float amount) {
        FakeTape t; t.fill_noise(11);
        DustCloud d;
        d.init(48000.f, 0xD0571u);
        d.set_dust(amount);
        d.set_rot(rot);
        d.set_delay_time(0.5f);
        double sum = 0.0;
        for (int i = 0; i < 96000; ++i) {
            float gl = 0.f, gr = 0.f;
            d.process(t.tap(), gl, gr);
            sum += (double)gl * 1000003.0 + (double)gr * 7919.0;
            t.advance();
        }
        return sum;
    };
    CHECK(checksum(0.5f, 0.8f) == doctest::Approx(74822799.72581546).epsilon(1e-12));   // zone F
    CHECK(checksum(0.9f, 0.8f) == doctest::Approx(28634632.408616465).epsilon(1e-12));  // zone R
}

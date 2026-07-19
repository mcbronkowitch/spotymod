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
    f.init(48000.f, s_buf_l, s_buf_r, 0xD0571u);
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
    f.init(48000.f, s_buf_l, s_buf_r, 0xD0571u);
    f.set_on(true, true);
    f.set_bpm(120.f);
    f.set_rate(6);                   // slice 6 -> kDivisions[11] "1/8" -> 0.25 s @120
    f.set_feedback(0.f);
    f.set_mix(1.f);
    CHECK(f.delay_time() == doctest::Approx(0.25f).epsilon(0.001));
}

TEST_CASE("flux: longest division clamps to the echo buffer at low BPM") {
    Flux f;
    f.init(48000.f, s_buf_l, s_buf_r, 0xD0571u);
    f.set_on(true, true);
    f.set_bpm(20.f);                 // "1/2" @20 BPM = 6 s > 5 s buffer
    f.set_rate(0);
    const float buf_s = (float)Flux::kMaxSamples / 48000.f;   // 5 s
    CHECK(f.delay_time() < buf_s);
    CHECK(f.delay_time() > buf_s - 0.1f);   // clamped just under the buffer
}

TEST_CASE("flux: feedback produces decaying repeats") {
    Flux f;
    f.init(48000.f, s_buf_l, s_buf_r, 0xD0571u);
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
    f.init(48000.f, s_buf_l, s_buf_r, 0xD0571u);
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
    f.init(48000.f, nullptr, nullptr, 0xD0571u);
    f.set_on(true, true);
    CHECK(!f.has_buffers());
    CHECK(!f.engaged());
    float l = 0.5f, r = 0.5f;
    f.process(l, r);
    CHECK(l == 0.5f);
}

TEST_CASE("flux: feedback at max blooms but stays bounded") {
    Flux f;
    f.init(48000.f, s_buf_l, s_buf_r, 0xD0571u);
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
    f.init(48000.f, s_buf_l, s_buf_r, 0xD0571u);
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
    // Bit-exactness is a claim about what lands on the tape, not merely what
    // Process returns -- compare the stores directly.
    CHECK(std::memcmp(a.line(), b.line(), Flux::kMaxSamples * sizeof(float)) == 0);
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
    // Process returns fast_tanh(...), so |y| <= 1 holds by construction of the
    // return statement no matter what the writeback does -- and fast_tanh's
    // clamp has its own test. The RETURN value therefore cannot show a runaway;
    // the TAPE can, because the writeback lands there. So assert on the tape,
    // and assert the loop actually produced signal rather than passing on zeros.
    CHECK(peak <= 1.f);
    CHECK(peak > 0.1f);             // it ran, it did not sit at silence
    float tape_peak = 0.f;
    for (size_t i = 0; i < Flux::kMaxSamples; ++i) {
        REQUIRE(std::isfinite(e.line()[i]));
        tape_peak = std::max(tape_peak, std::fabs(e.line()[i]));
    }
    // store = fast_tanh(out) * feedback_ + in + wb, with |fast_tanh| <= 1,
    // feedback 1.2, in 1.0, wb 0.9 -> the store cannot exceed 3.1 however long
    // it recirculates. A real runaway breaks this long before it reaches inf.
    CHECK(tape_peak <= 3.1f);   // measured 2.849 -- the bound bites
}

TEST_CASE("echo: frozen writeback overdubs the tape in the direction of wb") {
    // The one case the primitive exists for: frozen tape, non-zero writeback
    // overdubbed onto it. Covers both WEAR REGIMES INSIDE WriteBlend -- wear_
    // >= 1 (pure overdub) and wear_ < 1 (decay blended with the overdub). Note
    // both cases take the SAME branch: wb != 0 sends them to WriteBlend either
    // way, and Advance() is reached only with wb == 0, which the preserving-
    // freeze test above covers. This case is sensitive to a sign error or a
    // swapped argument pair in WriteBlend(wb, wear_): swapping the arguments
    // would compute fast_tanh(old * wb + wear_) instead of
    // fast_tanh(old * wear_ + wb), which disagrees with the expected values
    // below whenever wb != wear_.
    const float ds = 0.1f * 48000.f;
    const float kConst = 0.3f;   // known, non-zero prior tape content
    const float wb = 0.4f;       // known, non-zero writeback

    auto prime_and_freeze = [&](EchoDelay<Flux::kMaxSamples>& e, float wear) {
        e.SetFeedback(0.f);   // store == in exactly while unfrozen, wb == 0
        for (size_t i = 0; i < Flux::kMaxSamples; ++i) e.Process(kConst, ds, 0.f);
        // Confirm the priming actually landed bit-exact before trusting it
        // as the "known prior content" for the freeze pass below.
        float max_prime_err = 0.f;
        for (size_t i = 0; i < Flux::kMaxSamples; ++i)
            max_prime_err = std::max(max_prime_err, std::fabs(e.line()[i] - kConst));
        REQUIRE(max_prime_err == 0.f);

        e.set_freeze(true);
        e.set_wear(wear);
        for (size_t i = 0; i < Flux::kMaxSamples; ++i) e.Process(0.f, ds, wb);
    };

    // wear_ >= 1: pure overdub, no decay of the prior sample.
    static float buf1[Flux::kMaxSamples];
    EchoDelay<Flux::kMaxSamples> e1;
    e1.Init(48000.f, buf1);
    prime_and_freeze(e1, 1.f);
    const float expect1 = fast_tanh(kConst * 1.f + wb);
    float max_err1 = 0.f, max_abs1 = 0.f;
    for (size_t i = 0; i < Flux::kMaxSamples; ++i) {
        max_err1 = std::max(max_err1, std::fabs(e1.line()[i] - expect1));
        max_abs1 = std::max(max_abs1, std::fabs(e1.line()[i]));
    }
    CHECK(max_err1 == 0.f);
    CHECK(max_abs1 <= 1.f);           // fast_tanh's hard clamp, still holds
    CHECK(expect1 > kConst);          // positive wb moved the sample up

    // wear_ < 1: decay of the prior sample blended with the same overdub --
    // must land somewhere different from the wear_ == 1 case above.
    static float buf2[Flux::kMaxSamples];
    EchoDelay<Flux::kMaxSamples> e2;
    e2.Init(48000.f, buf2);
    prime_and_freeze(e2, 0.5f);
    const float expect2 = fast_tanh(kConst * 0.5f + wb);
    float max_err2 = 0.f, max_abs2 = 0.f;
    for (size_t i = 0; i < Flux::kMaxSamples; ++i) {
        max_err2 = std::max(max_err2, std::fabs(e2.line()[i] - expect2));
        max_abs2 = std::max(max_abs2, std::fabs(e2.line()[i]));
    }
    CHECK(max_err2 == 0.f);
    CHECK(max_abs2 <= 1.f);
    CHECK(std::fabs(expect2 - expect1) > 1e-3f);   // wear actually mattered
}

TEST_CASE("deline: N samples behind the head reads the sample written N steps ago") {
    // Pins down the tape-tap indexing convention data()/write_ptr() exist
    // for: write_ptr_ DECREMENTS on every Write, so it moves further back in
    // time as it advances forward in address space. "N samples behind the
    // head" is therefore data()[(write_ptr() + N) & mask] -- an INCREASING
    // index for further into the past, the opposite of the naive
    // "head - N" reading. Task 2's grain reads build directly on this.
    static float buf[Flux::kMaxSamples];
    DeLine<float, Flux::kMaxSamples> line;
    line.Init(buf);

    const int32_t mask = static_cast<int32_t>(Flux::kMaxSamples) - 1;
    const int n = 100;
    // Marker sequence: sample i carries value (i + 1), so 0 never collides
    // with the silent buffer Init() just zeroed.
    for (int i = 0; i < n; ++i) line.Write(static_cast<float>(i + 1));

    // back == 1 is the most recently written sample (value n); back == n is
    // the oldest one still in this run (value 1).
    for (int back = 1; back <= n; ++back) {
        const float expect = static_cast<float>(n - back + 1);
        const float got = line.data()[(line.write_ptr() + back) & mask];
        CHECK(got == doctest::Approx(expect));
    }
}

TEST_CASE("flux: dust 0 is bit-exact with the pre-DUST path at any rot") {
    static float ref_l[Flux::kMaxSamples], ref_r[Flux::kMaxSamples];
    static float dut_l[Flux::kMaxSamples], dut_r[Flux::kMaxSamples];
    // Dummy tape for the standalone DustCloud probe below -- content never
    // matters (the mechanisms under test are gain and grain count, not
    // waveform), only that it is a valid kMaxSamples/mask-contract buffer.
    static float probe_l[Flux::kMaxSamples], probe_r[Flux::kMaxSamples];
    for (float rot : {0.0f, 0.33f, 0.5f, 0.9f, 1.0f}) {
        Flux ref, dut;
        ref.init(48000.f, ref_l, ref_r, 0xD0571u);
        dut.init(48000.f, dut_l, dut_r, 0xD0571u);
        for (Flux* f : {&ref, &dut}) {
            f->set_on(true, true);
            f->set_bpm(120.f);
            f->set_rate(3);
            f->set_feedback(0.6f);
            f->set_mix(0.8f);
        }
        dut.set_dust(0.f);
        dut.set_rot(rot);

        // Mechanism 1 -- the head-takeover gain must be exactly unity at
        // DUST = 0. This is what makes "(e_l * hg + gl) * mix" collapse to
        // "e_l * mix" in Flux::process; Flux's own bypass branch means
        // head_gain() is never even read at DUST = 0, so a retuned knee
        // (e.g. 0.999 instead of 1.0) would silently pass the per-sample
        // output comparison below. Probed on a standalone DustCloud -- the
        // same class Flux wires in -- since Flux exposes no accessor for it.
        DustCloud probe;
        probe.init(48000.f, 1u);
        probe.set_dust(0.f);
        probe.set_rot(rot);
        probe.set_delay_time(dut.delay_time());
        REQUIRE(probe.head_gain() == 1.0f);

        // Mechanism 2 -- no grain may ever go alive at DUST = 0. Called
        // directly on the probe (not through Flux::process) because Flux's
        // "!_dust.active()" bypass ALSO stops DustCloud::process() from ever
        // running at DUST = 0 -- that outer guard would otherwise mask the
        // loss of DustCloud's own internal "_dust <= 0" guard, since neither
        // ref nor dut would call into DustCloud at all either way.
        const TapeTap probe_tap{probe_l, probe_r, 0,
                                 static_cast<int32_t>(Flux::kMaxSamples) - 1};
        int max_active = 0;
        for (int i = 0; i < 120000; ++i) {
            float gl = 0.f, gr = 0.f;
            probe.process(probe_tap, gl, gr);
            max_active = std::max(max_active, probe.active_grains());
        }
        REQUIRE(max_active == 0);

        for (int i = 0; i < 120000; ++i) {
            const float s = std::sin(0.011f * i) * 0.5f;
            float al = s, ar = s, bl = s, br = s;
            ref.process(al, ar);
            dut.process(bl, br);
            REQUIRE(al == bl);
            REQUIRE(ar == br);
        }

        // Bit-exactness is a claim about what lands on the tape, not only
        // about the returned sample -- a divergent store could hide behind
        // identical output for many samples before a read finally exposes
        // it. Compare the two instances' delay-line contents directly (the
        // exact buffers passed into init() above -- what EchoDelay::line()
        // would return).
        REQUIRE(std::memcmp(ref_l, dut_l, sizeof(ref_l)) == 0);
        REQUIRE(std::memcmp(ref_r, dut_r, sizeof(ref_r)) == 0);
    }
}

TEST_CASE("flux: dust makes sound and the head fades at the top") {
    // "The head fades at the top": head_gain() has no Flux accessor, so probe
    // it directly on a standalone DustCloud -- the idiom the dust-0 bit-exact
    // case above already uses. Old version of this case never read
    // head_gain() at all (Flux exposes no accessor for it either), so a
    // mutant that makes the knee branch return 1.f unconditionally passed
    // silently.
    DustCloud probe;
    probe.init(48000.f, 7u);
    probe.set_rot(0.5f);
    probe.set_dust(dust_tuning::kTakeoverKnee);
    CHECK(probe.head_gain() == 1.0f);    // AT the knee: still full echo
    probe.set_dust(0.9f);
    CHECK(probe.head_gain() < 1.0f);     // ABOVE the knee: it faded
    // _remap()'s cosine: t = (0.9 - 0.7) / (1 - 0.7) = 2/3,
    // cos(2/3 * pi/2) ~= 0.5 -- a loose bound well short of 1 still catches a
    // knee that merely clamps near-unity instead of actually fading.
    CHECK(probe.head_gain() < 0.9f);

    // "Dust makes sound": run a DUST = 0 reference Flux beside a DUST-up one
    // over identical input and require the outputs to diverge by more than a
    // trivial margin. The old peak > 0.01f check never exercised this claim:
    // at mix = 1, feedback = 0.5 the pre-existing echo alone clears 0.01f by
    // two orders of magnitude on its own, so a mutant forcing gl = gr = 0.f
    // in DustCloud::process (killing the grain sum outright) still passed.
    //
    // DUST is held at 0.5, AT OR BELOW kTakeoverKnee (0.7), so head_gain()
    // stays exactly 1.0 here -- the only mechanism that can make dut diverge
    // from ref is the grain sum itself, not the head fade tested above. (An
    // earlier version of this case ran DUST = 0.9 for this half too: with
    // gl = gr = 0.f forced, the head-gain fade ALONE still moved dut away
    // from ref, so the mutant passed. Isolating the two claims to two
    // different DUST settings is what makes each mutant land on the
    // assertion that actually names it.)
    static float ref_bl[Flux::kMaxSamples], ref_br[Flux::kMaxSamples];
    static float dut_bl[Flux::kMaxSamples], dut_br[Flux::kMaxSamples];
    Flux ref, dut;
    ref.init(48000.f, ref_bl, ref_br, 11u);
    dut.init(48000.f, dut_bl, dut_br, 11u);
    for (Flux* f : {&ref, &dut}) {
        f->set_on(true, true);
        f->set_bpm(120.f);
        f->set_rate(3);
        f->set_feedback(0.5f);
        f->set_mix(1.f);
    }
    dut.set_dust(0.5f);
    dut.set_rot(0.5f);
    CHECK(dut.dust_active());

    float peak = 0.f;
    double diff_sum_sq = 0.0;
    for (size_t i = 0; i < Flux::kMaxSamples; ++i) {
        const float s = std::sin(0.01f * i) * 0.4f;
        float rl = s, rr = s;
        float dl = s, dr = s;
        ref.process(rl, rr);
        dut.process(dl, dr);
        peak = std::max(peak, std::fabs(dl));
        REQUIRE(std::isfinite(dl));
        const double diff = (double)dl - (double)rl;
        diff_sum_sq += diff * diff;
    }
    const double diff_rms = std::sqrt(diff_sum_sq / (double)Flux::kMaxSamples);
    // Both Flux instances run the same deterministic dry input and the same
    // echo settings, and head_gain() == 1.0 at DUST = 0.5, so ref and dut
    // differ ONLY by the grain sum -- measured diff_rms == 0.148 here; 0.001
    // sits two orders of magnitude below that and above a silent-grain
    // mutant's exact 0.0 (confirmed: forcing gl = gr = 0.f in
    // DustCloud::process makes this measure exactly 0 and fail here).
    CHECK(diff_rms > 0.001);
    CHECK(peak < 4.f);   // measured 1.290 here; feedback 0.5 is sub-unity, so
                          // this stays far under the self-oscillating 8.f
                          // bound derived in the sibling case below.
}

TEST_CASE("flux: dust at full recirculates without running away (writeback still pending Task 5)") {
    static float bl[Flux::kMaxSamples], br[Flux::kMaxSamples];
    Flux f;
    f.init(48000.f, bl, br, 0xD0571u);
    f.set_on(true, true);
    f.set_bpm(120.f);
    f.set_rate(6);
    f.set_feedback(1.f);       // 1.2 coefficient — self-oscillating
    f.set_mix(1.f);
    f.set_dust(1.f);
    // Zone R at this commit exercises head fade-out, reverse-grain spawning
    // and the wear/erosion coefficient -- NOT writeback: DustCloud::process
    // still ends `return 0.f; // writeback arrives in Task 5`, so `wb` is
    // always zero here and the tape store this case's peak reflects is
    // unaffected by ROT. (The old inline comment here read "zone R:
    // writeback active", which was simply wrong at this commit.)
    f.set_rot(1.f);
    float peak = 0.f;
    for (int i = 0; i < 480000; ++i) {   // 10 s
        float l = 1.f, r = 1.f;          // sustained full scale
        f.process(l, r);
        peak = std::max(peak, std::fabs(l));
        REQUIRE(std::isfinite(l));
    }
    // Bound derivation: fast_tanh clamps the echo read at |x| <= 1; the tape
    // store itself blooms to ~2.2 under this feedback (echo.cpp's tanh-bounded
    // bloom, measured); worst case all kGrains = 8 grains sound at once and
    // sum coherently, normalised by 1/sqrt(8) -- sqrt(8) * 2.2 * 0.92 ~= 5.7
    // (0.92 folds in fast_sin's equal-power pan-gain ceiling), plus dry input
    // <= 1 -> ~6.7. 8.f keeps margin above that without masking a real runaway.
    CHECK(peak < 8.f);
}

TEST_CASE("flux: same seed reproduces the grain stream, different seeds diverge") {
    // Pins the seeding CONTRACT (I1, M4): DustCloud's seed must be a caller-
    // supplied constant that fully determines the grain stream on its own --
    // NOT anything derived from the echo buffer's address. That is exactly
    // what let the same patch re-roll its cloud on every load in the VCV
    // plugin: host/vcv/src/Spotymod.cpp declares the echo memory as a member
    // of `struct Spotymod : Module`, which Rack heap-allocates per instance,
    // so the address (and the old address-hashed seed derived from it) was
    // neither stable across loads nor under ASLR. A reproducibility test like
    // this one would have caught that before it shipped.
    auto run = [](uint32_t seed, float* bl, float* br) {
        Flux f;
        f.init(48000.f, bl, br, seed);
        f.set_on(true, true);
        f.set_bpm(120.f);
        f.set_rate(3);
        f.set_feedback(0.5f);
        f.set_mix(1.f);
        f.set_dust(0.9f);
        f.set_rot(0.6f);
        std::vector<float> out(48000);
        for (int i = 0; i < (int)out.size(); ++i) {
            const float s = std::sin(0.01f * i) * 0.4f;
            float l = s, r = s;
            f.process(l, r);
            out[i] = l;
        }
        return out;
    };

    // Deliberately different buffer addresses for every run (three distinct
    // static arrays): under the old buf_l-address-derived seed this alone
    // would have made same_a and same_b diverge despite the "same seed" below
    // being meaningless in that scheme -- there was no seed parameter to pass.
    static float a_bl[Flux::kMaxSamples], a_br[Flux::kMaxSamples];
    static float b_bl[Flux::kMaxSamples], b_br[Flux::kMaxSamples];
    static float c_bl[Flux::kMaxSamples], c_br[Flux::kMaxSamples];

    const auto same_a = run(0x1234abcdu, a_bl, a_br);
    const auto same_b = run(0x1234abcdu, b_bl, b_br);
    const auto diff_c = run(0x9e3779b9u, c_bl, c_br);

    CHECK(same_a == same_b);   // identical seed -> bit-identical grain stream

    bool any_diff = false;
    for (size_t i = 0; i < same_a.size(); ++i) {
        if (same_a[i] != diff_c[i]) { any_diff = true; break; }
    }
    CHECK(any_diff);           // different seed -> a different grain stream
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

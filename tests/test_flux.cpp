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

TEST_CASE("deline: N samples behind the head reads the sample written N steps ago") {
    // Pins down the tape-tap indexing convention data()/write_ptr() exist
    // for: write_ptr_ DECREMENTS on every Write, so it moves further back in
    // time as it advances forward in address space. "N samples behind the
    // head" is therefore data()[(write_ptr() + N) & mask] -- an INCREASING
    // index for further into the past, the opposite of the naive
    // "head - N" reading. TapBank's tap reads (fx/taps.h) build directly on
    // this.
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

TEST_CASE("flux: dust 0 stays inert at any rot -- against a never-touched reference, output AND tape") {
    // Recovered after review: the previous version of this test compared two
    // LIVE `Flux` instances that were both at DUST = 0, i.e. both took the
    // bypass branch (flux.cpp's `if (!_taps.active())`). ROT only writes
    // TapBank's one-pole coefficients (taps.cpp's `_update_filters`), which
    // the bypass never reads -- so that comparison was bypass-against-bypass,
    // tautologically equal by construction, regardless of what set_rot did.
    //
    // The only way to state "DUST = 0 leaves ROT inert" without begging the
    // question is a reference on which set_dust/set_rot are NEVER called at
    // all -- so a bug in set_rot's OWN side effects (e.g. corrupting _mix_lin
    // or _dt_target, not just TapBank's filters) has somewhere to show up.
    // Rate pinned to the ladder's shortest synced division ("1/32" -- slice
    // 11, kFluxRateOffset+11) so the delay is ~3000 samples @120 BPM/48 kHz,
    // not the 3-boot default's ~24000. A delay longer than the test window
    // (as the boot default is) means the tape read lands in never-written
    // territory for the ENTIRE run -- EchoDelay::Process() returns bit-exact
    // 0.0f regardless of _dt_current/_mix_lin, which would make this test
    // structurally blind to any corruption of either (confirmed empirically:
    // nudging _dt_target and separately _mix_lin by one ULP inside set_rot
    // both survived undetected at the boot rate). At ~3000 samples the 20000-
    // sample window covers several feedback cycles, so real, nonzero, tape-
    // derived material actually reaches the output and is available to
    // diverge.
    static float ref_l[Flux::kMaxSamples], ref_r[Flux::kMaxSamples];
    Flux ref;
    ref.init(48000.f, ref_l, ref_r);
    ref.set_on(true, true);
    ref.set_rate(11);
    // set_dust/set_rot: deliberately never called on `ref`.

    static float ref_out_l[20000], ref_out_r[20000];
    for (int i = 0; i < 20000; ++i) {
        const float x = std::sin(static_cast<float>(i) * 0.01f);
        float l = x, r = x;
        ref.process(l, r);
        ref_out_l[i] = l;
        ref_out_r[i] = r;
    }
    // Not a vacuous silence-vs-silence comparison: the caller's `l`/`r` start
    // as the dry input `x` itself (process() only adds to them), so the
    // reference trace is real signal from sample 0, not an untouched buffer.
    bool ref_nonzero = false;
    for (int i = 0; i < 20000; ++i) if (ref_out_l[i] != 0.f) { ref_nonzero = true; break; }
    REQUIRE(ref_nonzero);

    const float rots[] = { 0.f, 0.33f, 0.5f, 0.9f, 1.f };
    for (float rot : rots) {
        static float f_l[Flux::kMaxSamples], f_r[Flux::kMaxSamples];
        Flux f;
        f.init(48000.f, f_l, f_r);
        f.set_on(true, true);
        f.set_rate(11);
        f.set_dust(0.f);
        f.set_rot(rot);

        for (int i = 0; i < 20000; ++i) {
            const float x = std::sin(static_cast<float>(i) * 0.01f);
            float l = x, r = x;
            f.process(l, r);
            REQUIRE(l == ref_out_l[i]);   // exact ==, not Approx
            REQUIRE(r == ref_out_r[i]);
        }

        // Compare the tape itself, not only what was read off it. A store
        // that diverges (e.g. ROT's filters leaking into the echo's write
        // path) can hide behind identical output indefinitely -- output only
        // reveals a bad store once something reads that region back, which
        // may be thousands of samples later, or never inside this window.
        CHECK(std::memcmp(f_l, ref_l, sizeof(f_l)) == 0);
        CHECK(std::memcmp(f_r, ref_r, sizeof(f_r)) == 0);
    }
}

TEST_CASE("flux: taps sound only once offsets have been pushed") {
    static float buf_l[Flux::kMaxSamples], buf_r[Flux::kMaxSamples];
    Flux f;
    f.init(48000.f, buf_l, buf_r);
    f.set_on(true, true);
    f.set_dust(1.f);
    f.set_rot(0.f);
    // Push the echo's own delay time out past this whole test (mirrors
    // "flux: longest division clamps to the echo buffer at low BPM"): at the
    // default 120 BPM / "1/4" the echo's first arrival lands at ~24000
    // samples, inside the quiet-window measurement below, so without this
    // "quiet" would measure echo bleed, not taps.
    f.set_bpm(20.f);
    f.set_rate(0);

    // Prime the tape with signal, offsets still muted.
    double quiet = 0.0;
    for (int i = 0; i < 30000; ++i) {
        const float x = std::sin(static_cast<float>(i) * 0.03f);
        float l = x, r = x;
        f.process(l, r);
        if (i > 25000) quiet += std::fabs(static_cast<double>(l - x));
    }

    const int32_t off[2] = { 6000, 10500 };
    f.set_tap_offsets(off);

    double loud = 0.0;
    for (int i = 30000; i < 60000; ++i) {
        const float x = std::sin(static_cast<float>(i) * 0.03f);
        float l = x, r = x;
        f.process(l, r);
        if (i > 55000) loud += std::fabs(static_cast<double>(l - x));
    }
    CHECK(quiet == 0.0);
    // A real floor, not just `loud > quiet`: with quiet == 0.0 exactly, a tap
    // contribution as small as 1e-30 would also satisfy `loud > quiet` and
    // the assertion would prove nothing about audibility. Measured `loud` for
    // this offsets/window pair is ~123.2 (5000-sample sum of |l - x|,
    // kTapGain = 0.7 read straight off a primed tape) -- pinned as a literal,
    // not an expression over tap_tuning constants, so this floor doesn't
    // silently track a future gain-constant change.
    CHECK(loud > 1.0);
}

TEST_CASE("flux: init resets the DUST guard so a re-init's repeated push isn't swallowed") {
    // Reproduces Spotymod::reinit() -> Instrument::init() -> Flux::init(): a
    // sample-rate change re-initialises TapBank, whose own _dust resets to 0
    // (taps.cpp), but Flux::init must ALSO reset _dust_norm, or the guard on
    // set_dust (I3) sees the next push of the SAME value the user already
    // had dialled in as a no-op and swallows it.
    static float buf_l[Flux::kMaxSamples], buf_r[Flux::kMaxSamples];
    Flux f;
    f.init(48000.f, buf_l, buf_r);
    f.set_on(true, true);
    f.set_dust(0.7f);

    // Simulate the re-init.
    f.init(48000.f, buf_l, buf_r);
    f.set_on(true, true);
    CHECK_FALSE(f.taps_active());   // sanity: the re-init really did go inert

    f.set_bpm(20.f);                // push the echo's own arrival out of the window
    f.set_rate(0);
    const int32_t off[2] = { 6000, 10500 };
    f.set_tap_offsets(off);

    f.set_dust(0.7f);                // SAME value as before the re-init

    // At this moment, what would the un-fixed code produce? _dust_norm is
    // still 0.7f from before the re-init (Flux::init never touched it), so
    // `d == _dust_norm` is true and set_dust returns early: _taps.set_dust()
    // is never called, TapBank's OWN _dust (reset to 0 by TapBank::init)
    // never leaves 0, and taps_active() stays false -- forever, since
    // nothing will ever again push a DIFFERENT dust value in this scenario
    // (the user's knob hasn't moved). That is different from the fixed
    // code's true.
    CHECK(f.taps_active());

    double loud = 0.0;
    for (int i = 0; i < 30000; ++i) {
        const float x = std::sin(static_cast<float>(i) * 0.03f);
        float l = x, r = x;
        f.process(l, r);
        if (i > 25000) loud += std::fabs(static_cast<double>(l - x));
    }
    // Same floor technique as "flux: taps sound only once offsets have been
    // pushed" above: a real audibility floor, not just `loud > 0`.
    CHECK(loud > 1.0);
}

TEST_CASE("flux: init resets the ROT guard so a re-init's repeated push isn't swallowed") {
    // Isolates ROT from DUST: DUST is pushed to a DIFFERENT value after the
    // re-init (0.3 -> 0.7), so it always forwards regardless of its OWN
    // guard's state -- any difference measured below is attributable to ROT
    // alone. ROT is pushed to the SAME value (0.9) before and after the
    // re-init, which is exactly the shape that trips a stale guard.
    static float ref_l[Flux::kMaxSamples], ref_r[Flux::kMaxSamples];
    static float act_l[Flux::kMaxSamples], act_r[Flux::kMaxSamples];
    const int32_t off[2] = { 6000, tap_tuning::kMuted };   // tap 0 only: the LP branch

    // Reference: a FRESH Flux, dialled straight to the target state once.
    Flux ref;
    ref.init(48000.f, ref_l, ref_r);
    ref.set_on(true, true);
    ref.set_bpm(20.f);
    ref.set_rate(0);                // push the echo's own arrival out of the window
    ref.set_tap_offsets(off);
    ref.set_dust(0.7f);
    ref.set_rot(0.9f);              // strong LP: cutoff ~590 Hz, heavily attenuates 6 kHz

    // Actual: dial in a state, simulate a re-init, then dial in the target
    // state a second time -- ROT repeats 0.9, DUST does not repeat 0.3.
    Flux act;
    act.init(48000.f, act_l, act_r);
    act.set_on(true, true);
    act.set_dust(0.3f);
    act.set_rot(0.9f);
    act.init(48000.f, act_l, act_r);   // re-init
    act.set_on(true, true);
    act.set_bpm(20.f);
    act.set_rate(0);
    act.set_tap_offsets(off);
    act.set_dust(0.7f);             // different from the pre-reinit 0.3f -- always forwards
    act.set_rot(0.9f);               // SAME as the pre-reinit 0.9f -- swallowed if stale

    // 6 kHz probe (2*pi*6000/48000 rad/sample). The offset (6000) reads
    // material written 6000 samples ago, so the read position holds real
    // in-run signal -- not the pristine zero-initialised tape -- only once
    // at least 6000 samples have elapsed; the gain slew (~960 samples) and
    // offset dip (~192 samples) settle well before that. Run to 16000 and
    // measure the last 4000 samples for a wide settle margin.
    double ref_energy = 0.0, act_energy = 0.0;
    for (int i = 0; i < 16000; ++i) {
        const float x = std::sin(static_cast<float>(i) * 0.785398f);
        float rl = x, rr = x;
        ref.process(rl, rr);
        float al = x, ar = x;
        act.process(al, ar);
        if (i > 12000) {
            ref_energy += std::fabs(static_cast<double>(rl - x));
            act_energy += std::fabs(static_cast<double>(al - x));
        }
    }
    REQUIRE(ref_energy > 1.0);   // the reference itself must be audible and settled

    // At this moment, what would the un-fixed code produce? act's ROT push
    // is swallowed by the stale guard (_rot_norm still 0.9f from before the
    // re-init), so TapBank's OWN _rot -- reset to 0 by TapBank::init -- is
    // never told about 0.9f and stays at 0 (LP cutoff ~18 kHz, barely
    // touches a 6 kHz tone). act_energy would then sit close to an
    // UNFILTERED tap's level, several times ref_energy's heavily-filtered
    // level -- not close to ref_energy as asserted below.
    INFO("ref_energy=" << ref_energy << " act_energy=" << act_energy);
    CHECK(act_energy == doctest::Approx(ref_energy).epsilon(0.05));
}

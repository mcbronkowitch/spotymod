#include <doctest/doctest.h>
#include <vector>
#include <cmath>
#include <algorithm>
#include <limits>
#include "instrument.h"
#include "fx/reverb.h"
#include "mod/super_modulator.h"
using namespace spky;

TEST_CASE("instrument: init and render a block without NaNs") {
    Instrument inst;
    inst.init(48000.f);
    inst.set_tempo_bpm(120.f);
    inst.set_target_active(PART_A, LANE_PITCH, true);
    inst.set_target_active(PART_A, LANE_LEVEL, true);
    inst.set_rate(PART_A, 0.5f);
    inst.set_range(PART_A, 1.f);

    std::vector<float> l(96), r(96);
    inst.process(nullptr, nullptr, l.data(), r.data(), 96);
    for (int i = 0; i < 96; ++i) {
        CHECK(l[i] == l[i]);            // not NaN
        CHECK(l[i] >= -1.5f);
        CHECK(l[i] <=  1.5f);
    }
}

TEST_CASE("instrument: the two parts are decorrelated") {
    Instrument inst;
    inst.init(48000.f);
    inst.set_rate(PART_A, 0.5f);
    inst.set_rate(PART_B, 0.5f);
    inst.set_shape(PART_A, 1.f);
    inst.set_shape(PART_B, 1.f);
    inst.set_range(PART_A, 1.f);
    inst.set_range(PART_B, 1.f);

    std::vector<float> l(1), r(1);
    bool differ = false;
    for (int i = 0; i < 48000; ++i) {
        inst.process(nullptr, nullptr, l.data(), r.data(), 1);
        if (std::fabs(inst.lane_output(PART_A, LANE_PITCH)
                    - inst.lane_output(PART_B, LANE_PITCH)) > 0.05f) differ = true;
    }
    CHECK(differ);
}

TEST_CASE("instrument: set_scale is global and reaches both parts") {
    Instrument inst;
    inst.init(48000.f);
    inst.set_target_depth(PART_A, LANE_PITCH, 0.f);
    inst.set_target_depth(PART_B, LANE_PITCH, 0.f);
    inst.set_target_base(PART_A, LANE_PITCH, 0.5f);
    inst.set_target_base(PART_B, LANE_PITCH, 0.5f);
    inst.set_scale(SCALE_WHOLE);   // 18 semis is a whole-tone degree
    std::vector<float> l(1), r(1);
    for (int i = 0; i < 4000; ++i)   // ride out the 40 ms change slew
        inst.process(nullptr, nullptr, l.data(), r.data(), 1);
    CHECK(inst.pitch_cv(PART_A) == doctest::Approx(18.f / 36.f));
    CHECK(inst.pitch_cv(PART_B) == doctest::Approx(18.f / 36.f));
}

static float s_ti_echo[PART_COUNT][2][spky::Flux::kMaxSamples];
static spky::AmbientReverb s_ti_reverb;

static spky::FxMem test_fx_mem() {
    spky::FxMem m;
    for (int p = 0; p < PART_COUNT; ++p)
        for (int c = 0; c < 2; ++c) m.echo[p][c] = s_ti_echo[p][c];
    m.reverb = &s_ti_reverb;
    return m;
}

TEST_CASE("instrument: all FX off + send 0 is bit-identical to the no-FX build") {
    Instrument plain;
    plain.init(48000.f);
    Instrument fx;
    fx.init(48000.f, test_fx_mem());
    for (int p = 0; p < PART_COUNT; ++p)
        fx.set_fx_target_base(p, FXT_REV_SEND, 0.f);   // before any process()
    fx.set_reverb_mix(0.f);                        // MIX 0: dry passes untouched
    float pl, pr, fl, fr;
    for (int i = 0; i < 48000; ++i) {
        plain.process(nullptr, nullptr, &pl, &pr, 1);
        fx.process(nullptr, nullptr, &fl, &fr, 1);
        CHECK(fl == pl);
        CHECK(fr == pr);
    }
}

TEST_CASE("instrument: boot reverb send is audible") {
    Instrument plain;
    plain.init(48000.f);
    Instrument fx;
    fx.init(48000.f, test_fx_mem());   // boot REV_SEND base = 0.25
    float pl, pr, fl, fr;
    int diff = 0;
    for (int i = 0; i < 48000; ++i) {
        plain.process(nullptr, nullptr, &pl, &pr, 1);
        fx.process(nullptr, nullptr, &fl, &fr, 1);
        if (std::fabs(fl - pl) > 1e-5f) ++diff;
    }
    CHECK(diff > 1000);
}

TEST_CASE("instrument: fx setters reach the parts and reverb setters are null-safe") {
    Instrument inst;
    inst.init(48000.f);                 // NO reverb, NO buffers
    inst.set_fx_on(PART_A, FxBlock::Grit, true);
    inst.set_grit_mode(PART_A, GritMode::Reduce);
    inst.set_fx_target_active(PART_A, FXT_GRIT_INT, true);
    inst.set_fx_target_base(PART_A, FXT_GRIT_INT, 0.6f);
    inst.set_fx_target_depth(PART_A, FXT_GRIT_INT, 0.5f);
    inst.set_flux_mix(PART_A, 0.4f);
    inst.set_grit_mix(PART_A, 0.7f);
    inst.set_reverb_size(0.9f);         // must not crash without a reverb
    inst.set_reverb_tone(0.2f);
    inst.set_reverb_decay(0.7f);
    inst.set_reverb_diffusion(0.5f);
    inst.set_reverb_mix(0.7f);
    float l, r;
    inst.process(nullptr, nullptr, &l, &r, 1);
    CHECK(inst.fx_target_value(PART_A, FXT_GRIT_INT) >= 0.f);
    CHECK(l == l);   // not NaN
}

static float s_pp_echo[PART_COUNT][2][spky::Flux::kMaxSamples];

static spky::FxMem pp_fx_mem() {
    spky::FxMem m;
    for (int p = 0; p < PART_COUNT; ++p)
        for (int c = 0; c < 2; ++c) m.echo[p][c] = s_pp_echo[p][c];
    return m;
}

TEST_CASE("instrument: set_dust forwards to the named part only") {
    // MORPH at an extreme is an equal-power crossfade, and the isolation it
    // gives is ASYMMETRIC -- do not read the two halves as equally strong:
    //   morph 0: gain_a = cos(0) = 1.0f and gain_b = sin(0) = 0.0f, both
    //            bit-exact, so B is multiplied by a true zero. Bulletproof.
    //   morph 1: gain_b = sin(kQuarter) rounds to exactly 1.0f, but
    //            gain_a = cos(kQuarter) is -4.37e-8, NOT zero -- float32
    //            rounding of pi/2. A leaks at about -147 dB.
    // The A-audible half is therefore the load-bearing one; it is exact and it
    // is also the assertion that fires first on a part-swap mutant. The
    // B-audible half is near-zero isolation, strong enough for the mutants
    // tested but not a true zero, and it must not be relied on alone.
    // A per-part test must fail if part A's value is forwarded to part B: here,
    // if set_dust(PART_A, .) actually wrote PART_B's Flux (silent, morph 0),
    // base_a and dust_a would come out identical and CHECK(base_a != dust_a)
    // would fail -- likewise for the B-audible half below.
    auto run = [](float dust_a, float dust_b, float morph) {
        Instrument inst;
        inst.init(48000.f, pp_fx_mem());
        inst.set_morph(morph);
        inst.set_fx_on(PART_A, FxBlock::Flux, true);
        inst.set_fx_on(PART_B, FxBlock::Flux, true);
        inst.set_flux_mix(PART_A, 1.f);
        inst.set_flux_mix(PART_B, 1.f);
        float l, r;
        for (int i = 0; i < 20000; ++i) inst.process(nullptr, nullptr, &l, &r, 1);  // settle morph snap + fx fade
        inst.set_dust(PART_A, dust_a);
        inst.set_dust(PART_B, dust_b);
        std::vector<float> out(20000);
        for (size_t i = 0; i < out.size(); ++i) {
            inst.process(nullptr, nullptr, &l, &r, 1);
            out[i] = l;
        }
        return out;
    };

    const auto base_a = run(0.f, 0.f, 0.f);    // A audible (morph 0 -> gain_a = 1, gain_b = 0)
    const auto dust_a = run(0.6f, 0.f, 0.f);   // only PART_A's dust moves
    CHECK(base_a != dust_a);

    const auto base_b = run(0.f, 0.f, 1.f);    // B audible (morph 1 -> gain_a = 0, gain_b = 1)
    const auto dust_b = run(0.f, 0.6f, 1.f);   // only PART_B's dust moves
    // NOT a plain !=: at morph 1, gain_a is -4.37e-8, not an exact zero (see
    // the comment atop this case), so a mutant that ignores the part index
    // and always writes _parts[0] regardless of which part was named --
    // e.g. `void set_dust(int p, float n) { _parts[0].fx().set_dust(n); }`
    // -- still makes base_b and dust_b differ: the DUST it misapplied to the
    // wrong (near-silent) part leaks through that -147 dB gain, and `!=` on a
    // std::vector<float> reports "true" from a single differing sample no
    // matter how small. Compare RMS against a threshold instead -- real
    // per-part isolation moves dust_b by orders of magnitude more than a
    // -147 dB leak ever could.
    double sum_sq = 0.0;
    for (size_t i = 0; i < base_b.size(); ++i) {
        const double diff = (double)dust_b[i] - (double)base_b[i];
        sum_sq += diff * diff;
    }
    const double rms = std::sqrt(sum_sq / (double)base_b.size());
    CHECK(rms > 1e-4);
}

TEST_CASE("instrument: set_rot forwards to the named part only") {
    // Same isolation idiom as set_dust above. ROT only affects DustCloud
    // behaviour while DUST is active, so both parts get a common DUST > 0
    // baseline first and the runs diverge only in the ROT value.
    auto run = [](float rot_a, float rot_b, float morph) {
        Instrument inst;
        inst.init(48000.f, pp_fx_mem());
        inst.set_morph(morph);
        inst.set_fx_on(PART_A, FxBlock::Flux, true);
        inst.set_fx_on(PART_B, FxBlock::Flux, true);
        inst.set_flux_mix(PART_A, 1.f);
        inst.set_flux_mix(PART_B, 1.f);
        float l, r;
        for (int i = 0; i < 20000; ++i) inst.process(nullptr, nullptr, &l, &r, 1);
        inst.set_dust(PART_A, 0.7f);
        inst.set_dust(PART_B, 0.7f);
        for (int i = 0; i < 5000; ++i) inst.process(nullptr, nullptr, &l, &r, 1);  // let DUST settle in first
        inst.set_rot(PART_A, rot_a);
        inst.set_rot(PART_B, rot_b);
        std::vector<float> out(20000);
        for (size_t i = 0; i < out.size(); ++i) {
            inst.process(nullptr, nullptr, &l, &r, 1);
            out[i] = l;
        }
        return out;
    };

    const auto base_a = run(0.f, 0.f, 0.f);    // A audible
    const auto rot_a   = run(0.9f, 0.f, 0.f);  // only PART_A's rot moves
    CHECK(base_a != rot_a);

    const auto base_b = run(0.f, 0.f, 1.f);    // B audible
    const auto rot_b   = run(0.f, 0.9f, 1.f);  // only PART_B's rot moves
    // Same reasoning as set_dust's B-half above: an exact `!=` is maximally
    // sensitive to the -147 dB morph-1 leak on gain_a, so a mutant that
    // forwards to the wrong part still trips it via that leak. RMS against a
    // threshold instead.
    double sum_sq = 0.0;
    for (size_t i = 0; i < base_b.size(); ++i) {
        const double diff = (double)rot_b[i] - (double)base_b[i];
        sum_sq += diff * diff;
    }
    const double rms = std::sqrt(sum_sq / (double)base_b.size());
    CHECK(rms > 1e-4);
}

TEST_CASE("instrument: boots both parts on the synth engine with an audible drone") {
    Instrument inst;
    inst.init(48000.f);
    CHECK(inst.engine_id(PART_A) == ENGINE_SYNTH);
    CHECK(inst.engine_id(PART_B) == ENGINE_SYNTH);
    float l, r, energy = 0.f;
    for (int i = 0; i < 48000; ++i) {
        inst.process(nullptr, nullptr, &l, &r, 1);
        energy += l * l;
    }
    CHECK(inst.active_voices(PART_A) >= 1);
    CHECK(inst.active_voices(PART_B) >= 1);
    CHECK(energy > 1e-3f);
}

// DENSE 0 leaves only the downbeat/anchor slot able to fire, so after the
// guaranteed first-sample fire (STEP entry: step -1 -> 0) the next natural
// note is a full cycle away. Settle past that single note's decay before
// checking silence, so the manual trigger is the only voice left.
TEST_CASE("instrument: voice setters and manual trigger reach the part") {
    Instrument inst;
    inst.init(48000.f);
    inst.set_voice_decay(PART_A, 0.f);      // shortest decay ratio (0.1x cycle)
    inst.set_step(PART_A, true, 8);
    inst.set_density(PART_A, 0.f);          // anchor-only: next natural fire is a cycle away
    float l, r;
    for (int i = 0; i < 10000; ++i) inst.process(nullptr, nullptr, &l, &r, 1);
    CHECK(inst.active_voices(PART_A) == 0);
    inst.trigger_manual(PART_A);
    inst.process(nullptr, nullptr, &l, &r, 1);
    CHECK(inst.active_voices(PART_A) == 1);
    float peak = 0.f;                        // some voice's envelope is running
    for (int v = 0; v < 4; ++v) peak = std::max(peak, inst.voice_env(PART_A, v));
    CHECK(peak > 0.f);
}

TEST_CASE("instrument: set_engine switches to the test tone and back") {
    Instrument inst;
    inst.init(48000.f);
    inst.set_engine(PART_A, ENGINE_TEST_TONE);
    float l, r;
    for (int i = 0; i < 1000; ++i) inst.process(nullptr, nullptr, &l, &r, 1);
    CHECK(inst.engine_id(PART_A) == ENGINE_TEST_TONE);
    CHECK(inst.active_voices(PART_A) == 0);
    inst.set_engine(PART_A, ENGINE_SYNTH);
    for (int i = 0; i < 48000; ++i) inst.process(nullptr, nullptr, &l, &r, 1);
    CHECK(inst.engine_id(PART_A) == ENGINE_SYNTH);
    CHECK(inst.active_voices(PART_A) >= 1);   // the drone resumes
}

TEST_CASE("instrument M4: couple 0 + drift 0 -> PITCH lane matches a bare SuperModulator") {
    Instrument inst; inst.init(48000.f);
    inst.set_couple(0.f); inst.set_drift(0.f);
    inst.set_rate(PART_A, 0.5f);
    SuperModulator ref; ref.init(48000.f, 0x1234abcdu);   // PART_A seed (see instrument.cpp)
    ref.set_rate(0.5f);
    bool same = true;
    std::vector<float> l(1), r(1);
    for (int i = 0; i < 20000; ++i) {
        inst.process(nullptr, nullptr, l.data(), r.data(), 1);
        ref.process();
        if (inst.lane_output(PART_A, LANE_PITCH) != ref.lane_output(LANE_PITCH)) same = false;
    }
    CHECK(same);   // Center writes rate_scale=1 / shape_offset=0 -> zero perturbation
}

// Two tests: DRY isolation is exact (no reverb), SEND isolation is a decaying
// tail (with reverb). One combined test was wrong — at morph 1 the DRY path is
// gone immediately, but the shared reverb keeps ringing out the send injected
// during the 0.5->1 morph ramp, so an absolute "difference < 1e-5 within 1 s"
// contradicts the design ("only its already-committed tail rings out").
TEST_CASE("instrument M4: morph=1 isolates part A's dry path") {
    Instrument x; x.init(48000.f);                 // no reverb: a pure dry-isolation check
    Instrument y; y.init(48000.f);
    x.set_morph(1.f); y.set_morph(1.f);            // full B; part A must stop contributing
    x.set_rate(PART_A, 0.3f); x.set_target_base(PART_A, LANE_PITCH, 0.2f);
    y.set_rate(PART_A, 0.9f); y.set_target_base(PART_A, LANE_PITCH, 0.9f);   // A differs a lot
    float xl, xr, yl, yr, maxd = 0.f;
    for (int i = 0; i < 48000; ++i) {
        x.process(nullptr, nullptr, &xl, &xr, 1);
        y.process(nullptr, nullptr, &yl, &yr, 1);
        if (i > 16000) { float d = std::fabs(xl - yl); if (d > maxd) maxd = d; }  // after morph snaps to 1
    }
    CHECK(maxd < 1e-5f);   // A's dry contribution is gone (gain_a = cos(pi/2) ~ 0)
}

TEST_CASE("instrument M4: morph=1 injects no new reverb from part A (send isolated)") {
    static float echoX[PART_COUNT][2][Flux::kMaxSamples];
    static float echoY[PART_COUNT][2][Flux::kMaxSamples];
    static AmbientReverb rvX, rvY;
    FxMem mx, my;
    for (int p = 0; p < PART_COUNT; ++p)
        for (int c = 0; c < 2; ++c) { mx.echo[p][c] = echoX[p][c]; my.echo[p][c] = echoY[p][c]; }
    mx.reverb = &rvX; my.reverb = &rvY;
    Instrument x; x.init(48000.f, mx);
    Instrument y; y.init(48000.f, my);
    x.set_morph(1.f); y.set_morph(1.f);
    x.set_reverb_decay(0.15f); y.set_reverb_decay(0.15f);   // short tail so 3 s covers full decay
    x.set_reverb_size(0.2f);   y.set_reverb_size(0.2f);     // small room too
    x.set_rate(PART_A, 0.3f); x.set_target_base(PART_A, LANE_PITCH, 0.2f);
    y.set_rate(PART_A, 0.9f); y.set_target_base(PART_A, LANE_PITCH, 0.9f);
    float xl, xr, yl, yr, early = 0.f, late = 0.f;
    const int N = 48000 * 3;
    for (int i = 0; i < N; ++i) {
        x.process(nullptr, nullptr, &xl, &xr, 1);
        y.process(nullptr, nullptr, &yl, &yr, 1);
        float d = std::fabs(xl - yl);
        if (i < 24000)      { if (d > early) early = d; }   // first 0.5 s: morph ramp injects A
        if (i >= N - 24000) { if (d > late)  late  = d; }   // final 0.5 s: only a decayed tail
    }
    // No new A energy enters the shared reverb at morph 1 -> the divergence is a
    // decaying tail: the final window is far below the early transient, near zero.
    CHECK(early > 1e-4f);
    CHECK(late < early * 0.05f);
    CHECK(late < 1e-4f);
}

TEST_CASE("instrument: set_comp forwards to the part chain") {
    // Two identically-seeded instruments, one with comp up: the comp'd one
    // must be louder on the same deterministic synth content.
    auto render_rms = [](float comp) {
        Instrument inst;
        inst.init(48000.f);                       // engine-only init: no FxMem needed
        inst.set_comp(0, comp);
        inst.trigger_manual(0);
        double acc = 0.0;
        float l[96], r[96];
        const float inL[96] = {0}, inR[96] = {0};
        int n = 0;
        for (int b = 0; b < 500; ++b) {
            inst.process(inL, inR, l, r, 96);
            if (b == 250) inst.trigger_manual(0);
            for (int i = 0; i < 96; ++i) { acc += l[i] * l[i]; ++n; }
        }
        return std::sqrt((float)(acc / n));
    };
    CHECK(render_rms(1.f) > render_rms(0.f));
}

TEST_CASE("instrument: master output never exceeds 1.0 even driven hard") {
    Instrument inst;
    inst.init(48000.f);
    inst.set_master_drive(1.f);                    // 4x into the ceiling
    inst.set_comp(0, 1.f);
    inst.set_comp(1, 1.f);
    inst.trigger_manual(0);
    inst.trigger_manual(1);
    float l[96], r[96];
    const float inL[96] = {0}, inR[96] = {0};
    for (int b = 0; b < 1000; ++b) {
        inst.process(inL, inR, l, r, 96);
        if (b % 100 == 0) { inst.trigger_manual(0); inst.trigger_manual(1); }
        for (int i = 0; i < 96; ++i) {
            CHECK(std::fabs(l[i]) <= 1.f);
            CHECK(std::fabs(r[i]) <= 1.f);
            CHECK(std::isfinite(l[i]));
        }
    }
}

TEST_CASE("instrument: dynamics chain is deterministic end to end") {
    auto run = [] {
        Instrument inst;
        inst.init(48000.f);
        inst.set_master_drive(0.7f);
        inst.set_comp(0, 0.9f);
        inst.trigger_manual(0);
        std::vector<float> out;
        float l[96], r[96];
        const float inL[96] = {0}, inR[96] = {0};
        for (int b = 0; b < 500; ++b) {
            inst.process(inL, inR, l, r, 96);
            for (int i = 0; i < 96; ++i) out.push_back(l[i]);
        }
        return out;
    };
    auto a = run(), b = run();
    for (size_t i = 0; i < a.size(); ++i) CHECK(a[i] == b[i]);
}

TEST_CASE("instrument M4.8: mix 0 is bit-identical to the engine-only build") {
    Instrument plain;
    plain.init(48000.f);
    Instrument fx;
    fx.init(48000.f, test_fx_mem());
    fx.set_reverb_mix(0.f);            // before the first process(): snaps
    // NOTE: the default sends stay live — the wet return is simply discarded
    float pl, pr, fl, fr;
    for (int i = 0; i < 48000; ++i) {
        plain.process(nullptr, nullptr, &pl, &pr, 1);
        fx.process(nullptr, nullptr, &fl, &fr, 1);
        CHECK(fl == pl);
        CHECK(fr == pr);
    }
    CHECK(fx.reverb_asleep());
}

TEST_CASE("instrument M4.8: mix 1 with muted sends is exact silence (dry fully gone)") {
    Instrument fx;
    fx.init(48000.f, test_fx_mem());
    fx.set_reverb_mix(1.f);
    for (int p = 0; p < PART_COUNT; ++p)
        fx.set_fx_target_base(p, FXT_REV_SEND, 0.f);   // empty room: wet is silence
    float l, r;
    for (int i = 0; i < 48000; ++i) {
        fx.process(nullptr, nullptr, &l, &r, 1);
        CHECK(l == 0.f);               // dry gain is EXACTLY 0 at the endpoint
        CHECK(r == 0.f);
    }
}

TEST_CASE("instrument M4.8: mix 0.5 sits at equal power (both gains cos(pi/4))") {
    // Three identically-seeded fx instruments at MIX 0 / 0.5 / 1, default
    // sends live. Their dry and wet streams are bit-identical, so:
    //   out0   = dry            out1   = wet
    //   out05  = 0.7071*dry + 0.7071*wet
    // => rms(out05 - 0.7071*out0) / rms(out1) == 0.7071  (wet gain)
    //    rms(out05 - 0.7071*out1) / rms(out0) == 0.7071  (dry gain)
    static float echoEP[3][PART_COUNT][2][Flux::kMaxSamples];
    static AmbientReverb rvEP[3];
    Instrument inst[3];
    const float mixes[3] = { 0.f, 0.5f, 1.f };
    for (int k = 0; k < 3; ++k) {
        FxMem m;
        for (int p = 0; p < PART_COUNT; ++p)
            for (int c = 0; c < 2; ++c) m.echo[p][c] = echoEP[k][p][c];
        m.reverb = &rvEP[k];
        inst[k].init(48000.f, m);
        inst[k].set_reverb_mix(mixes[k]);
    }
    float l[3], r[3];
    for (int i = 0; i < 48000; ++i)                     // settle: gains + room fill
        for (int k = 0; k < 3; ++k) inst[k].process(nullptr, nullptr, &l[k], &r[k], 1);
    const float g = 0.70710678f;
    double accW = 0.0, acc1 = 0.0, accD = 0.0, acc0 = 0.0;
    for (int i = 0; i < 96000; ++i) {
        for (int k = 0; k < 3; ++k) inst[k].process(nullptr, nullptr, &l[k], &r[k], 1);
        float wet_half = l[1] - g * l[0];
        float dry_half = l[1] - g * l[2];
        accW += wet_half * wet_half;  acc1 += l[2] * l[2];
        accD += dry_half * dry_half;  acc0 += l[0] * l[0];
    }
    CHECK(std::sqrt(accW / acc1) == doctest::Approx(g).epsilon(0.02));
    CHECK(std::sqrt(accD / acc0) == doctest::Approx(g).epsilon(0.02));
}

TEST_CASE("instrument M4.8: hard MIX jumps are smoothed (no zipper)") {
    static float echoZ[PART_COUNT][2][Flux::kMaxSamples];
    static AmbientReverb rvZ;
    auto run_maxd = [&](bool stepped) {
        FxMem m;
        for (int p = 0; p < PART_COUNT; ++p)
            for (int c = 0; c < 2; ++c) m.echo[p][c] = echoZ[p][c];
        m.reverb = &rvZ;
        Instrument inst;
        inst.init(48000.f, m);                 // init() re-clears the shared statics
        float l = 0.f, r = 0.f, prev = 0.f, maxd = 0.f;
        for (int i = 0; i < 96000; ++i) {
            if (stepped && i == 48000) inst.set_reverb_mix(1.f);
            if (stepped && i == 72000) inst.set_reverb_mix(0.f);
            inst.process(nullptr, nullptr, &l, &r, 1);
            if (i > 0) { float d = std::fabs(l - prev); if (d > maxd) maxd = d; }
            prev = l;
        }
        return maxd;
    };
    float steady = run_maxd(false);
    float stepped = run_maxd(true);
    // an unsmoothed 0->1 gain jump would spike the per-sample delta far above
    // the drone's own; the 10 ms glide keeps it in the same ballpark
    CHECK(stepped < 2.f * steady + 0.01f);
}

TEST_CASE("instrument M4.8: MIX 0 sleeps the room, any MIX > 0 wakes it") {
    Instrument fx;
    fx.init(48000.f, test_fx_mem());
    float l, r;
    fx.process(nullptr, nullptr, &l, &r, 1);
    CHECK(!fx.reverb_asleep());            // boot mix 0.25: awake
    fx.set_reverb_mix(0.f);                // runtime fade-out -> sleep
    for (int i = 0; i < 9600; ++i) fx.process(nullptr, nullptr, &l, &r, 1);
    CHECK(fx.reverb_asleep());             // 0.2 s >> the 10 ms glide + snap
    fx.set_reverb_mix(0.4f);
    CHECK(!fx.reverb_asleep());            // waking is immediate
}

TEST_CASE("instrument M4.8: waking from sleep starts with an empty room (no ghost tail)") {
    static float echoGX[PART_COUNT][2][Flux::kMaxSamples];
    static float echoGY[PART_COUNT][2][Flux::kMaxSamples];
    static AmbientReverb rvGX, rvGY;
    FxMem mx, my;
    for (int p = 0; p < PART_COUNT; ++p)
        for (int c = 0; c < 2; ++c) { mx.echo[p][c] = echoGX[p][c]; my.echo[p][c] = echoGY[p][c]; }
    mx.reverb = &rvGX; my.reverb = &rvGY;
    Instrument x; x.init(48000.f, mx);
    Instrument y; y.init(48000.f, my);
    x.set_reverb_decay(0.85f); y.set_reverb_decay(0.85f);  // a surviving ghost would ring loud
    // Y is the reference: sends muted from boot, MIX 0.5 from boot
    y.set_reverb_mix(0.5f);
    for (int p = 0; p < PART_COUNT; ++p) y.set_fx_target_base(p, FXT_REV_SEND, 0.f);
    float xl, xr, yl, yr;
    // phase 1 (1 s): X rings up a loud tail on the default sends
    for (int i = 0; i < 48000; ++i) { x.process(nullptr, nullptr, &xl, &xr, 1);
                                      y.process(nullptr, nullptr, &yl, &yr, 1); }
    // phase 2 (0.5 s): X mutes its sends and goes to sleep at MIX 0
    for (int p = 0; p < PART_COUNT; ++p) x.set_fx_target_base(p, FXT_REV_SEND, 0.f);
    x.set_reverb_mix(0.f);
    for (int i = 0; i < 24000; ++i) { x.process(nullptr, nullptr, &xl, &xr, 1);
                                      y.process(nullptr, nullptr, &yl, &yr, 1); }
    CHECK(x.reverb_asleep());
    // phase 3 (0.5 s settle): X wakes at MIX 0.5; gains glide and snap
    x.set_reverb_mix(0.5f);
    for (int i = 0; i < 24000; ++i) { x.process(nullptr, nullptr, &xl, &xr, 1);
                                      y.process(nullptr, nullptr, &yl, &yr, 1); }
    // both rooms are now empty and unfed, the part streams are identical:
    // any difference left would be X's pre-sleep tail — it must be GONE
    float maxd = 0.f;
    for (int i = 0; i < 24000; ++i) {
        x.process(nullptr, nullptr, &xl, &xr, 1);
        y.process(nullptr, nullptr, &yl, &yr, 1);
        float d = std::fabs(xl - yl); if (d > maxd) maxd = d;
    }
    CHECK(maxd == 0.f);
}

TEST_CASE("instrument M4.8: mix automation incl. sleep is deterministic end to end") {
    auto run = [] {
        static float echoD[PART_COUNT][2][Flux::kMaxSamples];
        static AmbientReverb rvD;
        FxMem m;
        for (int p = 0; p < PART_COUNT; ++p)
            for (int c = 0; c < 2; ++c) m.echo[p][c] = echoD[p][c];
        m.reverb = &rvD;
        Instrument inst;
        inst.init(48000.f, m);            // init() re-clears the shared statics
        std::vector<float> out;
        float l[96], r[96];
        for (int b = 0; b < 500; ++b) {
            if (b == 100) inst.set_reverb_mix(0.8f);
            if (b == 250) inst.set_reverb_mix(0.f);   // sleeps mid-run
            if (b == 400) inst.set_reverb_mix(0.5f);  // wakes again
            inst.process(nullptr, nullptr, l, r, 96);
            for (int i = 0; i < 96; ++i) out.push_back(l[i]);
        }
        return out;
    };
    auto a = run(), b = run();
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) CHECK(a[i] == b[i]);
}

TEST_CASE("instrument: RST restarts both loops at the bar start") {
    // RST (reset_transport) is the bar-resync gesture: zero the downbeat AND
    // snap the lane phases to 0, so the loops restart on the new bar instead
    // of being dragged onto it by the grid servo.
    Instrument inst;
    inst.init(48000.f);
    inst.set_tempo_bpm(120.f);
    inst.set_sync(true);
    inst.set_step(PART_A, true, 12);
    inst.set_step(PART_B, true, 8);
    std::vector<float> l(1), r(1);
    for (int i = 0; i < 48000; ++i)                  // run ~1 s into the pattern
        inst.process(nullptr, nullptr, l.data(), r.data(), 1);
    inst.reset_transport();
    inst.process(nullptr, nullptr, l.data(), r.data(), 1);
    // the very next sample fires step 0 on both parts — a fresh downbeat
    CHECK(inst.lane_fired(PART_A, LANE_PITCH));
    CHECK(inst.lane_fired(PART_B, LANE_PITCH));
}

TEST_CASE("instrument: set_color blooms the FLOW pad live, click-free") {
    Instrument inst;
    inst.init(48000.f);
    inst.set_density(0, 0.f);                      // no lane fires: pure drone
    float outL[64], outR[64];
    for (int i = 0; i < 750; ++i)                  // 1 s warmup: drone settles
        inst.process(nullptr, nullptr, outL, outR, 64);
    CHECK(inst.active_voices(0) >= 1);
    inst.set_color(0, 0.7f);                       // knob turned up, NO trigger
    float max_step = 0.f, prev = 0.f;
    bool first = true;
    for (int i = 0; i < 1500; ++i) {               // 2 s
        inst.process(nullptr, nullptr, outL, outR, 64);
        for (int k = 0; k < 64; ++k) {
            if (!first && std::fabs(outL[k] - prev) > max_step)
                max_step = std::fabs(outL[k] - prev);
            prev = outL[k];
            first = false;
        }
    }
    CHECK(inst.active_voices(0) >= 3);             // the pad bloomed
    CHECK(max_step < 0.5f);                        // no hard discontinuity
}

TEST_CASE("instrument: COLOR 0 stays bit-deterministic") {
    Instrument a, b;
    a.init(48000.f);
    b.init(48000.f);
    b.set_color(0, 0.f);                           // explicit 0 == untouched
    b.set_color(1, 0.f);
    float al[64], ar[64], bl[64], br[64];
    for (int i = 0; i < 1500; ++i) {
        a.process(nullptr, nullptr, al, ar, 64);
        b.process(nullptr, nullptr, bl, br, 64);
        for (int k = 0; k < 64; ++k) {
            CHECK(al[k] == bl[k]);                 // exact
            CHECK(ar[k] == br[k]);
        }
    }
}

TEST_CASE("instrument: control raster survives a block-size-agnostic call pattern") {
    // The raster lives in Part and advances per sample, so rendering the same
    // audio in 96-sample blocks and in 7-sample blocks must give the same
    // samples. If anything ever ties the tick to the host block boundary,
    // this is what catches it.
    auto render = [](size_t chunk, std::vector<float>& out) {
        Instrument inst;
        inst.init(48000.f);
        inst.set_tempo_bpm(120.f);
        for (int p = 0; p < PART_COUNT; ++p) {
            inst.set_depth(p, 1.f);
            inst.set_rate(p, 0.8f);
        }
        out.assign(4800, 0.f);
        std::vector<float> r(4800, 0.f);
        for (size_t i = 0; i < 4800; i += chunk) {
            const size_t n = std::min(chunk, size_t(4800) - i);
            inst.process(nullptr, nullptr, out.data() + i, r.data() + i, n);
        }
    };
    std::vector<float> a, b;
    render(96, a);
    render(7, b);
    for (size_t i = 0; i < a.size(); ++i) REQUIRE(a[i] == b[i]);
}

TEST_CASE("instrument: set_tempo_bpm guards a non-positive or non-finite bpm at the single door") {
    // Instrument::set_tempo_bpm is the one call both SuperModulator::
    // set_tempo_bpm and Flux::set_bpm go through -- both keep their own _bpm
    // and bypass Transport entirely, so guarding only Transport::set_bpm (as
    // it already was) left this door open: host/render/scenario.cpp forwards
    // an unvalidated scenario-file `bpm` straight into this method (task 12
    // finding 2). A guarded call must be a complete no-op, so a reference
    // instrument that only ever receives the one valid bpm is compared,
    // sample for sample, against a "dut" that also receives interleaved bad
    // calls -- the same bit-exact idiom already used for FLUX's other
    // unchanged-value guards (I3, see flux.h). SYNC is on so SuperModulator
    // actually divides by bpm (division_hz), which is the path that reacts.
    auto setup = [](Instrument& inst) {
        inst.init(48000.f);
        inst.set_sync(true);
        inst.set_tempo_bpm(120.f);
        inst.set_target_active(PART_A, LANE_PITCH, true);
        inst.set_rate(PART_A, 0.5f);
        inst.set_range(PART_A, 1.f);
    };
    Instrument ref, dut;
    setup(ref);
    setup(dut);

    std::vector<float> rl(96), rr(96), dl(96), dr(96);
    for (int block = 0; block < 50; ++block) {
        if (block == 10) {
            dut.set_tempo_bpm(0.f);
            dut.set_tempo_bpm(-30.f);
            dut.set_tempo_bpm(std::numeric_limits<float>::quiet_NaN());
        }
        ref.process(nullptr, nullptr, rl.data(), rr.data(), 96);
        dut.process(nullptr, nullptr, dl.data(), dr.data(), 96);
        for (int i = 0; i < 96; ++i) {
            REQUIRE(std::isfinite(dl[i]));
            REQUIRE(dl[i] == rl[i]);
            REQUIRE(dr[i] == rr[i]);
        }
    }
}

#include <doctest/doctest.h>
#include <cmath>
#include <vector>
#include "instrument.h"
#include "sampler/sampler_config.h"
using namespace spky;

static constexpr size_t kSFrames = 48000;   // 1 s per part is plenty for tests

struct InstRig {
    std::vector<float> echo[PART_COUNT][2];
    std::vector<SampleBuffer::Frame> sbuf[PART_COUNT];
    AmbientReverb reverb;
    FxMem mem;
    Instrument inst;

    InstRig() {
        for (int p = 0; p < PART_COUNT; ++p) {
            for (int c = 0; c < 2; ++c) {
                echo[p][c].assign(Flux::kMaxSamples, 0.f);
                mem.echo[p][c] = echo[p][c].data();
            }
            sbuf[p].assign(kSFrames, SampleBuffer::Frame{ 0.f, 0.f });
            mem.sampler_buf[p] = sbuf[p].data();
        }
        mem.sampler_frames = kSFrames;
        mem.reverb = &reverb;
        inst.init(48000.f, mem);
    }
    // Render n samples with a constant input on both channels.
    std::vector<float> render(int n, float in = 0.f) {
        std::vector<float> out(n);
        for (int i = 0; i < n; ++i) {
            float l = 0.f, r = 0.f;
            inst.process(&in, &in, &l, &r, 1);
            out[i] = l;
        }
        return out;
    }
};

static float rms_of(const std::vector<float>& v, size_t from, size_t n) {
    double acc = 0.0;
    for (size_t i = from; i < from + n && i < v.size(); ++i) acc += double(v[i]) * v[i];
    return float(std::sqrt(acc / double(n)));
}

TEST_CASE("part: ENGINE_SAMPLER is selectable and records from the input") {
    InstRig g;
    g.inst.set_engine(PART_A, ENGINE_SAMPLER);
    // The click-free swap needs a few ms to complete.
    g.render(960);
    CHECK(g.inst.engine_id(PART_A) == ENGINE_SAMPLER);

    REQUIRE(g.inst.sampler_fill(PART_A) == doctest::Approx(0.f));
    g.inst.sampler_record(PART_A, true);
    // Feed a DC 0.5 in; the buffer must fill.
    g.render(24000, 0.5f);
    g.inst.sampler_record(PART_A, false);
    g.render(480);
    CHECK(g.inst.sampler_fill(PART_A) > 0.4f);
    // fill() only counts frames WRITTEN, not their content -- recording
    // silence into the buffer fills it exactly as "well" as recording the
    // real input (SampleBuffer::write() is called every sample once armed,
    // regardless of what it is handed). So fill() alone cannot tell a
    // correctly-threaded input from a mistakenly zeroed one; read the
    // test-owned backing memory directly, well past the 192-sample record
    // fade-in, to prove the DC value itself made it into the buffer.
    REQUIRE(g.sbuf[PART_A].size() > 5000);
    CHECK(g.sbuf[PART_A][5000].l == doctest::Approx(0.5f));
    CHECK(g.sbuf[PART_A][5000].r == doctest::Approx(0.5f));
}

TEST_CASE("part: input reaches ONLY the sampler engine, and only when routed") {
    InstRig g;
    // Part B stays on the synth. Recording is armed on A only.
    g.inst.set_engine(PART_A, ENGINE_SAMPLER);
    g.render(960);
    g.inst.sampler_record(PART_A, true);
    g.render(9600, 0.5f);
    g.inst.sampler_record(PART_A, false);
    CHECK(g.inst.sampler_fill(PART_A) > 0.f);
    CHECK(g.inst.sampler_fill(PART_B) == doctest::Approx(0.f));
}

TEST_CASE("part: a nullptr sampler buffer leaves the part silent, not crashing") {
    std::vector<float> echo[PART_COUNT][2];
    AmbientReverb reverb;
    FxMem mem;
    for (int p = 0; p < PART_COUNT; ++p)
        for (int c = 0; c < 2; ++c) {
            echo[p][c].assign(Flux::kMaxSamples, 0.f);
            mem.echo[p][c] = echo[p][c].data();
        }
    mem.reverb = &reverb;                       // sampler_buf left nullptr
    Instrument inst;
    inst.init(48000.f, mem);
    inst.set_engine(PART_A, ENGINE_SAMPLER);
    inst.set_engine(PART_B, ENGINE_SAMPLER);
    const float in = 0.5f;
    for (int i = 0; i < 24000; ++i) {
        float l = 0.f, r = 0.f;
        inst.process(&in, &in, &l, &r, 1);
        REQUIRE(std::isfinite(l));
        REQUIRE(std::isfinite(r));
    }
    CHECK(inst.sampler_fill(PART_A) == doctest::Approx(0.f));
}

TEST_CASE("part: the composed gate reaches the sampler in STEP") {
    InstRig g;
    // Part B stays on its ENGINE_SYNTH boot default, a standing FLOW drone
    // (Part::init: "lanes boot in FLOW -> drone") that fluctuates with its
    // own modulators. Left alone, its bleed into the master mix is enough
    // to satisfy the hi/lo spread below on its own, making the assertions
    // pass whether or not PART_A's sampler is chopping at all -- silence it
    // the same way every render scenario does when isolating one part.
    g.inst.set_target_active(PART_B, LANE_LEVEL, false);
    g.inst.set_target_base(PART_B, LANE_LEVEL, 0.f);
    g.inst.set_engine(PART_A, ENGINE_SAMPLER);
    g.render(960);
    // Load content so the cloud has something to play.
    std::vector<float> tone(24000);
    for (size_t i = 0; i < tone.size(); ++i)
        tone[i] = std::sin(6.2831853f * 300.f * float(i) / 48000.f);
    g.inst.load_sample(PART_A, tone.data(), tone.data(), tone.size());

    g.inst.set_step(PART_A, true, 8);
    g.inst.set_target_base(PART_A, LANE_LEVEL, 1.f);
    auto v = g.render(48000 * 4);
    // A gated texture is neither silent nor continuous: it has loud and
    // quiet stretches. Measure the spread of 50 ms RMS windows.
    float lo = 1e9f, hi = 0.f;
    for (size_t i = 4800; i + 2400 < v.size(); i += 2400) {
        const float e = rms_of(v, i, 2400);
        if (e < lo) lo = e;
        if (e > hi) hi = e;
    }
    CHECK(hi > 0.005f);          // it sounds
    CHECK(lo < 0.4f * hi);       // ...and it is chopped, not a standing cloud

    // Swap re-sync: forcing an engine swap while a note is held must
    // re-push the CURRENT gate to the freshly active engine (part.cpp's
    // swap block), the same way it already re-pushes flow/hold/cycle.
    // Without that push the sampler is left believing the gate is still
    // low, so it silently swallows the next edge too -- its own set_gate
    // no-ops on "already at this value" (engine_iface.h) -- and stays
    // silent until the NEXT natural STEP fire.
    //
    // A fresh rig, on ENGINE_SYNTH (the boot default) and never yet
    // switched to the sampler, so there is no leftover grain tail from the
    // render above to confound the read. Material is loaded and STEP armed
    // up front so the sampler is ready to sound the instant it activates.
    // A manual gate pulse (5 ms = 240 samples) is opened on the SYNTH,
    // outliving the swap's ~4 ms (192-sample) fade-out, then the part is
    // switched to the sampler while that pulse is still open: the gate
    // never actually CHANGES value across the swap, so the per-sample edge
    // detector (unlike the swap block) has nothing to forward either way --
    // the re-push is the only path that can inform the freshly active
    // engine. The fade-out zeroes the synth's own note before the swap
    // completes (part.cpp's `fade` factor), so any sound from sample 192
    // on can only be the sampler's.
    //
    // The observable is active_grains(), not the audio itself: the swap's
    // fade-in is still near-zero gain this close to the swap point, and the
    // reverb tail from the synth's own pre-swap note dwarfs a single quiet
    // grain in the raw signal -- active_grains() observes the scheduler
    // directly, with neither confound.
    InstRig g2;
    g2.inst.set_target_active(PART_B, LANE_LEVEL, false);
    g2.inst.set_target_base(PART_B, LANE_LEVEL, 0.f);
    g2.inst.load_sample(PART_A, tone.data(), tone.data(), tone.size());
    g2.inst.set_step(PART_A, true, 8);
    g2.inst.trigger_manual(PART_A);
    g2.inst.set_engine(PART_A, ENGINE_SAMPLER);
    g2.render(250);   // covers the ~192-sample fade-out/swap plus slack
    CHECK(g2.inst.sampler_grains(PART_A) > 0);
}

TEST_CASE("part: engine switch synth <-> sampler is click-free") {
    InstRig g;
    std::vector<float> tone(24000);
    for (size_t i = 0; i < tone.size(); ++i) tone[i] = 0.8f;
    g.inst.set_engine(PART_A, ENGINE_SAMPLER);
    g.render(960);
    g.inst.load_sample(PART_A, tone.data(), tone.data(), tone.size());
    auto a = g.render(9600);

    g.inst.set_engine(PART_A, ENGINE_SYNTH);
    auto b = g.render(9600);
    g.inst.set_engine(PART_A, ENGINE_SAMPLER);
    auto c = g.render(9600);

    // No step anywhere across either swap.
    for (size_t i = 1; i < b.size(); ++i) CHECK(std::fabs(b[i] - b[i - 1]) < 0.25f);
    for (size_t i = 1; i < c.size(); ++i) CHECK(std::fabs(c[i] - c[i - 1]) < 0.25f);
}

TEST_CASE("part: voice-row edits stick on the sampler while the synth is active") {
    InstRig g;
    // Part B stays on its ENGINE_SYNTH boot default, which is a standing
    // FLOW drone (Part::init: "lanes boot in FLOW -> drone") -- left alone
    // it would bleed into the master mix and defeat the near-silence
    // assertion below, on a part this test has no interest in. Every render
    // scenario silences the part it isn't exercising the same way.
    g.inst.set_target_active(PART_B, LANE_LEVEL, false);
    g.inst.set_target_base(PART_B, LANE_LEVEL, 0.f);
    // Set FILT hard left while the SYNTH is the active engine...
    g.inst.set_voice_filt(PART_A, -1.f);
    g.inst.set_engine(PART_A, ENGINE_SAMPLER);
    g.render(960);
    std::vector<float> tone(24000, 0.8f);
    g.inst.load_sample(PART_A, tone.data(), tone.data(), tone.size());
    g.inst.set_target_base(PART_A, LANE_LEVEL, 1.f);
    auto v = g.render(24000);
    // ...and the sampler must already be silent when it comes up.
    CHECK(rms_of(v, 12000, 4800) < 0.005f);
}

TEST_CASE("part: CHOKE holds a sampler drone like a synth drone") {
    InstRig g;
    g.inst.set_engine(PART_B, ENGINE_SAMPLER);
    g.render(960);
    std::vector<float> tone(24000, 0.6f);
    g.inst.load_sample(PART_B, tone.data(), tone.data(), tone.size());
    g.inst.set_target_base(PART_B, LANE_LEVEL, 1.f);
    // Full A priority: B must duck whenever A holds.
    g.inst.set_choke(-1.f);
    auto v = g.render(48000 * 3);
    float lo = 1e9f, hi = 0.f;
    for (size_t i = 4800; i + 2400 < v.size(); i += 2400) {
        const float e = rms_of(v, i, 2400);
        if (e < lo) lo = e;
        if (e > hi) hi = e;
    }
    CHECK(hi > 0.005f);
    CHECK(lo < 0.6f * hi);       // the cloud really ducks
}

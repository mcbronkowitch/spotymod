#include <doctest/doctest.h>
#include <cmath>
#include <vector>
#include "instrument.h"
#include "mod/rng.h"
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

TEST_CASE("sampler rng: MOTION-scatter stream is decorrelated from mod lane 0") {
    // Part::init (part.cpp:15,19) seeds SuperModulator's lane 0 with
    // seed_base + 0 * 2654435761u == seed_base (super_modulator.cpp:15),
    // and seeds the sampler's Rng with seed_base ^ <a constant>
    // (SamplerEngine::init, sampler_engine.cpp). Both are the same
    // xorshift32 generator (mod/rng.h). A Task-6-review regression had the
    // sampler XOR in the SAME constant Part::init itself XORs seed_base
    // with before handing it to SamplerEngine::set_seed -- which cancels,
    // leaving the sampler seeded with seed_base exactly, bit-identical to
    // lane 0. Left uncaught, MOTION's grain-position jitter, pan, and
    // octave-scatter draws would silently retrace lane 0's own PITCH
    // modulation every time -- the exact correlation the decorrelation
    // constant exists to prevent, invisible in any single render because
    // both streams individually look like ordinary noise.
    //
    // This is a correlation property, not a correctness one: neither stream
    // is "wrong" in isolation, so a reader unaware of that history may look
    // at this test and call it trivial. It is not -- it is the only check
    // in the suite that would catch the two constants colliding again.
    const uint32_t seed_base = 0xabcd1234u;
    Rng lane0;
    lane0.seed(seed_base);                      // SuperModulator lane 0 (i == 0)
    Rng sampler_rng;
    sampler_rng.seed(seed_base ^ 0xC0FFEE11u);  // SamplerEngine::init's constant

    bool differed = false;
    for (int i = 0; i < 256; ++i) {
        if (lane0.next_u32() != sampler_rng.next_u32()) { differed = true; break; }
    }
    CHECK(differed);
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

TEST_CASE("part: input reaches a part's SamplerEngine only while that part is routed to it") {
    // Every Part owns BOTH a SynthEngine and a SamplerEngine (part.h), and
    // Instrument::sampler_record()/sampler_fill() always address a part's
    // _sampler member directly, regardless of which engine is currently
    // active (instrument.h). Part::process(), however, only ever calls
    // process_in() on the ACTIVE engine (part.cpp: `_engine->process_in(...)`,
    // where `_engine` tracks set_engine()). So a part's SamplerEngine can
    // only actually receive input while that part is routed to
    // ENGINE_SAMPLER -- arming its recording is necessary but not
    // sufficient.
    //
    // The previous version of this test left B's recording UNARMED, so
    // sampler_fill(PART_B) == 0 was guaranteed by the recording gate alone
    // (SampleBuffer::write no-ops unless recording) and said nothing about
    // routing -- it would have passed identically even if input reached
    // every part's sampler unconditionally. Arming B's recording too, while
    // leaving B on the synth, removes that confound: B's sampler is now
    // willing to record, so a fill > 0 here could only be explained by input
    // reaching a part whose active engine is not the sampler -- the bug this
    // test claims to guard against.
    InstRig g;
    g.inst.set_engine(PART_A, ENGINE_SAMPLER);   // A routed to the sampler...
    g.render(960);
    g.inst.sampler_record(PART_A, true);
    g.inst.sampler_record(PART_B, true);         // ...B stays on the synth, but arm it too
    g.render(9600, 0.5f);
    g.inst.sampler_record(PART_A, false);
    g.inst.sampler_record(PART_B, false);
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
}

// SLICE-GROOVE AUDIT (Task 9). Split out of "part: the composed gate reaches
// the sampler in STEP" above, whose first half still holds. This half asserts
// that re-pushing the CURRENT gate at an engine swap makes the sampler sound
// -- true only while a gate EDGE spawned a grain, which Task 5 removed: a
// STEP grain now comes from trigger()/trigger_chord(), never from the gate.
// The Part-side requirement (the swap must re-push gate/flow/hold/cycle to
// the freshly active engine) is unchanged and still worth a test; it just
// needs an observable that survives the new STEP core. Task 9 settles it.
TEST_CASE("part: an engine swap re-pushes the held gate to the sampler"
          * doctest::skip(true)) {
    std::vector<float> tone(24000);
    for (size_t i = 0; i < tone.size(); ++i)
        tone[i] = std::sin(6.2831853f * 300.f * float(i) / 48000.f);

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
    // Part A stays on its ENGINE_SYNTH boot-default FLOW drone, exactly the
    // way test_choke.cpp's arm_both()/"choke -1: the yielding FLOW drone
    // ducks out and comes back" leaves it. Left at full LEVEL and never
    // silenced, A's own drone alone satisfies an instrument-level hi/lo RMS
    // spread whether or not B's sampler is ducking at all -- that was this
    // case's bug (Task 6 review). Muting A's LEVEL target removes that
    // confound; it does not touch gate()/flow(), which is what the CHOKE
    // priority window (instrument.cpp) actually reads, so A still "holds"
    // for CHOKE purposes.
    //
    // The observable is sampler_grains(PART_B), not instrument-level RMS:
    // it reads the cloud's own scheduler state directly, with no dependence
    // on A's contribution to the mix, the reverb tail, or the FILT/LEVEL
    // chain -- the same reasoning the STEP-gate case above uses for
    // active_grains().
    InstRig g;
    g.inst.set_engine(PART_B, ENGINE_SAMPLER);
    g.render(960);
    std::vector<float> tone(24000, 0.6f);
    g.inst.load_sample(PART_B, tone.data(), tone.data(), tone.size());
    g.inst.set_target_base(PART_B, LANE_LEVEL, 1.f);

    g.inst.set_target_active(PART_A, LANE_LEVEL, false);
    g.inst.set_target_base(PART_A, LANE_LEVEL, 0.f);

    g.render(24000);                 // let B's cloud establish, unchoked (default choke = 0)
    CHECK(g.inst.sampler_grains(PART_B) > 0);

    g.inst.set_choke(-1.f);          // full A priority: B must duck while A holds
    g.render(48000);                 // A's FLOW drone holds forever, so this decays out and stays out
    CHECK(g.inst.sampler_grains(PART_B) == 0);

    g.inst.set_choke(0.f);           // floor free again
    g.render(48000);
    CHECK(g.inst.sampler_grains(PART_B) > 0);   // the cloud re-arms and comes back
}

// The four cases below use a bare Part rather than InstRig: Part::init takes
// its own sampler_mem/sampler_frames pair directly (part.h), so a real
// vector-backed buffer plugs straight in without going through Instrument.
// That matters here because the brief's original draft called
// `Part p; p.init(48000.f, 0);` with no memory at all -- SamplerEngine::
// set_memory(nullptr, 0) leaves _buf empty, so SampleBuffer::is_empty() is
// true forever and SamplerEngine::_spawn_one() returns before spawning
// anything (sampler_engine.cpp). Every test that counts grains would measure
// nothing. Giving each Part its own kSFrames-sized backing vector (the same
// pattern InstRig uses for its per-part sbuf[]) fixes that without needing
// the full Instrument.

TEST_CASE("sampler part: the MOTION lane breathes the grain overlap") {
    // DENS would otherwise be the deck's only completely static control --
    // it hangs off no lane and no jack. This mirrors how MOTION already
    // reaches COLOR (part.cpp:129-134).
    std::vector<SampleBuffer::Frame> sbuf(kSFrames, SampleBuffer::Frame{ 0.f, 0.f });
    Part p;
    p.init(48000.f, 0, nullptr, nullptr, sbuf.data(), sbuf.size());
    p.set_engine(ENGINE_SAMPLER);
    p.set_sampler_overlap(0.5f);
    p.set_depth(1.f);
    p.set_target_active(LANE_MOTION, true);

    float lo = 2.f, hi = -2.f;
    for (int i = 0; i < 48000; ++i) {
        float a = 0.f, b = 0.f;
        p.process(a, b);
        const float e = p.overlap_eff();
        if (e < lo) lo = e;
        if (e > hi) hi = e;
    }
    CHECK(hi > lo + 0.02f);           // it actually moves
    CHECK(lo >= 0.f);
    CHECK(hi <= 1.f);
}

TEST_CASE("sampler part: an inactive MOTION target leaves the overlap on the knob") {
    std::vector<SampleBuffer::Frame> sbuf(kSFrames, SampleBuffer::Frame{ 0.f, 0.f });
    Part p;
    p.init(48000.f, 0, nullptr, nullptr, sbuf.data(), sbuf.size());
    p.set_engine(ENGINE_SAMPLER);
    p.set_sampler_overlap(0.4f);
    p.set_target_active(LANE_MOTION, false);
    for (int i = 0; i < 4800; ++i) { float a = 0.f, b = 0.f; p.process(a, b); }
    CHECK(p.overlap_eff() == doctest::Approx(0.4f));
}

TEST_CASE("sampler part: a deactivated PITCH lane holds pitch but keeps firing") {
    // The whole point of the pitch decision: material and a synth deck stay
    // in one key, WITHOUT losing rhythmic triggering. _active gates the
    // VALUE (part.cpp:44-57); lane_fired is independent of it (part.cpp:194),
    // which is why STEP still triggers.
    std::vector<SampleBuffer::Frame> sbuf(kSFrames, SampleBuffer::Frame{ 0.f, 0.f });
    Part p;
    p.init(48000.f, 0, nullptr, nullptr, sbuf.data(), sbuf.size());
    p.set_engine(ENGINE_SAMPLER);
    p.set_step(true, 8);
    p.set_depth(1.f);
    p.set_target_active(LANE_PITCH, false);

    const float pitch0 = p.target_raw(LANE_PITCH);
    bool fired_at_least_once = false;
    for (int i = 0; i < 48000 * 4; ++i) {
        float a = 0.f, b = 0.f;
        p.process(a, b);
        if (p.mod().lane_fired(LANE_PITCH)) fired_at_least_once = true;
        CHECK(p.target_raw(LANE_PITCH) == pitch0);   // exactly, never drifting
    }
    CHECK(fired_at_least_once);
}

TEST_CASE("sampler part: punch() produces a grain in the FLOW cloud") {
    // Regression pin for an M5b defect this plan fixes as a side effect:
    // trigger_manual only latches _burst_ratio, and _next_ratio reads that
    // latch solely when !_flow (sampler_engine.cpp:247) -- so the pad has
    // been inert in the standing cloud. Renamed from the brief's "TRIG
    // produces a grain in the FLOW cloud": this calls punch() directly, not
    // through the Task 6 host/panel wiring (TRIG isn't connected to it yet),
    // so a reader later must not mistake this for proof the panel path works.
    std::vector<SampleBuffer::Frame> sbuf(kSFrames, SampleBuffer::Frame{ 0.f, 0.f });
    Part p;
    p.init(48000.f, 0, nullptr, nullptr, sbuf.data(), sbuf.size());
    p.set_engine(ENGINE_SAMPLER);
    std::vector<float> tone(24000);
    for (size_t i = 0; i < tone.size(); ++i)
        tone[i] = std::sin(6.2831853f * 300.f * float(i) / 48000.f);
    p.sampler().load_sample(tone.data(), tone.data(), tone.size());
    p.set_step(false, 8);                   // FLOW
    for (int i = 0; i < 4800; ++i) { float a = 0.f, b = 0.f; p.process(a, b); }

    // Task 4 review, Befund 1: at this grain length/overlap the natural
    // spawn_every lands around 2,300 samples -- an 400-sample observation
    // window (the original count here) is well inside that, so
    // spawn_count() > before would trip on the very same schedule with or
    // without punch(), and the test would prove nothing about punch()
    // specifically. punch() zeroes _spawn_ctr (sampler_engine.cpp), which
    // the scheduler decrements and checks against <= 0.f, so a forced spawn
    // always lands on the very next process() call. Shrinking the window to
    // 8 samples means only a punch()-forced spawn can fit; the REQUIRE below
    // pins the margin so a future change to grain length/overlap that shrinks
    // spawn_every can't silently let the natural schedule creep back inside
    // an 8-sample window without this test noticing.
    REQUIRE(p.sampler().spawn_interval_samples() > 100.f);

    const int before = p.sampler().spawn_count();
    p.sampler().punch();                    // what NEW/TRIG will call
    for (int i = 0; i < 8; ++i) { float a = 0.f, b = 0.f; p.process(a, b); }
    CHECK(p.sampler().spawn_count() > before);
}

TEST_CASE("sampler part: SUB and DTUN no longer reach the sampler") {
    // They are GENE SIZE and ORGANIZE on the panel now (spec 2026-07-21
    // morphagene-controls). The sampler's own sub/detune stay at their
    // silent defaults, and the synth keeps both.
    //
    // Task 4 review, Befund 2: set_voice_sub/set_voice_detune (part.h) now
    // forward ONLY to _synth.set_sub()/_synth.set_detune() -- the sampler
    // leg below is only half this case pinned. Deleting BOTH forwarding
    // calls (not just the sampler one) would still pass every CHECK that
    // used to be here, because nothing observed the synth leg.
    //
    // Final-fixes pass, Befund B: closed. SynthEngine now exposes
    // sub_level()/detune_max_ct() (engine/synth/synth_engine.h) and Part
    // exposes synth() alongside the existing sampler() (engine/parts/
    // part.h), purely as observers -- neither changes what either engine
    // does. The two CHECKs below pin that a future edit deleting both
    // forwarding calls at once (not just the sampler-silencing one) would
    // now be caught here.
    Part p;
    p.init(48000.f, 0);
    p.set_voice_sub(1.f);
    p.set_voice_detune(1.f);
    CHECK(p.sampler().sub() == doctest::Approx(0.f));
    CHECK(p.sampler().detune() == doctest::Approx(0.f));
    CHECK(p.synth().sub_level() == doctest::Approx(1.f));
    CHECK(p.synth().detune_max_ct() == doctest::Approx(SynthEngine::kDetuneCeilCt));
}

TEST_CASE("part: the sampler does not quantize its pitch, the synth still does") {
    // The centre detent of TUNE must play a recording back at exactly its
    // original pitch. It did not: 0.5 of the 36-semitone pitch span is exactly
    // 18 semitones -- a tritone above the root -- which most scales do not
    // contain, so Quantizer::process snapped it to a neighbouring degree.
    // Three of the eight scales pulled the detent a semitone flat, one pushed
    // it a semitone sharp, and only four left it alone. Recorded material
    // played back out of tune against its own source, and the direction
    // depended on where SCALE happened to sit.
    //
    // The sampler's TUNE is a whole-recording transposition, not a melody, so
    // it is not quantized at all. The synth's must still be.
    for (int scale = 0; scale < 8; ++scale) {
        InstRig g;
        g.inst.set_scale(scale);
        g.inst.set_tune(PART_A, 0.5f);
        g.inst.set_tune(PART_B, 0.5f);
        g.inst.set_target_active(PART_A, LANE_PITCH, false);
        g.inst.set_depth(PART_A, 0.f);
        g.inst.set_target_active(PART_B, LANE_PITCH, false);
        g.inst.set_depth(PART_B, 0.f);

        g.inst.set_engine(PART_A, ENGINE_SAMPLER);   // A: sampler
        g.inst.set_engine(PART_B, ENGINE_SYNTH);     // B: synth, the control
        g.render(2000);                              // past the click-free swap

        // Unity: pitch 0.5 maps to ratio 8^(0.5-0.5) == 1.0 exactly. Compared
        // exactly, not with Approx -- "close to unity" is what the quantizer
        // already delivered on four of the eight scales, and the whole point
        // is that the detent is unity on ALL of them.
        CHECK(g.inst.pitch_cv(PART_A) == 0.5f);
    }

    // The synth leg, on a scale that provably moves the detent (scale 0 pulled
    // it to 17/36 in the measurement above). Without this, deleting the
    // quantizer call outright would still pass everything above.
    InstRig g;
    g.inst.set_scale(0);
    g.inst.set_tune(PART_B, 0.5f);
    g.inst.set_target_active(PART_B, LANE_PITCH, false);
    g.inst.set_depth(PART_B, 0.f);
    g.inst.set_engine(PART_B, ENGINE_SYNTH);
    g.render(2000);
    CHECK(g.inst.pitch_cv(PART_B) != doctest::Approx(0.5f));
    CHECK(g.inst.pitch_cv(PART_B) == doctest::Approx(17.f / 36.f));
}

TEST_CASE("part: the sampler granulates at ONE pitch whatever COLOR says") {
    // COLOR builds a chord, and the sampler's cloud spreads a chord round-robin
    // across grains -- one grain per note, cycling. On a synth that is a chord.
    // On a granulated recording it is the same material replayed at several
    // transpositions at once: a harmoniser, heard as grains jumping octaves.
    //
    // COLOR_A ships at 0.647 ("pad blooms into a seventh/ninth stack"), a synth
    // decision, so a freshly flipped deck A granulated at four ratios spanning
    // nearly two octaves without the user touching anything. COLOR is not part
    // of the sampler's control surface -- it reached pitch through the chord
    // surface the way MOTION reached it through the octave scatter.
    //
    // The deck now plays exactly one pitch: the PITCH target, which with the
    // lane off is TUNE alone.
    for (float color : {0.f, 0.35f, 0.647f, 1.f}) {
        InstRig g;
        g.inst.set_engine(PART_A, ENGINE_SAMPLER);
        g.inst.set_target_active(PART_A, LANE_PITCH, false);
        g.inst.set_depth(PART_A, 0.f);
        g.inst.set_tune(PART_A, 0.5f);
        g.inst.set_color(PART_A, color);
        g.render(2000);                       // past the click-free engine swap

        // Give it material, then let the cloud run and watch every spawn.
        std::vector<float> buf(24000);
        for (size_t i = 0; i < buf.size(); ++i)
            buf[i] = std::sin(6.2831853f * 441.f * float(i) / 48000.f);
        g.inst.load_sample(PART_A, buf.data(), buf.data(), buf.size());

        std::vector<float> seen;
        float prev = -1.f;
        for (int i = 0; i < 48000 * 8; ++i) {
            g.render(1);
            const float cur = g.inst.sampler_last_spawn_ratio(PART_A);
            if (cur != prev) {
                prev = cur;
                bool known = false;
                for (float s : seen) if (s == cur) known = true;
                if (!known) seen.push_back(cur);
            }
        }
        INFO("COLOR = ", color);
        // One ratio, and it is unity: TUNE sits at its centre detent.
        REQUIRE(seen.size() == 1);
        CHECK(seen[0] == doctest::Approx(1.f));
    }

    // The synth must keep its chord. Without this, flattening unconditionally
    // -- which would pass every CHECK above -- goes unnoticed.
    InstRig g;
    g.inst.set_engine(PART_B, ENGINE_SYNTH);
    g.inst.set_color(PART_B, 1.f);
    g.render(2000);
    CHECK(g.inst.synth_chord_n(PART_B) > 1);
}

// --- Review 2026-07-22: MOTION pinnt den Sampler nicht mehr ---

TEST_CASE("F-04: ORGANIZE reaches the spawn position on a sampler deck") {
    // LANE_MOTION hat die Basis 0.5, und niemand schreibt sie -- weder Host
    // noch Instrument. Der Positions-Scatter ist damit +-content und die
    // Spawn-Position exakt gleichverteilt, egal was ORGANIZE sagt. Der Test
    // misst bei MOD = 0, wo gar kein Scatter sein darf.
    //
    // A single last_spawn_pos() read after the render is not enough: pre-fix,
    // that position is uniform over the whole buffer (kSFrames = 48000), and
    // the pass window here is Approx(want).epsilon(0.02), i.e. roughly
    // +-864 around want ~= 43199 -- about 3.6% of the 48000-wide range. A
    // uniform draw lands inside a 3.6%-wide window by pure chance about 1
    // time in 28, so a single sample is nowhere near enough to tell "fixed"
    // from "unfixed": it would pass on the broken code about as often as it
    // fails on the fixed one, purely on which RNG seed the run happens to
    // draw. That is exactly what happened during Task 3 -- this case only
    // went red because that particular seed happened to land outside the
    // window; a different seed would have shipped the bug silently.
    //
    // Collecting every spawn across the whole render and requiring ALL of
    // them inside the window (the same shape the MOD sibling below uses to
    // track lo/hi) closes that hole: with the fix, MOD = 0 means every spawn
    // reads the exact same centre (no scatter at all), so every one of them
    // must land in the window. A still-uniform (unfixed) distribution would
    // need EVERY collected spawn to land in the same 3.6% sliver to slip
    // past -- for more than a handful of spawns that is not a realistic
    // false pass, unlike the single-draw version above.
    // A bare Part, not InstRig: this needs sampler().spawn_count(), the
    // engine's own cumulative counter (already used by the punch() test
    // above), to detect each real spawn. last_spawn_pos() itself cannot do
    // that job here -- it holds its value between spawns, and WITH the fix
    // MOD = 0 means every spawn reads the exact same centre (no scatter at
    // all, which is the whole point). So consecutive spawns are bit-identical
    // and a "value changed" test would see only the very first one and then
    // go quiet, undercounting real spawns down to one -- checked empirically
    // while building this test. spawn_count() has no such blind spot: it
    // increments once per spawn regardless of whether the position moved.
    std::vector<SampleBuffer::Frame> sbuf(kSFrames, SampleBuffer::Frame{ 0.f, 0.f });
    Part p;
    p.init(48000.f, 0, nullptr, nullptr, sbuf.data(), sbuf.size());
    p.set_engine(ENGINE_SAMPLER);
    p.set_depth(0.f);                    // MOD = 0

    std::vector<float> l(kSFrames), r(kSFrames);
    for (size_t i = 0; i < kSFrames; ++i) {
        l[i] = std::sin(6.2831853f * 220.f * float(i) / 48000.f);
        r[i] = l[i];
    }
    p.sampler().load_sample(l.data(), r.data(), kSFrames);

    // ORGANIZE ans obere Ende: alle Spawns muessen dort landen.
    p.set_target_base(LANE_SOURCE, 0.9f);

    int last_count = p.sampler().spawn_count();
    std::vector<float> spawns;
    for (int i = 0; i < 48000 * 4; ++i) {
        float a = 0.f, b = 0.f;
        p.process(a, b);
        const int count = p.sampler().spawn_count();
        if (count != last_count) {
            last_count = count;
            spawns.push_back(p.sampler().last_spawn_pos());
        }
    }

    // Guard against passing vacuously: if the deck never actually spawned
    // (a broken rig, an engine that silently stayed on ENGINE_SYNTH, an empty
    // buffer, etc.) `spawns` would be empty and every CHECK below would
    // trivially pass by never running. Four seconds of render at these grain
    // settings produces well over a thousand spawns (measured while building
    // this test); 20 is a generous floor that only trips if spawning itself
    // is broken, not if the count merely varies with grain-length settings.
    REQUIRE(spawns.size() > 20);

    const float want = 0.9f * float(kSFrames - 1);
    for (float pos : spawns) {
        INFO("spawn pos=" << pos << " want~" << want);
        CHECK(pos == doctest::Approx(want).epsilon(0.02));
    }
}

TEST_CASE("F-04: MOD brings the sampler's scatter back") {
    // Die Gegenprobe: der Nebel muss erreichbar bleiben, sonst hat der Fix
    // eine Klangfarbe entfernt statt sie steuerbar zu machen.
    InstRig g;
    const int p = 0;
    g.inst.set_engine(p, ENGINE_SAMPLER);
    g.inst.set_depth(p, 1.f);                   // MOD = 1

    std::vector<float> l(kSFrames), r(kSFrames);
    for (size_t i = 0; i < kSFrames; ++i) {
        l[i] = std::sin(6.2831853f * 220.f * float(i) / 48000.f);
        r[i] = l[i];
    }
    g.inst.load_sample(p, l.data(), r.data(), kSFrames);
    g.inst.set_target_base(p, spky::LANE_SOURCE, 0.9f);

    // Ueber viele Spawns hinweg muss die Position wandern.
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 48000 * 4; ++i) {
        g.render(1);
        const float pos = g.inst.sampler_last_spawn_pos(p);
        if (pos < lo) lo = pos;
        if (pos > hi) hi = pos;
    }
    INFO("spawn position range " << lo << " .. " << hi);
    CHECK(hi - lo > 0.2f * float(kSFrames));
}

TEST_CASE("K-01: trigger_manual flattens the chord on a sampler deck") {
    // trigger_manual ruft trigger_chord ohne _flatten_for_sampler. Bei
    // COLOR > 0 landen bis zu vier Toene in der SamplerEngine, bis der
    // naechste _control_tick korrigiert -- bis zu zwoelf Spawns weit.
    //
    // Zwei Anpassungen gegenueber dem Brief-Entwurf, beide durch Messung
    // erzwungen (Schritt 2 des Briefs sieht das ausdruecklich vor):
    //
    // 1) Die PITCH-Lane wird hier explizit abgeschaltet, genau wie im
    // Nachbartest "part: the sampler granulates at ONE pitch whatever COLOR
    // says" (Zeile 514 oben): target_raw() gibt der PITCH-Lane ihre eigene,
    // von set_depth() unabhaengige Modulationstiefe (d = 1.f fuer
    // LANE_PITCH, part.cpp:47, "the PITCH lane is the anchor"), also
    // scattert MOTION die Tonhoehe weiterhin selbst bei depth = 0. Ohne die
    // Abschaltung ueberlagert dieses (gewollte) Scatter das Messsignal: die
    // erste Messung zeigte den Ratio-Sprung bei Sample 190 -- weit nach dem
    // naechsten Control-Tick (<= 96 Samples) -- unveraendert VOR und NACH dem
    // Fix, also vom Scatter, nicht vom fehlenden Flatten.
    //
    // 2) SIZE auf das Minimum (kuerzeste Koerner, ~1 ms = 48 Samples). Bei
    // der Default-Koernerlaenge liegt der natuerliche spawn_every bei
    // ~190-200 Samples -- laenger als das <= 96-Sample-Zeitfenster, in dem
    // _chord_n > 1 ueberhaupt steht (Part::_control_tick ruft set_chord
    // jeden Tick, unabhaengig von diesem Fix). Ohne einen zweiten Spawn
    // INNERHALB dieses Fensters bleibt _rr bei 0 stehen und der Bug ist
    // unsichtbar, egal wie lang das Beobachtungsfenster ist -- das war
    // exakt der zweite Fehlschlag der ersten Testversion. Bei kOverlap = 8
    // (Default, sampler_engine.h) und Koernerlaenge 48 Samples liegt
    // spawn_interval bei 48/8 = 6, geklemmt auf kSpawnMinSamples = 8 --
    // genug fuer ueber ein Dutzend Spawns in einem 96-Sample-Fenster, exakt
    // die Groessenordnung, die der Hintergrund-Absatz des Briefs nennt.
    InstRig g;
    const int p = 0;
    g.inst.set_engine(p, ENGINE_SAMPLER);
    g.inst.set_target_active(p, LANE_PITCH, false);
    g.inst.set_target_base(p, LANE_SIZE, 0.f);   // kuerzeste Koerner
    g.inst.set_color(p, 1.f);                   // maximaler Chord
    g.inst.set_depth(p, 0.f);

    std::vector<float> l(kSFrames), r(kSFrames);
    for (size_t i = 0; i < kSFrames; ++i) {
        l[i] = std::sin(6.2831853f * 220.f * float(i) / 48000.f);
        r[i] = l[i];
    }
    g.inst.load_sample(p, l.data(), r.data(), kSFrames);
    g.render(4800);

    g.inst.trigger_manual(p);
    // Direkt nach dem Trigger, vor dem naechsten Control-Tick, muessen alle
    // Spawns auf demselben Verhaeltnis landen.
    const int spawns_before = g.inst.sampler_spawn_count(p);
    float first = 0.f;
    bool  have  = false;
    for (int i = 0; i < 90; ++i) {
        g.render(1);
        const float ratio = g.inst.sampler_last_spawn_ratio(p);
        if (!have) { first = ratio; have = true; }
        INFO("i=" << i << " ratio=" << ratio << " first=" << first);
        CHECK(ratio == doctest::Approx(first).epsilon(1e-4));
    }

    // Guard against passing vacuously (Minor 5, review 2026-07-22): every
    // CHECK above only ever compares sampler_last_spawn_ratio() against ITS
    // OWN first reading, so if no spawn landed in this 90-sample window at
    // all, the value never changes and every comparison trivially holds.
    // Today that cannot happen only because SIZE = 0 (see comment 2 above)
    // clamps spawn_interval to kSpawnMinSamples = 8, forcing over a dozen
    // spawns into this exact window -- a coincidence of this test's own
    // setup, not something the assertions above check for themselves. Same
    // pattern as the F-04 "ORGANIZE reaches the spawn position" test: use the
    // engine's own cumulative counter, which increments once per spawn
    // regardless of whether the observed value moved.
    const int spawns = g.inst.sampler_spawn_count(p) - spawns_before;
    INFO("spawns in window=" << spawns);
    REQUIRE(spawns > 5);
}

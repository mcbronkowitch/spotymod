#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include "synth/synth_engine.h"
using namespace spky;

// Feed one set of targets: TIMBRE, FILTER, PITCH, MOTION, LEVEL by lane slot.
static void feed(SynthEngine& e, float pitch, float timbre = 0.f, float filter = 1.f,
                 float motion = 0.f, float level = 1.f) {
    float t[LANE_COUNT] = { timbre, filter, pitch, motion, level };
    e.set_targets(t, 0.5f);
}

// Fresh engine in "measurement" trim: pure sine, no sub, no detune, mono.
static void fresh(SynthEngine& e, uint32_t seed = 99) {
    e.set_seed(seed);
    e.init(48000.f);
    e.set_sub(0.f);
    e.set_detune(0.f);
    e.set_cycle(1.f);
    feed(e, 0.5f);
}

static std::vector<float> render_l(SynthEngine& e, int n) {
    std::vector<float> out(n);
    for (auto& s : out) {
        float l = 0.f, r = 0.f;
        e.process(l, r);
        s = l;
    }
    return out;
}

static int crossings(const std::vector<float>& v, size_t from = 0) {
    int n = 0;
    for (size_t i = from + 1; i < v.size(); ++i)
        if (v[i - 1] <= 0.f && v[i] > 0.f) ++n;
    return n;
}

TEST_CASE("synth: pitch contract - trigger(p) sounds at 110*8^p Hz, latched in STEP") {
    SynthEngine e;
    fresh(e);
    e.trigger(0.5f);                        // 311.13 Hz
    auto v1 = render_l(e, 48000);
    int n1 = crossings(v1, 4800);           // skip the attack; ~311 * 0.9 = 280
    CHECK(n1 >= 275);
    CHECK(n1 <= 285);

    SynthEngine e2;
    fresh(e2);
    e2.trigger(0.5f);
    feed(e2, 0.9f);                         // target moves AFTER the trigger...
    auto v2 = render_l(e2, 48000);
    CHECK(crossings(v2, 4800) == n1);       // ...a STEP voice must not follow
}

TEST_CASE("synth: round-robin fills 4 voices; 5th steals the oldest, click-free") {
    SynthEngine e;
    fresh(e);
    e.set_cycle(4.f);                       // long decay: all voices stay active
    for (int v = 0; v < 4; ++v) {
        e.trigger(0.2f + 0.15f * v);
        render_l(e, 960);                   // 20 ms apart
    }
    CHECK(e.active_voices() == 4);
    for (int v = 0; v < 4; ++v) CHECK(e.voice_env(v) > 0.f);

    float env0_at_steal = 0.f;              // voice 0 is the oldest
    float prev = 0.f, max_delta = 0.f;
    for (int i = 0; i < 9600; ++i) {
        if (i == 4800) {
            env0_at_steal = e.voice_env(0); // level the instant before the steal
            e.trigger(0.8f);                // 5th note: steal
        }
        float l = 0.f, r = 0.f;
        e.process(l, r);
        if (i > 0) max_delta = std::max(max_delta, std::fabs(l - prev));
        prev = l;
    }
    CHECK(e.active_voices() == 4);          // reuse, never a 5th voice
    CHECK(e.voice_env(0) > env0_at_steal);  // retriggered FROM its level, rising
    CHECK(max_delta < 0.2f);                // no click on the steal
}

TEST_CASE("synth: decay length tracks set_cycle (ratio 1.5x honored)") {
    auto silence_time = [](float cycle_s) {
        SynthEngine e;
        fresh(e);
        e.set_cycle(cycle_s);
        e.trigger(0.5f);
        int n = 0;
        while (n < 48000 * 30) {
            float l = 0.f, r = 0.f;
            e.process(l, r);
            ++n;
            if (e.active_voices() == 0) break;
        }
        return n;
    };
    // decay = 1.5 x cycle (to -60 dB); voice idles at -80 dB = ~1.33 x that
    int fast = silence_time(0.5f);          // decay 0.75 s -> idle ~1.0 s
    int slow = silence_time(2.0f);          // decay 3 s    -> idle ~4 s
    CHECK(fast > static_cast<int>(0.75f * 48000));
    CHECK(fast < static_cast<int>(1.4f * 48000));
    CHECK(slow > static_cast<int>(3.0f * 48000));
    CHECK(slow < static_cast<int>(5.6f * 48000));
    CHECK(slow > fast * 3);
}

TEST_CASE("synth: attack floor 2 ms and decay clamp 50 ms at extreme cycles") {
    SynthEngine e;
    fresh(e);
    e.set_cycle(0.02f);     // attack 2% -> 0.4 ms, floored to 2 ms;
                            // decay 1.5x -> 30 ms, clamped up to 50 ms
    e.trigger(0.5f);
    int to_peak = 0;
    while (e.voice_env(0) < 1.f && to_peak < 4800) {
        float l = 0.f, r = 0.f;
        e.process(l, r);
        ++to_peak;
    }
    CHECK(to_peak >= 80);                   // ~96 samples = 2 ms (ctrl-rate slack)
    CHECK(to_peak <= 200);
    int n = to_peak;
    while (e.active_voices() > 0 && n < 48000) {
        float l = 0.f, r = 0.f;
        e.process(l, r);
        ++n;
    }
    CHECK(n - to_peak > static_cast<int>(0.05f * 48000));         // >= 50 ms decay
    CHECK(n - to_peak < static_cast<int>(0.05f * 48000 * 2.0f));
    float l = 0.f, r = 0.f;                 // STEP silence is EXACT zero
    e.process(l, r);
    CHECK(l == 0.f);
    CHECK(r == 0.f);
}

TEST_CASE("synth: FLOW drone - auto-trigger, sustain 0.7, pitch tracks, demotion") {
    SynthEngine e;
    fresh(e);
    feed(e, 0.25f);                         // 185.0 Hz
    e.set_flow(true);                       // no sustaining voice -> auto-trigger
    auto v = render_l(e, 48000);
    CHECK(e.active_voices() >= 1);
    CHECK(e.sustain_voice() >= 0);
    int n = crossings(v, 4800);             // ~185 * 0.9 = 166
    CHECK(n >= 160);
    CHECK(n <= 172);

    render_l(e, 48000 * 3);                 // ride decay-to-sustain out
    CHECK(e.voice_env(e.sustain_voice()) == doctest::Approx(0.7f).epsilon(0.03));

    feed(e, 0.75f);                         // 523.3 Hz: the drone must follow
    render_l(e, 9600);                      // let the control rate catch up
    auto v2 = render_l(e, 48000);
    int n2 = crossings(v2);
    CHECK(n2 >= 515);
    CHECK(n2 <= 532);

    int old_voice = e.sustain_voice();      // a new fire demotes the old drone
    e.trigger(0.25f);
    CHECK(e.sustain_voice() != old_voice);
    render_l(e, 48000 * 8);                 // demoted voice decays to zero
    CHECK(e.voice_env(old_voice) == 0.f);
    CHECK(e.voice_env(e.sustain_voice()) == doctest::Approx(0.7f).epsilon(0.03));
}

TEST_CASE("synth: entering FLOW mid-run with no sustaining voice auto-triggers") {
    SynthEngine e;
    fresh(e);
    render_l(e, 4800);                      // STEP, no trigger: stays silent
    CHECK(e.active_voices() == 0);
    e.set_flow(true);                       // drone promise
    render_l(e, 4800);
    CHECK(e.active_voices() >= 1);
    CHECK(e.sustain_voice() >= 0);
}

TEST_CASE("synth: MOTION width 0 is dead mono; width 1 separates the channels") {
    SynthEngine e;
    fresh(e);
    e.set_flow(true);                       // steady drone to measure
    feed(e, 0.5f, 0.f, 1.f, 0.f);           // width 0
    float max_diff = 0.f;
    for (int i = 0; i < 48000; ++i) {
        float l = 0.f, r = 0.f;
        e.process(l, r);
        max_diff = std::max(max_diff, std::fabs(l - r));
    }
    CHECK(max_diff == 0.f);                 // identical gains -> bit-equal L/R

    SynthEngine e2;
    fresh(e2);
    e2.set_flow(true);
    feed(e2, 0.5f, 0.f, 1.f, 1.f);          // width 1: voice 0 fans hard left
    float suml = 0.f, sumr = 0.f;
    for (int i = 0; i < 48000; ++i) {
        float l = 0.f, r = 0.f;
        e2.process(l, r);
        suml += l * l;
        sumr += r * r;
    }
    CHECK(std::fabs(suml - sumr) / (suml + sumr + 1e-9f) > 0.2f);
}

TEST_CASE("synth: identical seed + call sequence is bit-identical") {
    auto run = [] {
        SynthEngine e;
        e.set_seed(1234);
        e.init(48000.f);
        e.set_cycle(0.8f);
        e.set_flow(true);
        std::vector<float> out;
        out.reserve(96000);
        for (int i = 0; i < 48000; ++i) {
            float t[LANE_COUNT] = { 0.4f, 0.7f, 0.5f, 0.8f, 0.9f };
            e.set_targets(t, 0.5f);
            if (i == 10000 || i == 20000) e.trigger(0.3f);
            float l = 0.f, r = 0.f;
            e.process(l, r);
            out.push_back(l);
            out.push_back(r);
        }
        return out;
    };
    CHECK(run() == run());
}

TEST_CASE("synth: trigger_chord with one note is bit-identical to trigger") {
    SynthEngine a, b;
    a.set_seed(42u); b.set_seed(42u);
    a.init(48000.f); b.init(48000.f);
    a.set_flow(false); b.set_flow(false);
    a.trigger(0.4f);
    const float p = 0.4f;
    b.trigger_chord(&p, 1);
    for (int i = 0; i < 9600; ++i) {
        float la = 0.f, ra = 0.f, lb = 0.f, rb = 0.f;
        a.process(la, ra); b.process(lb, rb);
        CHECK(la == lb);                           // exact — the COLOR-0 invariant
        CHECK(ra == rb);
    }
}

TEST_CASE("synth: a 4-note stab lands all voices inside the spread window") {
    SynthEngine e;
    e.set_seed(7u);
    e.init(48000.f);
    e.set_flow(false);
    const float chord[4] = { 0.3f, 0.36f, 0.42f, 0.5f };
    e.trigger_chord(chord, 4);
    CHECK(e.active_voices() >= 1);                 // the root fires immediately
    const int window = static_cast<int>(SynthEngine::kStabSpreadS * 48000.f) + 2;
    for (int i = 0; i < window; ++i) { float l, r; e.process(l, r); }
    CHECK(e.active_voices() == 4);                 // the rest strewed in behind
}

TEST_CASE("synth: chord stabs are deterministic across engines") {
    SynthEngine a, b;
    a.set_seed(9u); b.set_seed(9u);
    a.init(48000.f); b.init(48000.f);
    a.set_flow(false); b.set_flow(false);
    const float chord[3] = { 0.3f, 0.38f, 0.47f };
    a.trigger_chord(chord, 3);
    b.trigger_chord(chord, 3);
    for (int i = 0; i < 9600; ++i) {
        float la = 0.f, ra = 0.f, lb = 0.f, rb = 0.f;
        a.process(la, ra); b.process(lb, rb);
        CHECK(la == lb);
        CHECK(ra == rb);
    }
}

namespace {
// drive an engine for n samples, feeding a constant chord surface
static void run_surface(SynthEngine& e, const float* chord, int n_chord,
                        int samples, float* max_step = nullptr) {
    float prev = 0.f;
    for (int i = 0; i < samples; ++i) {
        e.set_chord(chord, n_chord);
        float l = 0.f, r = 0.f;
        e.process(l, r);
        if (max_step && i > 0 && std::fabs(l - prev) > *max_step)
            *max_step = std::fabs(l - prev);
        prev = l;
    }
}
} // namespace

TEST_CASE("synth: FLOW drones the whole chord as a surface") {
    SynthEngine e;
    e.set_seed(3u);
    e.init(48000.f);
    const float chord[4] = { 0.3f, 0.36f, 0.42f, 0.5f };
    e.set_flow(true);                              // promise arms
    run_surface(e, chord, 4, 48000);               // 1 s
    CHECK(e.sustain_count() == 4);
    for (int v = 0; v < SynthEngine::kVoices; ++v)
        CHECK(e.voice_env(v) == doctest::Approx(0.7f).epsilon(0.05));
}

TEST_CASE("synth: the next chord crossfades the surface") {
    SynthEngine e;
    e.set_seed(3u);
    e.init(48000.f);
    const float chord[4] = { 0.3f, 0.36f, 0.42f, 0.5f };
    e.set_flow(true);
    run_surface(e, chord, 4, 48000);
    const float next[3] = { 0.35f, 0.41f, 0.47f };
    e.trigger_chord(next, 3);
    run_surface(e, next, 3, 48000);
    CHECK(e.sustain_count() == 3);
}

TEST_CASE("synth: COLOR bloom and collapse without a trigger, click-free") {
    SynthEngine e;
    e.set_seed(3u);
    e.init(48000.f);
    const float one[1] = { 0.4f };
    const float three[3] = { 0.4f, 0.33f, 0.48f };
    e.set_flow(true);
    run_surface(e, one, 1, 24000);                 // settle as a single drone
    CHECK(e.sustain_count() == 1);
    float step = 0.f;
    run_surface(e, three, 3, 24000, &step);        // knob turned up: bloom
    CHECK(e.sustain_count() == 3);
    CHECK(step < 0.3f);                            // no hard discontinuity
    step = 0.f;
    run_surface(e, one, 1, 48000, &step);          // knob back down: collapse
    CHECK(e.sustain_count() == 1);
    CHECK(step < 0.3f);
}

TEST_CASE("synth: hold releases the whole surface and re-arms the chord") {
    SynthEngine e;
    e.set_seed(3u);
    e.init(48000.f);
    const float chord[3] = { 0.3f, 0.36f, 0.42f };
    e.set_flow(true);
    run_surface(e, chord, 3, 48000);
    CHECK(e.sustain_count() == 3);
    e.set_hold(true);                              // CHOKE
    run_surface(e, chord, 3, 4800);
    CHECK(e.sustain_count() == 0);
    e.set_hold(false);                             // floor free: full chord returns
    run_surface(e, chord, 3, 48000);
    CHECK(e.sustain_count() == 3);
}

TEST_CASE("synth: entering FLOW fires the promise as the full chord") {
    SynthEngine e;
    e.set_seed(3u);
    e.init(48000.f);
    e.set_flow(false);
    const float chord[3] = { 0.3f, 0.36f, 0.42f };
    run_surface(e, chord, 3, 480);                 // STEP: surface ignored
    CHECK(e.sustain_count() == 0);
    e.set_flow(true);
    run_surface(e, chord, 3, 48000);
    CHECK(e.sustain_count() == 3);
}

TEST_CASE("synth: re-bloom under a decay tail never cannibalizes the surface") {
    // final-review finding #1: a bloom that finds no free voice must steal
    // the oldest NON-sustaining voice, never a live surface voice. Repro:
    // 4-voice FLOW surface -> collapse to 3 (demoted voice still decaying,
    // long tail) -> bloom back to 4. Under the bug, the steal picks the
    // globally oldest voice by _order regardless of _sustaining[], which
    // hits the root (slot 0) and breaks slot contiguity; a later bloom then
    // lands on the now-missing slot 0 and _demote_all() nukes the whole
    // surface (sustain_count churns 3 -> 1 -> ... instead of holding >= 3).
    SynthEngine e;
    e.set_seed(5u);
    e.init(48000.f);
    e.set_decay(0.8f);                              // long tail outlives the re-bloom window
    const float four[4]  = { 0.3f, 0.36f, 0.42f, 0.5f };
    const float three[3] = { 0.3f, 0.36f, 0.42f };
    e.set_flow(true);                               // promise arms the 4-note surface
    run_surface(e, four, 4, 48000);                  // ~1 s: settle as a full surface
    CHECK(e.sustain_count() == 4);
    run_surface(e, three, 3, 480);                   // collapse to 3; demoted voice still decaying
    CHECK(e.sustain_count() == 3);

    int min_sustain = e.sustain_count();
    int reached_four_at = -1;
    for (int i = 0; i < 480; ++i) {                  // re-bloom to 4 (~5 control ticks)
        e.set_chord(four, 4);
        float l = 0.f, r = 0.f;
        e.process(l, r);
        const int sc = e.sustain_count();
        if (sc < min_sustain) min_sustain = sc;
        if (reached_four_at < 0 && sc == 4) reached_four_at = i;
    }
    CHECK(min_sustain >= 3);                         // never cannibalized below the collapsed floor
    REQUIRE(reached_four_at >= 0);
    CHECK(reached_four_at < 300);                    // reaches 4 within ~3 control ticks
}

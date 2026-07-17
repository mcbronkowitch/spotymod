#pragma once
#include "mod/lane_id.h"

namespace spky {

// Selectable part engines. ENGINE_SYNTH is the boot default from M2 on;
// the test tone stays selectable (tests, A/B reference). The M5 sampler
// will extend this enum.
enum EngineId { ENGINE_TEST_TONE = 0, ENGINE_SYNTH = 1 };

// A part's sound engine. Consumes the 5 normalized target values; produces
// stereo audio. TestToneEngine (M1), SynthEngine (M2) and SamplerEngine
// (M5) implement the same interface behind the same Part.
class IPartEngine {
public:
    virtual ~IPartEngine() = default;
    virtual void init(float sample_rate) = 0;
    virtual void set_targets(const float* targets /*[LANE_COUNT]*/, float tune) = 0;
    virtual void trigger(float pitch_norm) = 0;
    virtual void process(float& outL, float& outR) = 0;

    // M2 additions - default no-ops so engines that don't care (test tone,
    // M5 sampler) ignore them. Part forwards both: cycle on change (not per
    // sample), flow on STEP/FLOW switches.
    virtual void set_cycle(float /*seconds*/) {}   // master-lane cycle length
    virtual void set_flow(bool /*flow*/) {}        // true = FLOW, false = STEP

    // CHOKE hold (spec 2026-07-16 choke-priority rev. 2): while held, a FLOW
    // engine releases its sustaining drone (decays out, click-free) and stops
    // auto-retriggering; releasing the hold re-arms it. Default no-op.
    virtual void set_hold(bool /*on*/) {}

    // Chord layer (spec 2026-07-17 chord-layer). trigger_chord fires one
    // chord; the default keeps single-note engines working unchanged.
    // set_chord feeds the CURRENT chord surface targets (refreshed every
    // sample by Part) so a FLOW engine can track root + COLOR live; engines
    // without a surface ignore it.
    virtual void trigger_chord(const float* pitches_norm, int n) {
        for (int i = 0; i < n; ++i) trigger(pitches_norm[i]);
    }
    virtual void set_chord(const float* /*pitches_norm*/, int /*n*/) {}
};

} // namespace spky

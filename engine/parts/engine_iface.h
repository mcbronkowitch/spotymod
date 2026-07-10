#pragma once
#include "mod/lane_id.h"

namespace spky {

// A part's sound engine. Consumes the 5 normalized target values; produces
// stereo audio. M1 ships TestToneEngine; SynthVoice (M2) and SamplerEngine
// (M5) implement the same interface behind the same Part.
class IPartEngine {
public:
    virtual ~IPartEngine() = default;
    virtual void init(float sample_rate) = 0;
    virtual void set_targets(const float* targets /*[LANE_COUNT]*/, float tune) = 0;
    virtual void trigger(float pitch_norm) = 0;
    virtual void process(float& outL, float& outR) = 0;
};

} // namespace spky

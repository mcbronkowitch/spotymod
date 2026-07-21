#pragma once
#include <string>

// Host-side state of the M5b texture deck, one instance per part. Everything
// here is edit-layer state the engine does not own and the module must
// persist: the engine keeps the audio, this keeps how it got there.
namespace spkyvcv {

struct SamplerPartState {
    std::string path;       // last file loaded, "" if none / live-recorded
    int   tapeIdx  = 1;     // 0 = Digital, 1 = Tape -- an index, because the
                            // context menu binds it with createIndexPtrSubmenuItem
    bool  reverse  = false;
    float feedback = 0.95f; // overdub feedback, ~-3 dB (engine default)
    bool  testTone = false; // dev: ENG's sampler slot plays the test tone instead
    bool  factoryLoaded = false;  // content came from the factory WAV, not the user
};

}  // namespace spkyvcv

#pragma once
#include "Effects/reverbsc.h"
#include "Effects/pitchshifter.h"

namespace spky {

// The one shared room behind both parts. Input is the summed per-part sends
// (post-FX, pre-morph); output is wet-only and joins the master AFTER the
// part mix. Optional shimmer: a +12 st pitch shifter on the previous wet
// frame, fed back into the reverb input — fully skipped at shimmer == 0.
//
// BIG object (~530 KB — ReverbSc's aux buffer and the shifter's delay lines
// are inline members). Never stack-allocate: the desktop host owns it as a
// static; the M6 firmware shell places it in SDRAM. Injected via FxMem.
class AmbientReverb {
public:
    void init(float sample_rate);
    void set_size(float norm);      // decay: ReverbSc feedback 0.4 .. 0.99
    void set_tone(float norm);      // damping LP 500 Hz .. 16 kHz, exp
    void set_shimmer(float norm);   // 0 = shifter skipped entirely (CPU)
    float shimmer() const { return _shim; }
    void process(float in_l, float in_r, float& out_l, float& out_r);

private:
    daisysp::ReverbSc _rev;
    daisysp::PitchShifter _shift;
    float _shim = 0.f;
    float _last_l = 0.f;
    float _last_r = 0.f;
};

} // namespace spky

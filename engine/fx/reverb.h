#pragma once
#include "oliverb/oliverb.h"

namespace spky {

// The one shared room behind both parts. Input is the summed per-part sends
// (post-FX, morph-scaled in the Instrument mix) and joins the master AFTER
// the part mix as a wet-only signal.
//
// M4.5: the core is a vendored Oliverb (Clouds Parasite, MIT) — Erbe-Verb-
// style playable room. SIZE really rescales the delay reads (turning it
// Doppler-warps the tail), DECAY crosses 1.0 near the top of its travel
// into a soft-limited self-sustaining bloom, DIFFUSION morphs the room from
// discrete slap echoes to a dense wash and drags a little line modulation
// along with it (the old DEPTH knob is gone). Shimmer is gone (so is the
// separately-licensed DaisySP dependency it relied on).
//
// BIG object (~130 KB — the float delay buffer is an inline member). Never
// stack-allocate: the desktop host owns it as a static; the M6 firmware
// shell places it in SDRAM. Injected via FxMem.
class AmbientReverb {
public:
    void init(float sample_rate);
    void clear();                 // empty the room (buffer + loop filter state); params survive
    void set_size(float norm);    // room size; smoothed inside -> Doppler ride
    void set_decay(float norm);   // loop gain; crosses 1.0 at ~0.9 (bloom above)
    void set_tone(float norm);    // loop LP damping 500 Hz .. 16 kHz, exp
    void set_diffusion(float norm);       // room density: AP coeff 0..0.9 only
    void set_diffuser_mod_depth(float norm); // ap1..ap4 LFO smear depth (wash), independent
    void set_mod_depth(float norm);       // tail-delay LFO wobble depth, independent
    void process(float in_l, float in_r, float& out_l, float& out_r);

private:
    clouds::Oliverb _verb;
    float _sr = 48000.f;
    int _ctrl = 0;   // control-rate divider for the LFO slope refresh
    float _buffer[clouds::Oliverb::kBufferSize];
};

} // namespace spky

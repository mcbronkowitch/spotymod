#include "fx/reverb.h"
#include "Utility/dsp.h"
#include "util/math.h"
#include <cmath>

using namespace spky;

namespace {
constexpr int kCtrlInterval = 96;          // engine control-rate raster
constexpr uint32_t kRngSeed = 0x0BE21D5u;  // fixed: bit-deterministic renders
constexpr float kModRate = 0.5f;           // internal LFO speed; DIFFUSION weakly couples the amount
// ~80 Hz one-pole low-cut inside the loop: keeps the >100% bloom from
// accumulating DC / low-mid mud (parasites' anti-DC offset, same value).
constexpr float kHpPin = 0.01f;
constexpr float kInputGain = 0.5f;         // L+R sum -> mono average into the room
// The self-oscillating bloom (decay > 1.0) plateaus near digital full scale
// at the core's output taps (they carry 2x the in-loop signal, plus Hermite
// overshoot under depth modulation). Trim the wet-only room -8 dB so the
// bloom leaves headroom at the master sum — the M4.6 limiter is a ceiling,
// not a mixer; don't lean on it. Ear-tunable in [0.40, 0.50]; kept at the
// low end to hold the ambient_wash showcase's bloom clear of the ceiling.
constexpr float kWetGain = 0.40f;
}

void AmbientReverb::init(float sample_rate) {
    _sr = sample_rate;
    _ctrl = 0;
    _verb.Init(_buffer, kRngSeed);
    _verb.set_input_gain(kInputGain);
    _verb.set_hp(kHpPin);
    _verb.set_mod_rate(kModRate);
    set_size(0.6f);     // boot defaults (spec: audible, nothing screams,
    set_decay(0.55f);   // nothing self-oscillates)
    set_tone(0.5f);
    set_diffusion(0.7f);   // coeff 0.63 ~= the old stock 0.625 room
    _verb.Prepare();
}

void AmbientReverb::clear() {
    _verb.Clear();
    _ctrl = 0;   // refresh the LFO slopes on the next process()
}

void AmbientReverb::set_size(float norm) {
    // parasites mapping: keep the room inside the tuned sweet range
    _verb.set_size(0.05f + 0.94f * clampf(norm, 0.f, 1.f));
}

void AmbientReverb::set_decay(float norm) {
    // Linear to 1.0 at norm 0.9; the top 10% of travel pushes past unity
    // into the soft-limited bloom, capped at 1.05. (Ear-tunable knee.)
    float d = clampf(norm, 0.f, 1.f) * (1.f / 0.9f);
    _verb.set_decay(d > 1.05f ? 1.05f : d);
}

void AmbientReverb::set_tone(float norm) {
    float fc = daisysp::fmap(clampf(norm, 0.f, 1.f), 500.f, 16000.f,
                             daisysp::Mapping::LOG);
    // exact one-pole coefficient for that cutoff (control-rate libm is fine)
    _verb.set_lp(1.f - std::exp(-TWO_PI * fc / _sr));
}

void AmbientReverb::set_diffusion(float norm) {
    norm = clampf(norm, 0.f, 1.f);
    // applied instantly like decay/tone (only SIZE smooths, for the Doppler)
    _verb.set_diffusion(0.90f * norm);
    // weak coupling: more smear = slightly more line motion (0.05..0.25 of
    // the old DEPTH range; the knob owns density, motion just rides along)
    _verb.set_mod_amount((0.05f + 0.20f * norm) * 450.f);
}

void AmbientReverb::process(float in_l, float in_r, float& out_l, float& out_r) {
    if (_ctrl == 0) {
        _verb.Prepare();
        _ctrl = kCtrlInterval;
    }
    --_ctrl;
    out_l = in_l;
    out_r = in_r;
    _verb.Process(&out_l, &out_r);
    out_l *= kWetGain;
    out_r *= kWetGain;
}

#include "fx/reverb.h"
#include "Utility/dsp.h"
#include "util/math.h"
#include <cmath>

using namespace spky;

namespace {
constexpr int kCtrlInterval = 96;          // engine control-rate raster
constexpr uint32_t kRngSeed = 0x0BE21D5u;  // fixed: bit-deterministic renders
constexpr float kModRate = 0.5f;           // internal LFO speed; DEPTH scales amount only
// ~80 Hz one-pole low-cut inside the loop: keeps the >100% bloom from
// accumulating DC / low-mid mud (parasites' anti-DC offset, same value).
constexpr float kHpPin = 0.01f;
constexpr float kDiffusion = 0.625f;       // Oliverb stock
constexpr float kInputGain = 0.5f;         // L+R sum -> mono average into the room
}

void AmbientReverb::init(float sample_rate) {
    _sr = sample_rate;
    _ctrl = 0;
    _verb.Init(_buffer, kRngSeed);
    _verb.set_diffusion(kDiffusion);
    _verb.set_input_gain(kInputGain);
    _verb.set_hp(kHpPin);
    _verb.set_mod_rate(kModRate);
    set_size(0.6f);     // boot defaults (spec: audible, nothing screams,
    set_decay(0.55f);   // nothing self-oscillates)
    set_tone(0.5f);
    set_depth(0.25f);
    _verb.Prepare();
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

void AmbientReverb::set_depth(float norm) {
    // parasites full-knob 300 samples at 32 kHz -> x1.5 at 48 kHz
    _verb.set_mod_amount(clampf(norm, 0.f, 1.f) * 450.f);
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
}

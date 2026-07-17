#include "synth/synth_engine.h"
#include <cmath>
#include "util/math.h"

using namespace spky;

namespace {
// 4 voices at full level + sub must stay inside the part's +/-1.5 headroom.
constexpr float kVoiceGain = 0.22f;
constexpr float kPanFan[SynthEngine::kVoices] = { -1.f, 1.f, -0.5f, 0.5f };

// Pitch contract (identical numbers to TestToneEngine): 0..1 = 36 semitones.
// std::pow is fine here - trigger/control rate, never per sample.
inline float pitch_to_hz(float p) { return 110.f * std::pow(8.f, clampf(p, 0.f, 1.f)); }

// FILTER target: exponential 60 Hz .. 14 kHz (spec).
inline float filter_hz(float n) {
    return 60.f * std::pow(14000.f / 60.f, clampf(n, 0.f, 1.f));
}
} // namespace

void SynthEngine::init(float sample_rate) {
    _sr = sample_rate;
    for (int v = 0; v < kVoices; ++v) {
        _voices[v].init(sample_rate, _seed + 0x9e3779b9u * static_cast<uint32_t>(v + 1));
        _order[v] = 0;
    }
    _seq = 0;
    _sustain_voice = -1;
    _auto_pending = false;
    _hold = false;
    _next_rr = 0;
    _ctrl_ctr = 0;                 // first process() runs a control tick
    _level.init(sample_rate, 0.01f);
    _level.reset(_targets[LANE_LEVEL]);
    _update_control();
}

void SynthEngine::set_targets(const float* t, float /*tune*/) {
    // tune is already summed into the quantized PITCH target upstream (Part).
    for (int i = 0; i < LANE_COUNT; ++i) _targets[i] = t[i];
}

void SynthEngine::set_cycle(float seconds) {
    _cycle_s = clampf(seconds, 0.01f, 120.f);   // applied at the next ctrl tick
}

void SynthEngine::set_flow(bool flow) {
    if (flow == _flow) return;
    _flow = flow;
    if (!flow) {
        // STEP: no sustaining voice; a holding drone is released (decays out)
        if (_sustain_voice >= 0) _voices[_sustain_voice].set_sustaining(false);
        _sustain_voice = -1;
        _auto_pending = false;
    } else if (_sustain_voice < 0 && !_hold) {
        _auto_pending = true;      // drone promise; fires in process()
    }
}

void SynthEngine::set_hold(bool on) {
    if (on == _hold) return;
    _hold = on;
    if (on) {
        // CHOKE: release the FLOW drone (decays out, click-free) and stop
        // the auto-retrigger while the other deck holds the floor.
        if (_sustain_voice >= 0) _voices[_sustain_voice].set_sustaining(false);
        _sustain_voice = -1;
        _auto_pending = false;
    } else if (_flow && _sustain_voice < 0) {
        _auto_pending = true;      // floor is free again: drone fades back in
    }
}

void SynthEngine::trigger(float pitch_norm) { _do_trigger(pitch_norm); }

void SynthEngine::_do_trigger(float pitch_norm) {
    int pick = -1;
    for (int i = 0; i < kVoices; ++i) {              // round-robin over free voices
        int v = (_next_rr + i) % kVoices;
        if (!_voices[v].active()) { pick = v; break; }
    }
    if (pick < 0) {                                   // none free: steal the oldest
        uint32_t oldest = _order[0];
        pick = 0;
        for (int v = 1; v < kVoices; ++v)
            if (_order[v] < oldest) { oldest = _order[v]; pick = v; }
    }
    _next_rr = (pick + 1) % kVoices;
    _order[pick] = ++_seq;

    if (_flow) {
        if (_sustain_voice >= 0 && _sustain_voice != pick)
            _voices[_sustain_voice].set_sustaining(false);   // demote: decays out
        _sustain_voice = pick;
        _voices[pick].set_sustaining(true);
    }
    _voices[pick].trigger(pitch_to_hz(pitch_norm));   // pitch LATCHED here
}

void SynthEngine::_update_control() {
    const float attack_s = clampf(_attack_ratio * _cycle_s, kAttackFloorS, kDecayMaxS);
    const float decay_s  = clampf(_decay_ratio  * _cycle_s, kDecayMinS,  kDecayMaxS);

    const float timbre = _targets[LANE_SOURCE];       // pad 1 = TIMBRE
    const float det_ct = timbre * timbre * _detune_max_ct;   // t^2 law (spec)
    const float off    = _filt_amt < 0.f ? kFiltLeftScale * _filt_amt : _filt_amt;
    const float n_raw  = _targets[LANE_SIZE] + off;          // pad 2 = FILTER + trim
    const float cutoff = filter_hz(n_raw);                   // clamps 0..1 internally
    _filt_gain = clampf(1.f + n_raw / kFiltFadeRange, 0.f, 1.f);
    const float width  = clampf(_targets[LANE_MOTION], 0.f, 1.f);
    const float dt_s   = kCtrlInterval / _sr;

    for (int v = 0; v < kVoices; ++v) {
        Voice& vc = _voices[v];
        vc.set_env_times(attack_s, decay_s);
        vc.set_morph(timbre);
        vc.set_detune_cents(det_ct);
        vc.set_sub_level(_sub_level);
        vc.set_cutoff_hz(cutoff);
        vc.set_resonance(_resonance);
        vc.set_pan(kPanFan[v] * width);
        vc.set_drift_amount(width);
        vc.update_control(dt_s);
    }

    // only the FLOW sustaining voice tracks the target after its trigger
    if (_flow && _sustain_voice >= 0)
        _voices[_sustain_voice].set_pitch_hz(pitch_to_hz(_targets[LANE_PITCH]));
}

void SynthEngine::process(float& outL, float& outR) {
    if (_auto_pending) {                     // targets are fresh by now (Part
        _auto_pending = false;               // calls set_targets every sample)
        _do_trigger(_targets[LANE_PITCH]);
    }
    if (--_ctrl_ctr <= 0) {
        _ctrl_ctr = kCtrlInterval;
        _update_control();
    }
    const float gain = _level.process(_targets[LANE_LEVEL] * _filt_gain) * kVoiceGain;
    float l = 0.f, r = 0.f;
    for (auto& v : _voices) v.process(l, r);
    outL = l * gain;
    outR = r * gain;
}

void SynthEngine::set_attack(float n) {
    _attack_ratio = 0.002f * std::pow(250.f, clampf(n, 0.f, 1.f));
}

void SynthEngine::set_decay(float n) {
    _decay_ratio = 0.1f * std::pow(80.f, clampf(n, 0.f, 1.f));
}

void SynthEngine::set_resonance(float n) { _resonance = clampf(n, 0.f, 1.f); }
void SynthEngine::set_sub(float n)       { _sub_level = clampf(n, 0.f, 1.f); }
void SynthEngine::set_filt(float n) { _filt_amt = clampf(n, -1.f, 1.f); }

void SynthEngine::set_detune(float n) {
    _detune_max_ct = clampf(n, 0.f, 1.f) * kDetuneCeilCt;
}

int SynthEngine::active_voices() const {
    int n = 0;
    for (const auto& v : _voices)
        if (v.active()) ++n;
    return n;
}

float SynthEngine::voice_env(int v) const {
    if (v < 0 || v >= kVoices) return 0.f;
    return _voices[v].env_value();
}

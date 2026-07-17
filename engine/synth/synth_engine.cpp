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
        _sustaining[v] = false;
        _chord_slot[v] = -1;
    }
    _seq = 0;
    _auto_pending = false;
    _hold = false;
    _next_rr = 0;
    _ctrl_ctr = 0;                 // first process() runs a control tick
    _pending_n = 0;
    _chord_n = 1;
    _chord[0] = _targets[LANE_PITCH];
    _vel_now = 1.f;
    _stab_rng.seed(_seed ^ 0x57AB5EEDu);
    _level.init(sample_rate, 0.01f);
    _level.reset(_targets[LANE_LEVEL]);
    _update_control();
}

int SynthEngine::sustain_voice() const {
    for (int v = 0; v < kVoices; ++v)
        if (_sustaining[v]) return v;
    return -1;
}

int SynthEngine::sustain_count() const {
    int n = 0;
    for (int v = 0; v < kVoices; ++v)
        if (_sustaining[v]) ++n;
    return n;
}

void SynthEngine::_demote_all() {
    for (int v = 0; v < kVoices; ++v)
        if (_sustaining[v]) {
            _voices[v].set_sustaining(false);
            _sustaining[v] = false;
            _chord_slot[v] = -1;
        }
}

void SynthEngine::set_targets(const float* t, float /*tune*/) {
    // tune is already summed into the quantized PITCH target upstream (Part).
    for (int i = 0; i < LANE_COUNT; ++i) _targets[i] = t[i];
    // engine-standalone default: with no chord fed, the surface is the pitch
    // target (keeps engine-only tests and the test tone semantics unchanged)
    if (_chord_n <= 1) _chord[0] = _targets[LANE_PITCH];
}

void SynthEngine::set_chord(const float* p, int n) {
    if (n < 1) n = 1;
    if (n > kMaxChord) n = kMaxChord;
    for (int i = 0; i < n; ++i) _chord[i] = p[i];
    _chord_n = n;
}

void SynthEngine::set_cycle(float seconds) {
    _cycle_s = clampf(seconds, 0.01f, 120.f);   // applied at the next ctrl tick
}

void SynthEngine::set_flow(bool flow) {
    if (flow == _flow) return;
    _flow = flow;
    if (!flow) {
        _demote_all();                 // STEP: no surface; drones decay out
        _pending_n = 0;
        _auto_pending = false;
    } else if (sustain_count() == 0 && !_hold) {
        _auto_pending = true;          // drone promise; fires in process()
    }
}

void SynthEngine::set_hold(bool on) {
    if (on == _hold) return;
    _hold = on;
    if (on) {
        _demote_all();                 // CHOKE: the whole surface ducks out
        _pending_n = 0;
        _auto_pending = false;
    } else if (_flow && sustain_count() == 0) {
        _auto_pending = true;
    }
}

void SynthEngine::trigger(float pitch_norm) { _do_trigger(pitch_norm, 1.f, 0); }

void SynthEngine::trigger_chord(const float* p, int n) {
    if (n <= 1) { _do_trigger(p[0], 1.f, 0); return; }   // COLOR-0 exact path
    if (n > kMaxChord) n = kMaxChord;
    _vel_now = 1.f / std::sqrt(static_cast<float>(n));   // equal-power comp
    _pending_n = 0;
    _do_trigger(p[0], _vel_now, 0);                      // root lands on the beat
    for (int i = 1; i < n; ++i) {                        // rest strews over ~8 ms
        const int ctr = 1 + static_cast<int>(_stab_rng.next_unipolar()
                                             * kStabSpreadS * _sr);
        _pending[_pending_n].ctr = ctr;
        _pending[_pending_n].pitch = p[i];
        _pending[_pending_n].slot = i;
        ++_pending_n;
    }
}

void SynthEngine::_do_trigger(float pitch_norm, float vel, int chord_slot) {
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
        if (chord_slot == 0) _demote_all();   // a new chord replaces the surface
        _sustaining[pick] = true;
        _chord_slot[pick] = chord_slot;
        _voices[pick].set_sustaining(true);
    } else {
        _sustaining[pick] = false;
        _chord_slot[pick] = -1;
    }
    _voices[pick].set_vel(vel);
    _voices[pick].trigger(pitch_to_hz(pitch_norm));   // pitch LATCHED here
}

void SynthEngine::_adjust_surface() {
    int m = 0, worst = -1;
    bool has[kMaxChord] = { false, false, false, false };
    for (int v = 0; v < kVoices; ++v)
        if (_sustaining[v]) {
            ++m;
            const int s = _chord_slot[v];
            if (s >= 0 && s < kMaxChord) has[s] = true;
            if (worst < 0 || _chord_slot[v] > _chord_slot[worst]) worst = v;
        }
    if (m == 0) return;                    // no surface -> nothing to grow
    _vel_now = 1.f / std::sqrt(static_cast<float>(_chord_n));
    if (_chord_n > m) {                    // bloom: add the first missing slot
        for (int s = 0; s < _chord_n; ++s)
            if (!has[s]) { _do_trigger(_chord[s], _vel_now, s); break; }
    } else if (m > _chord_n && worst >= 0 && _chord_slot[worst] >= _chord_n) {
        _voices[worst].set_sustaining(false);   // collapse: drop the top slot
        _sustaining[worst] = false;
        _chord_slot[worst] = -1;
    }
}

void SynthEngine::_update_control() {
    // chord surface follows COLOR live (spec §2 amendment): one voice per
    // control tick blooms in / the top slot collapses out — never while a
    // stab is still strewing in
    if (_flow && !_hold && _pending_n == 0) _adjust_surface();

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

    // surface voices track their chord slot; vel follows the chord size
    for (int v = 0; v < kVoices; ++v)
        if (_sustaining[v]) {
            const int s = _chord_slot[v];
            if (s >= 0 && s < _chord_n)
                _voices[v].set_pitch_hz(pitch_to_hz(_chord[s]));
            _voices[v].set_vel(_vel_now);
        }
}

void SynthEngine::process(float& outL, float& outR) {
    if (_pending_n > 0) {                        // strewed stab tones land here
        int w = 0;
        for (int i = 0; i < _pending_n; ++i) {
            if (--_pending[i].ctr <= 0)
                _do_trigger(_pending[i].pitch, _vel_now, _pending[i].slot);
            else
                _pending[w++] = _pending[i];
        }
        _pending_n = w;
    }
    if (_auto_pending) {
        _auto_pending = false;
        trigger_chord(_chord, _chord_n);         // full chord at current COLOR
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

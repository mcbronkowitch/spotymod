#pragma once
#include <cmath>

namespace spky {

// AD / ADS envelope with exponential segments.
//
// - Attack: one-pole rise toward an overshoot target (1.2), so the level
//   crosses 1.0 in exactly the programmed attack time (ln 6 law), then
//   switches to decay. Because the rise starts from the CURRENT level,
//   trigger() doubles as the click-free retrigger-from-level used on steals.
// - Decay: one-pole fall toward the sustain level. decay_s is defined as the
//   time to fall 60 dB (ln 1000 law). Sustain > 0 = ADS hold (the FLOW
//   sustaining voice); sustain 0 = plain AD. Setting sustain to 0 while
//   holding IS the demotion release: the same coefficient now converges to
//   zero, i.e. "released: decays to zero at the decay rate" (spec).
// - Idle below 1e-4 (-80 dB): level snaps to exact 0 and process() is free.
//
// set_times uses std::exp - CONTROL RATE ONLY (SynthEngine calls it once per
// 96-sample block); process() is a multiply-add.
class Env {
public:
    void init(float sample_rate) {
        _sr = sample_rate;
        _level = 0.f;
        _sustain = 0.f;
        _stage = Stage::Idle;
        set_times(0.01f, 0.5f);
    }

    void set_times(float attack_s, float decay_s) {      // control rate
        if (attack_s < 1e-4f) attack_s = 1e-4f;
        if (decay_s  < 1e-3f) decay_s  = 1e-3f;
        // SynthEngine::_update_control pushes these at every control tick for
        // every voice, from values that only move when the master cycle length
        // or the ATK/DEC knobs do -- i.e. almost never. That was 16 libm expf
        // per 96-sample block on the two-part instrument, computing the
        // coefficients they already held. The guard is exact: identical inputs
        // give identical coefficients, so nothing downstream can tell.
        if (attack_s == _a_time && decay_s == _d_time) return;
        _a_time = attack_s;
        _d_time = decay_s;
        _a_coef = 1.f - std::exp(-1.7918f / (attack_s * _sr));   // ln 6
        _d_coef = 1.f - std::exp(-6.9078f / (decay_s * _sr));    // ln 1000
    }

    void set_sustain(float s) { _sustain = s < 0.f ? 0.f : (s > 1.f ? 1.f : s); }

    void trigger() { _stage = Stage::Attack; }           // rises from current level

    float process() {
        switch (_stage) {
            case Stage::Idle:
                return 0.f;
            case Stage::Attack:
                _level += _a_coef * (kAttackTarget - _level);
                if (_level >= 1.f) { _level = 1.f; _stage = Stage::Decay; }
                break;
            case Stage::Decay:
                _level += _d_coef * (_sustain - _level);
                if (_sustain <= 0.f && _level < kSilence) {
                    _level = 0.f;
                    _stage = Stage::Idle;
                }
                break;
        }
        return _level;
    }

    bool  active() const { return _stage != Stage::Idle; }
    float value()  const { return _level; }

private:
    enum class Stage { Idle, Attack, Decay };

    static constexpr float kAttackTarget = 1.2f;   // overshoot: finite rise time
    static constexpr float kSilence = 1e-4f;       // -80 dB idle threshold

    float _sr = 48000.f;
    float _level = 0.f;
    float _sustain = 0.f;
    float _a_time = -1.f;
    float _d_time = -1.f;
    float _a_coef = 0.01f;
    float _d_coef = 0.001f;
    Stage _stage = Stage::Idle;
};

} // namespace spky

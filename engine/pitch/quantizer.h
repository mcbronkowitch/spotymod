#pragma once
#include <cstdint>
#include <cmath>
#include "util/math.h"

namespace spky {

enum class QuantMode { Scale, Chrom, Free };

// Global scale list, ordered dark -> bright (spec: the selection knob sweep
// is a brightness axis). Bit i set = semitone i relative to root is allowed.
enum ScaleId {
    SCALE_MIN_PENT = 0,
    SCALE_AEOLIAN,
    SCALE_DORIAN,      // boot default
    SCALE_MAJ_PENT,
    SCALE_LYDIAN,
    SCALE_WHOLE,
    SCALE_LIST_COUNT
};

constexpr uint16_t SCALE_MASKS[SCALE_LIST_COUNT] = {
    0x04A9,  // minor pentatonic  0 3 5 7 10
    0x05AD,  // aeolian           0 2 3 5 7 8 10
    0x06AD,  // dorian            0 2 3 5 7 9 10
    0x0295,  // major pentatonic  0 2 4 7 9
    0x0AD5,  // lydian            0 2 4 6 7 9 11
    0x0555,  // whole tone        0 2 4 6 8 10
};

constexpr uint16_t CHROM_MASK = 0x0FFF;

// Scale quantizer on the pitch contract: normalized 0..1 = 36 semitones
// (3 octaves). Part applies it as the last stage of the PITCH target; voices
// later apply it to the target + V/Oct sum. FREE returns the input untouched.
class Quantizer {
public:
    static constexpr float SPAN_SEMIS = 36.f;
    static constexpr float HYST_SEMIS = 0.30f;   // switch ~15 cents past midpoint

    void init(float sample_rate) {
        _slew_len = static_cast<int>(sample_rate * 0.04f);   // ~40 ms change slew
        _slew_ctr = 0;
        _have_note = false;
        _have_out = false;
    }

    void set_scale(uint16_t mask12) { if (mask12 != _scale) { _scale = mask12; on_change(); } }
    void set_mode(QuantMode m)      { if (m != _mode)       { _mode = m;       on_change(); } }
    void set_root(int semis)        { if (semis != _root)   { _root = semis;   on_change(); } }

    QuantMode mode() const { return _mode; }

    float process(float norm) {
        if (_mode == QuantMode::Free) {
            _last_out = norm;
            _have_out = true;
            _have_note = false;
            return norm;
        }
        const uint16_t mask = (_mode == QuantMode::Chrom) ? CHROM_MASK : _scale;
        const float semis = clampf(norm, 0.f, 1.f) * SPAN_SEMIS;
        int note = nearest_note(semis, mask);
        if (_have_note && note != _last_note && allowed(_last_note, mask)) {
            const float d_last = std::fabs(semis - static_cast<float>(_last_note));
            const float d_note = std::fabs(semis - static_cast<float>(note));
            if (d_last - d_note < HYST_SEMIS) note = _last_note;   // hold
        }
        _last_note = note;
        _have_note = true;

        float out = static_cast<float>(note) / SPAN_SEMIS;
        if (_slew_ctr > 0) {
            --_slew_ctr;
            const float t = 1.f - static_cast<float>(_slew_ctr) / static_cast<float>(_slew_len);
            out = lerpf(_slew_from, out, t);
        }
        _last_out = out;
        _have_out = true;
        return out;
    }

private:
    void on_change() {
        _have_note = false;                       // re-pick without hysteresis
        if (_have_out && _mode != QuantMode::Free) {
            _slew_from = _last_out;               // soften the jump (~40 ms)
            _slew_ctr = _slew_len;
        } else {
            _slew_ctr = 0;                        // into FREE: instant passthrough
        }
    }

    bool allowed(int k, uint16_t mask) const {
        int deg = (k - _root) % 12;
        if (deg < 0) deg += 12;
        return (mask >> deg) & 1;
    }

    int nearest_note(float semis, uint16_t mask) const {
        const int center = static_cast<int>(semis + 0.5f);
        for (int d = 0; d <= 12; ++d) {
            const int lo = center - d, hi = center + d;
            const bool lo_ok = lo >= 0 && allowed(lo, mask);
            const bool hi_ok = hi <= 36 && allowed(hi, mask);
            if (lo_ok && hi_ok && lo != hi)
                return std::fabs(semis - static_cast<float>(hi))
                     < std::fabs(semis - static_cast<float>(lo)) ? hi : lo;
            if (lo_ok) return lo;
            if (hi_ok) return hi;
        }
        return center;  // unreachable with a non-empty mask
    }

    QuantMode _mode  = QuantMode::Scale;
    uint16_t  _scale = SCALE_MASKS[SCALE_DORIAN];
    int       _root  = 0;
    int       _last_note = 0;
    bool      _have_note = false;
    float     _last_out  = 0.f;
    bool      _have_out  = false;
    float     _slew_from = 0.f;
    int       _slew_ctr  = 0;
    int       _slew_len  = 1920;
};

} // namespace spky

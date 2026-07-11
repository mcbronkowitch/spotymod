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

    void init(float sample_rate) { (void)sample_rate; }

    void set_scale(uint16_t mask12) { _scale = mask12; }
    void set_mode(QuantMode m)      { _mode = m; }
    void set_root(int semis)        { _root = semis; }

    QuantMode mode() const { return _mode; }

    float process(float norm) {
        if (_mode == QuantMode::Free) return norm;
        const uint16_t mask = (_mode == QuantMode::Chrom) ? CHROM_MASK : _scale;
        const float semis = clampf(norm, 0.f, 1.f) * SPAN_SEMIS;
        return static_cast<float>(nearest_note(semis, mask)) / SPAN_SEMIS;
    }

private:
    bool allowed(int k, uint16_t mask) const {
        int deg = (k - _root) % 12;
        if (deg < 0) deg += 12;
        return (mask >> deg) & 1;
    }

    // Outward search from the rounded center: the first allowed note at
    // integer distance d is the float-nearest up to the lo/hi tie, which is
    // resolved by comparing real distances (equal -> lower note wins).
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
};

} // namespace spky

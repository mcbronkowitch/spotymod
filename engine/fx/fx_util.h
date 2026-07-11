#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// Ports of the original firmware's src/core/{xfade,softswitch,hann}.h with the
// libDaisy-adjacent includes stripped. Sound-identical: same square-law
// crossfade, same 4 ms Hann micro-ASR for click-free FX switching.

namespace spky {

inline const std::array<float, 192>& hann_curve() {
    static const std::array<float, 192> table = [] {
        std::array<float, 192> t{};
        constexpr float half_pi = 1.5707963705f;
        for (size_t i = 0; i < t.size(); ++i) {
            float s = std::sin(half_pi * static_cast<float>(i)
                                       / static_cast<float>(t.size() - 1));
            t[i] = std::clamp(s * s, 0.f, 1.f);
        }
        return t;
    }();
    return table;
}

inline float hann_value_at(float norm_pos) {
    const auto& curve = hann_curve();
    float pos = static_cast<float>(curve.size() - 1) * norm_pos;
    auto ipos = static_cast<size_t>(pos);
    float frac = pos - static_cast<float>(ipos);
    size_t npos = ipos + 1 >= curve.size() ? curve.size() - 1 : ipos + 1;
    return curve[ipos] + (curve[npos] - curve[ipos]) * frac;
}

// The square law crossfade
// Adopted from Will. C. Pirkle "Designing Software Synthesizer Plugins in C++".
class XFade {
public:
    XFade() = default;
    XFade(const XFade&) = delete;
    XFade& operator=(const XFade&) = delete;

    void Process(float lhs0, float lhs1, float rhs0, float rhs1,
                 float& out0, float& out1) const {
        out0 = lhs0 * _lhs + rhs0 * _rhs;
        out1 = lhs1 * _lhs + rhs1 * _rhs;
    }

    void SetStage(float value) {
        _stage = std::clamp(value, 0.f, 1.f);
        float sq = _stage * _stage;
        _lhs = 1.f - sq;
        _rhs = 2.f * _stage - sq;
    }

    float Stage() const { return _stage; }

private:
    float _stage = 0.f;
    float _lhs = 1.f;
    float _rhs = 0.f;
};

// Four-millisecond micro ASR envelope for click-free on/off switching.
class SoftSwitch {
public:
    SoftSwitch() = default;
    SoftSwitch(const SoftSwitch&) = delete;
    SoftSwitch& operator=(const SoftSwitch&) = delete;

    void init(float sample_rate) { _kof = 1.f / (0.004f * sample_rate); }

    void set_on(bool on, bool immediate = false) {
        _on = on;
        if (immediate) _stage = _on ? Stage::hold : Stage::idle;
    }

    bool is_on() const { return _on; }
    bool is_idle() const { return _stage == Stage::idle; }

    float process(bool inverse = false) {
        switch (_stage) {
            case Stage::idle:
                _out = 0.f;
                _iterator = 0;
                if (_on) _stage = Stage::rise;
                break;
            case Stage::rise:
                if (!_on) _stage = Stage::fall;
                else _out = hann_value_at(_iterator * _kof);
                if (++_iterator >= 191) _stage = Stage::hold;
                break;
            case Stage::hold:
                _out = 1.f;
                _iterator = 191;
                if (!_on) _stage = Stage::fall;
                break;
            case Stage::fall:
                if (_on) _stage = Stage::rise;
                else _out = hann_value_at(_iterator * _kof);
                if (--_iterator <= 0) _stage = Stage::idle;
                break;
        }
        return std::clamp(inverse ? 1.f - _out : _out, 0.f, 1.f);
    }

private:
    enum class Stage { idle, rise, hold, fall };

    int32_t _iterator = 0;
    float _kof = 1.f;
    float _out = 0.f;
    Stage _stage = Stage::idle;
    bool _on = false;
};

} // namespace spky

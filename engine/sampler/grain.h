#pragma once
#include "sampler/sample_buffer.h"
#include "fx/fx_util.h"      // hann_value_at
#include "util/fast_sin.h"
#include "util/math.h"

namespace spky {

// One grain of the texture deck's cloud.
//
// Everything is latched at spawn() -- start position, pitch ratio, pan,
// window shape, direction -- so a moving lane makes the cloud audibly drag
// behind rather than smearing every running grain. process() is then pure
// playback with no parameter reads at all.
//
// The window is two halves of the shared rising Hann table (fx_util.h):
// attack over `atk` samples, decay over `dec`, unity in between. An unequal
// split IS the skew control, so ATK/DEC alone shape soft vs percussive.
class Grain {
public:
    // len/atk/dec are in output samples. atk and dec are clamped so they
    // cannot overlap -- a caller asking for more than the grain has gets
    // both halves scaled proportionally, never a fold-over.
    //
    // No overlap-normalization gain is latched here. That was tried (a
    // `gain` parameter folded into the pan gains below, by analogy with
    // SynthEngine::trigger_chord's per-trigger 1/sqrt(n) latch) and measured
    // worse: a chord's notes are triggered together and share one group that
    // never outlives its count, but grains spawn independently and overlap
    // arbitrarily -- there is no group. A gain latched at spawn is correct
    // only at that instant and stale for the rest of the grain's life, so
    // the summed cloud stops matching the live grain count. The
    // overlap-normalization factor (1/sqrt(active), the COLOR loudness law)
    // is instead computed globally and smoothed in SamplerEngine, and
    // applied once to the summed cloud (see SamplerEngine::_norm).
    void spawn(float start, float ratio, float pan, int len,
               int atk, int dec, bool reverse) {
        _len     = len < 2 ? 2 : len;
        if (atk < 1) atk = 1;
        if (dec < 1) dec = 1;
        if (atk + dec > _len) {
            // Scale both to fit, keeping their ratio. Integer-safe.
            const long long total = static_cast<long long>(atk) + dec;
            atk = static_cast<int>((static_cast<long long>(atk) * _len) / total);
            dec = _len - atk;
            if (atk < 1) { atk = 1; dec = _len - 1; }
            if (dec < 1) { dec = 1; atk = _len - 1; }
        }
        _atk     = atk;
        _dec     = dec;
        _pos     = start;
        _ratio   = ratio;
        _reverse = reverse;
        _i       = 0;
        _rel_len = 0;
        _rel_ctr = 0;
        _active  = true;

        // Equal-power pan, the Voice idiom (synth/voice.cpp:89-93):
        // angle 0..0.25 turns, gr = sin(a), gl = sin(a + quarter turn).
        const float a = (clampf(pan, -1.f, 1.f) + 1.f) * 0.125f;
        _gr = fast_sin(a);
        _gl = fast_sin(a + 0.25f);
    }

    bool active() const { return _active; }
    void kill() { _active = false; }

    // Begin a click-free fade from wherever the window currently is. Used by
    // CHOKE and by the STEP gate falling. Leaving FLOW deliberately does NOT
    // call this -- running grains simply finish their own window.
    void release(int fade_len) {
        if (!_active || _rel_len > 0) return;
        _rel_len = fade_len < 1 ? 1 : fade_len;
        _rel_ctr = _rel_len;
        _rel_from = _window();      // freeze the level we are fading from
    }

    void process(const SampleBuffer& buf, float& outL, float& outR) {
        if (!_active) { outL = 0.f; outR = 0.f; return; }

        float l = 0.f, r = 0.f;
        buf.read_linear(_pos, l, r);

        float w;
        if (_rel_len > 0) {
            // Scale the frozen level by a falling Hann -- continuous at the
            // moment release() was called, and reaching exactly zero after
            // _rel_len samples. Decrement BEFORE the lookup/retirement
            // check: checking-then-decrementing (the brief's original form)
            // needs _rel_len + 1 process() calls to ever see _rel_ctr == 0,
            // so the grain never actually retires within its stated
            // fade_len -- caught by "release fades out ... click-free"
            // (CHECK_FALSE(g.active()) failed with `true` after exactly
            // fade_len calls).
            --_rel_ctr;
            w = _rel_from * hann_value_at(static_cast<float>(_rel_ctr)
                                          / static_cast<float>(_rel_len));
            if (_rel_ctr <= 0) _active = false;
        } else {
            w = _window();
        }

        outL = l * w * _gl;
        outR = r * w * _gr;

        _pos += _reverse ? -_ratio : _ratio;
        if (_rel_len == 0 && ++_i >= _len) _active = false;
    }

private:
    float _window() const {
        if (_i < _atk)
            return hann_value_at(static_cast<float>(_i) / static_cast<float>(_atk));
        const int from_end = _len - 1 - _i;
        if (from_end < _dec)
            return hann_value_at(static_cast<float>(from_end) / static_cast<float>(_dec));
        return 1.f;
    }

    float _pos      = 0.f;
    float _ratio    = 1.f;
    float _gl       = 0.7071f;
    float _gr       = 0.7071f;
    float _rel_from = 0.f;
    int   _len      = 0;
    int   _i        = 0;
    int   _atk      = 1;
    int   _dec      = 1;
    int   _rel_len  = 0;
    int   _rel_ctr  = 0;
    bool  _reverse  = false;
    bool  _active   = false;
};

}  // namespace spky

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
        _start   = start;
        _off     = 0.f;
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

    // Observation seam: the absolute frame index this grain will read next.
    float read_pos() const { return _start + _off; }

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
        buf.read_linear(_start + _off, l, r);

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

        _off += _reverse ? -_ratio : _ratio;
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

    // The read position is kept as a start frame plus a RELATIVE offset, and
    // the absolute index is formed only at the read_linear call. An absolute
    // accumulator (`_pos += _ratio`) silently stalls: float spacing at frame
    // 524,288 is 0.0625 and at 1,048,576 it is 0.125, so any ratio at or
    // below the local ulp fails to move it at all and the grain emits DC for
    // its entire window. Both the reachable minimum ratio (~0.0154) and a
    // plain 2^-kPitchOctaves (0.0625) sit at or under that boundary, and
    // MOTION now scatters spawns across the whole buffer, so on a full 42 s
    // recording most spawns land in the coarse region.
    //
    // _off stays small and therefore finely spaced. Its magnitude never
    // exceeds _len * _ratio, so the stall condition _ratio <= ulp(_off) --
    // i.e. _ratio <= _len * _ratio * 2^-23 -- reduces to _len >= 2^23
    // (8,388,608 samples), which SamplerEngine::_spawn_one's pool-throughput
    // ceiling (kGrains * _spawn_every = 4,032,000 at the top of SIZE) keeps
    // out of reach. The two fixes are load-bearing together: without that
    // ceiling, tape mode could ask for a 1.31e8-sample grain and stall this
    // accumulator too.
    //
    // What this does NOT fix, stated plainly: `_start + _off` is still a
    // float, so the value handed to read_linear is still rounded to the
    // representable grid at that frame -- 0.125 at 1.5 M frames. A very low
    // ratio therefore advances the absolute index in 1/8-frame steps every
    // ~8 output samples rather than smoothly. That is a property of using a
    // float frame index into a multi-megaframe buffer at all: it applies at
    // EVERY ratio (a unity-ratio grain reading frame 1.5 M has always landed
    // on the same 1/8-frame grid), it predates this branch, and it is
    // quantization, not a stall -- the grain traverses its material at the
    // correct average rate. Removing it entirely would mean read_linear
    // taking an integer frame and a fraction separately, a wider change to
    // SampleBuffer's carefully-reasoned fold than this fix warrants.
    float _start    = 0.f;
    float _off      = 0.f;
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

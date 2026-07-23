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
// One deliberate exception, added 2026-07-22: trim_total() lets the engine
// SHORTEN a running grain from outside. Latching length turned SIZE into a
// one-way street -- a 42 s grain kept sounding for its full 42 s however far
// the knob came back down, and nothing on the deck could stop it. Only
// shortening is allowed, so the drag-behind character survives on the way up.
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
    //
    // That argument is about the OVERLAP-NORMALIZATION factor specifically:
    // 1/sqrt(active) has to track the live grain count, so latching it is
    // wrong by construction. The `gain` parameter added 2026-07-23 (spec
    // feel-accents) is the opposite case -- an accent read from the grain's
    // own transient, constant for the grain's whole life. Latching it is not
    // just safe there, it is the only correct reading. The two must not be
    // confused: nothing may route _norm through this parameter.
    void spawn(float start, float ratio, float pan, int len,
               int atk, int dec, bool reverse, float gain = 1.f) {
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
        // The accent gain rides ON the pan gains rather than on the window:
        // process() already multiplies by both, so this costs nothing per
        // sample, and _level() stays the pure window -- which is what makes
        // release()/trim_total() inherit the gain instead of squaring it.
        const float a = (clampf(pan, -1.f, 1.f) + 1.f) * 0.125f;
        const float g = gain < 0.f ? 0.f : gain;
        _gr = fast_sin(a) * g;
        _gl = fast_sin(a + 0.25f) * g;
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
        // BEFORE _rel_len is written, not after: _level() branches on
        // _rel_len, so arming first makes it read the release path with a
        // stale (zero) _rel_from and the fade starts from silence instead of
        // from the window -- a full-scale step, which is precisely the click
        // this function exists to avoid. Caught by "release fades out from
        // the current level, click-free" (test_grain.cpp).
        _rel_from = _level();       // freeze the level we are fading from
        _rel_len = fade_len < 1 ? 1 : fade_len;
        _rel_ctr = _rel_len;
    }

    // Cap the grain's TOTAL life at `total` output samples counted from
    // spawn, fading out click-free over at least `min_fade`. This is the one
    // deliberate exception to "everything is latched at spawn": it exists so
    // that turning SIZE DOWN cuts a cloud that is already sounding.
    //
    // Two properties the callers rely on, both load-bearing:
    //
    //   * It can only SHORTEN. A `total` that is longer than what the grain
    //     has left is ignored, so turning SIZE back up never stretches a
    //     running grain and the cloud keeps dragging behind a rising lane
    //     the way this class was built to.
    //   * It is idempotent in `total`, not incremental. Unlike release() it
    //     may be called on a grain that is already fading and will shorten
    //     that fade -- but the result depends only on the `total` handed in,
    //     never on how many times it was called. A lane modulating SIZE
    //     therefore does not compound: the alternative, scaling the
    //     REMAINING life by (new size / previous size) on each control tick,
    //     telescopes over an LFO cycle and collapses the cloud within
    //     seconds (pinned by "a SIZE wobble that returns does not compound"
    //     in tests/test_sampler_engine.cpp).
    //
    // A cap that already lies in the past does not cut: the grain gets
    // `min_fade`, because truncating a window sitting on its plateau is
    // exactly the click the release path exists to avoid.
    void trim_total(int total, int min_fade) {
        if (!_active) return;
        if (min_fade < 1) min_fade = 1;
        int want = total - _i;
        if (want < min_fade) want = min_fade;
        const int left = _rel_len > 0 ? _rel_ctr : _len - _i;
        if (want >= left) return;          // never extend, never loosen
        _rel_from = _level();
        _rel_len  = want;
        _rel_ctr  = want;
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
        // _i counts elapsed samples since spawn UNCONDITIONALLY -- it used to
        // stop the moment a fade was armed, because only _window() read it
        // and _window() is not called during a fade. trim_total's "total,
        // counted from spawn" made it load-bearing again: with a frozen _i,
        // a second trim on an already-fading grain computes its remaining
        // life from a stale elapsed count and leaves the grain longer than
        // asked. Retirement still keys off the fade when one is running, so
        // _i is free to run past _len.
        ++_i;
        if (_rel_len == 0 && _i >= _len) _active = false;
    }

private:
    // The window level the grain is emitting right now, whether it is in its
    // normal window or already fading. Both release() and trim_total() freeze
    // this value to fade from, which is what makes re-arming a fade mid-flight
    // continuous rather than a step.
    float _level() const {
        if (_rel_len <= 0) return _window();
        return _rel_from * hann_value_at(static_cast<float>(_rel_ctr)
                                         / static_cast<float>(_rel_len));
    }

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
    // ACHTUNG: Die Rechnung unten stammt aus der Zeit, als kOverlap eine
    // Compile-Time-Konstante von 8 war, und die dort genannte Pool-Decke
    // (kGrains * _spawn_every = 4 032 000) gilt nur fuer diesen Overlap.
    // Seit DENS ihn zur Laufzeit auf 1 stellen kann, waere dieselbe Decke
    // 32 256 000 und der Stall wieder erreichbar. Was ihn heute ausschliesst,
    // ist die zusaetzliche absolute Grenze kGrainLenCeil = 2^22 in
    // SamplerEngine::_spawn_one, nicht die Pool-Decke.
    //
    // _off stays small and therefore finely spaced. Its magnitude never
    // exceeds _len * _ratio, so the stall condition _ratio <= ulp(_off) --
    // i.e. _ratio <= _len * _ratio * 2^-23 -- reduces to _len >= 2^23
    // (8,388,608 samples), which SamplerEngine::_spawn_one's pool-throughput
    // ceiling (kGrains * _spawn_every = 4,032,000 at the top of SIZE,
    // kOverlap = 8 fixed) used to keep out of reach on its own -- see the
    // note above for what actually does today. The fixes are load-bearing
    // together: without kGrainLenCeil, tape mode could ask for a
    // 1.31e8-sample grain and stall this accumulator.
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

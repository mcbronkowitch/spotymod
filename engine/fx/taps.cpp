#include "fx/taps.h"
#include "fx/flux.h"
#include "fx/fx_util.h"
#include "util/math.h"
#include "Utility/dsp.h"
#include <cmath>

using namespace spky;

// The power-of-two contract TapeTap's AND-mask read depends on, checked once
// here rather than on every construction in the audio path.
static_assert((Flux::kMaxSamples & (Flux::kMaxSamples - 1)) == 0,
              "TapeTap's mask read requires a power-of-two tape");

void spky::derive_offsets(const RhythmView& rv, int32_t tape_len, int32_t out[2]) {
    out[0] = out[1] = tap_tuning::kMuted;
    if (!rv.valid) return;

    int32_t g0 = rv.gap[0];
    int32_t g1 = rv.gap[1];
    if (g0 < tap_tuning::kMinGap || g1 < tap_tuning::kMinGap) return;

    // Uniformity guard: evenly spaced taps ARE a delay (the diagnosis that
    // killed zone S twice). Counting the dry signal at t = 0, the spacings the
    // listener hears are exactly {g0, g1} -- so testing the gaps IS testing
    // the spacings.
    const float mean = 0.5f * (static_cast<float>(g0) + static_cast<float>(g1));
    const float tol  = tap_tuning::kUniformTol * mean;
    if (std::fabs(static_cast<float>(g0) - mean) <= tol &&
        std::fabs(static_cast<float>(g1) - mean) <= tol) {
        g1 = static_cast<int32_t>(tap_tuning::kUniformSpread * static_cast<float>(g0));
        if (g1 < tap_tuning::kMinGap) return;   // too short to spread audibly
    }

    const int32_t limit = tape_len - 2;
    const int32_t o0 = g0;
    // int64_t: g0 and g1 are otherwise-unconstrained int32_t, so g0 + g1 can
    // overflow int32_t (signed overflow is UB) for large inputs. Widening the
    // sum keeps this comparison exact without narrowing before the bound
    // check below, which is what actually decides whether o1 is representable.
    const int64_t o1 = static_cast<int64_t>(g0) + static_cast<int64_t>(g1);
    // Mute, never clamp: clamping would put two taps at the same position,
    // turning a missing echo into a doubled one.
    if (o0 <= limit) out[0] = o0;
    if (o1 <= limit) out[1] = static_cast<int32_t>(o1);
}

void TapBank::init(float sample_rate) {
    _sr = sample_rate > 0.f ? sample_rate : 48000.f;
    _dip_len = static_cast<int32_t>(tap_tuning::kDipSeconds * _sr);
    if (_dip_len < 1) _dip_len = 1;
    _gain_coef = 1.f / (tap_tuning::kGainSlewS * _sr);
    if (_gain_coef > 1.f) _gain_coef = 1.f;
    _dust = 0.f;
    _rot = -1.f;
    _reads = 0;
    for (auto& t : _t) t = Tap{};
    set_rot(0.f);
}

void TapBank::set_dust(float d) {
    _dust = clampf(d, 0.f, 1.f);
    // Tap 0 ramps over the knob's first half, tap 1 over the second: the
    // intermediate positions are an accent hierarchy (strong/weak), which is
    // the groove dimension a stepped tap count could not give.
    for (int i = 0; i < tap_tuning::kTaps; ++i) {
        const float g = clampf(2.f * _dust - static_cast<float>(i), 0.f, 1.f);
        _t[i].gain_target = g * tap_tuning::kTapGain;
    }
}

void TapBank::set_rot(float r) {
    const float v = clampf(r, 0.f, 1.f);
    if (v == _rot) return;      // the powf pair below must not run per tick
    _rot = v;
    _update_filters();
}

void TapBank::_update_filters() {
    // Geometric interpolation: the sweep is even by ear, not by hertz.
    const float lp_hz = tap_tuning::kLpOpenHz
        * std::pow(tap_tuning::kLpSplitHz / tap_tuning::kLpOpenHz, _rot);
    const float hp_hz = tap_tuning::kHpOpenHz
        * std::pow(tap_tuning::kHpSplitHz / tap_tuning::kHpOpenHz, _rot);
    constexpr float two_pi = 6.2831853f;
    _t[0].lp.a = 1.f - std::exp(-two_pi * lp_hz / _sr);
    _t[1].lp.a = 1.f - std::exp(-two_pi * hp_hz / _sr);
    if (_t[0].lp.a > 1.f) _t[0].lp.a = 1.f;
    if (_t[1].lp.a > 1.f) _t[1].lp.a = 1.f;
}

void TapBank::set_offsets(const int32_t off[tap_tuning::kTaps]) {
    for (int i = 0; i < tap_tuning::kTaps; ++i) {
        Tap& t = _t[i];
        const int32_t want = off[i];
        if (want == t.off && t.dip == Dip::run) continue;
        const int32_t d = want > t.off ? want - t.off : t.off - want;
        if (d < tap_tuning::kRelatchMin && t.dip == Dip::run) continue;
        // Dip, never crossfade: at no point may a tap read two positions.
        // Doubling a bank's reads whenever the source pattern changes is a
        // data-dependent worst case, which is the disease this design cures.
        t.next_off = want;
        t.dip = Dip::out;
        t.dip_ctr = _dip_len;
    }
}

void TapBank::process(const TapeTap& tape, float& l, float& r) {
    _reads = 0;
    float sum_l = 0.f, sum_r = 0.f;
    for (int i = 0; i < tap_tuning::kTaps; ++i) {
        Tap& t = _t[i];

        daisysp::fonepole(t.gain, t.gain_target, _gain_coef);
        // Snap: a one-pole approaches 0 asymptotically, so without this the
        // read would never actually be skipped and CPU would never follow the
        // knob down.
        if (t.gain_target == 0.f && t.gain < 1e-4f) t.gain = 0.f;

        float env = 1.f;
        switch (t.dip) {
            case Dip::out:
                env = hann_value_at(static_cast<float>(t.dip_ctr)
                                    / static_cast<float>(_dip_len));
                if (--t.dip_ctr <= 0) { t.off = t.next_off; t.dip = Dip::in; t.dip_ctr = 0; }
                break;
            case Dip::in:
                env = hann_value_at(static_cast<float>(t.dip_ctr)
                                    / static_cast<float>(_dip_len));
                if (++t.dip_ctr >= _dip_len) t.dip = Dip::run;
                break;
            case Dip::run:
                break;
        }

        if (t.gain <= 0.f || t.off == tap_tuning::kMuted) continue;

        // One MONO read per tap: tap 0 off the left tape, tap 1 off the right.
        // The echo path is effectively mono, so this is decorrelation, not
        // information loss -- and it halves the SDRAM traffic.
        const bool right = (i == 1);
        const float s = tape.read(right, t.off);
        ++_reads;

        // Tap 0 low-passes, tap 1 high-passes (x - lp(x)). At ROT 0 both are
        // effectively open and the bank is a plain two-tap delay on purpose.
        const float lp = t.lp.process(s);
        const float v = (i == 0 ? lp : s - lp) * t.gain * env;

        if (i == 0) { sum_l += v * tap_tuning::kPanNear; sum_r += v * tap_tuning::kPanFar; }
        else        { sum_l += v * tap_tuning::kPanFar;  sum_r += v * tap_tuning::kPanNear; }
    }

    l += sum_l;
    r += sum_r;
}

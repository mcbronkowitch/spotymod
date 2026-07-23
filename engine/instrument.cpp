#include "instrument.h"
#include "fx/taps.h"
#include "util/math.h"
#include <cmath>

using namespace spky;

namespace {
constexpr float kHalfPi = 1.57079632679489661923f;
// Keeps the dry level (cos = 0.92, -0.7 dB) with a leaner room than the
// pre-M4.8 fixed mix (wet rides ON TOP of the reverb's internal trim, so
// 0.25 puts it -8.3 dB against the old join; the old balance sits at 0.5,
// -3 dB overall). Chosen by ear, 2026-07-14.
constexpr float kDefaultReverbMix = 0.25f;
constexpr float kMixSmoothS = 0.010f;    // dry/wet gain glide; ear-tunable
}

void Instrument::init(float sample_rate) { init(sample_rate, FxMem{}); }

void Instrument::init(float sample_rate, const FxMem& mem) {
    _sr = sample_rate;
    _reverb = mem.reverb;
    _parts[PART_A].init(sample_rate, 0x1234abcdu,
                        mem.echo[PART_A][0], mem.echo[PART_A][1],
                        mem.sampler_buf[PART_A], mem.sampler_frames);
    _parts[PART_B].init(sample_rate, 0x9e3779b9u,
                        mem.echo[PART_B][0], mem.echo[PART_B][1],
                        mem.sampler_buf[PART_B], mem.sampler_frames);
    if (_reverb) _reverb->init(sample_rate);
    for (int p = 0; p < PART_COUNT; ++p) {
        _rev_dry[p].init(sample_rate, kMixSmoothS);
        _rev_wet[p].init(sample_rate, kMixSmoothS);
    }
    _rev_primed = false;
    _rev_asleep = false;
    set_reverb_mix(kDefaultReverbMix);   // convenience overload -> both decks
    _limiter.init();
    _center.init(sample_rate, 0x5ce47e12u);
    _ctrl_ctr = 0;
    set_tempo_bpm(_bpm);
}

void Instrument::set_reverb_mix(int part, float n) {
    n = clampf(n, 0.f, 1.f);
    if (n <= 0.f)      { _rev_dry_target[part] = 1.f; _rev_wet_target[part] = 0.f; }
    else if (n >= 1.f) { _rev_dry_target[part] = 0.f; _rev_wet_target[part] = 1.f; }
    else {
        _rev_dry_target[part] = std::cos(n * kHalfPi);   // equal-power crossfade
        _rev_wet_target[part] = std::sin(n * kHalfPi);   // rides the SEND, not the return
    }
    if (_rev_wet_target[part] > 0.f) _rev_asleep = false;   // wake into the cleared room
}

void Instrument::set_reverb_mix(float n) {   // convenience: both decks together
    set_reverb_mix(PART_A, n);
    set_reverb_mix(PART_B, n);
}

void Instrument::set_tempo_bpm(float bpm) {
    // The real single door (task 12 finding 2): Transport::set_bpm guards its
    // own readers, but SuperModulator::set_tempo_bpm and Flux::set_bpm each
    // keep their own _bpm and bypass Transport entirely -- guarding only
    // Transport left both reachable with an unvalidated value, including
    // host/render/scenario.cpp's unvalidated scenario-file `bpm` field, which
    // forwards straight into this method. Dropped silently, same policy as
    // Transport::set_bpm: the last good tempo is kept rather than clamped to
    // an arbitrary floor.
    if (!(bpm > 0.f) || !std::isfinite(bpm)) return;
    _bpm = bpm;
    _center.set_tempo_bpm(bpm);
    for (auto& p : _parts) p.mod().set_tempo_bpm(bpm);
    for (auto& p : _parts) p.fx().set_bpm(bpm);
}

void Instrument::process(const float* inL, const float* inR,
                         float* outL, float* outR, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const float in_l = inL ? inL[i] : 0.f;
        const float in_r = inR ? inR[i] : 0.f;
        if (_ctrl_ctr == 0) {                 // control-rate center update (per 96 samples)
            _center.update(_parts[PART_A].mod(), _parts[PART_B].mod(),
                           _parts[PART_A], _parts[PART_B]);
            // Cross-feed: each bank's taps are placed by the OTHER bank's
            // groove. Instrument is the only scope where both parts are
            // visible; no Part gets a pointer to its sibling. The views only
            // change once per source-lane cycle, so this is a handful of
            // integer ops at 500 Hz.
            constexpr int32_t kTapeLen = static_cast<int32_t>(Flux::kMaxSamples);
            int32_t off_a[tap_tuning::kTaps], off_b[tap_tuning::kTaps];
            derive_offsets(_parts[PART_B].mod().rhythm(), kTapeLen, off_a);
            derive_offsets(_parts[PART_A].mod().rhythm(), kTapeLen, off_b);
            _parts[PART_A].fx().set_tap_offsets(off_a);
            _parts[PART_B].fx().set_tap_offsets(off_b);
            _ctrl_ctr = Center::kCtrlInterval;
        }
        --_ctrl_ctr;

        // CHOKE: event-priority between the decks (spec 2026-07-16
        // choke-priority, rev. 2 discrete zones + rev. 3 env windows).
        // The panel snaps to -1/-0.5/0/+0.5/+1; negative = A priority.
        const int pri = _choke > 0.f ? PART_B : PART_A;
        const int yld = 1 - pri;
        const float amt = _choke < 0.f ? -_choke : _choke;

        float pl[PART_COUNT], prr[PART_COUNT];
        float psl[PART_COUNT], psr[PART_COUNT];
        _parts[pri].set_inhibit(false);   // knob flips must never strand a part
        _parts[pri].process(in_l, in_r, pl[pri], prr[pri], psl[pri], psr[pri]);
        if (amt > 0.f) {
            // Stage 1 (|c| <= 0.5): blocked while the priority side HOLDS a
            // note — STEP: gate high (note + sustain, the tail is free);
            // FLOW: a drone is always "on". Stage 2 (|c| > 0.5): additionally
            // through the whole audible decay (env floor 1e-4).
            bool window = _parts[pri].gate() || _parts[pri].flow();
            if (!window && amt > 0.5f)
                window = _parts[pri].max_voice_env() > 1e-4f;
            _parts[yld].set_inhibit(window);
        } else {
            _parts[yld].set_inhibit(false);
        }
        _parts[yld].process(in_l, in_r, pl[yld], prr[yld], psl[yld], psr[yld]);

        const float al = pl[PART_A],  ar = prr[PART_A];
        const float bl = pl[PART_B],  br = prr[PART_B];
        const float asl = psl[PART_A], asr = psr[PART_A];
        const float bsl = psl[PART_B], bsr = psr[PART_B];

        const float ga = _center.gain_a();
        const float gb = _center.gain_b();
        float l = al * ga + bl * gb;          // MORPH blend (null-reverb path keeps this)
        float r = ar * ga + br * gb;
        if (_reverb) {
            if (!_rev_primed) {              // snap the mix set before the first block
                for (int p = 0; p < PART_COUNT; ++p) {
                    _rev_dry[p].reset(_rev_dry_target[p]);
                    _rev_wet[p].reset(_rev_wet_target[p]);
                }
                if (_rev_wet_target[PART_A] == 0.f && _rev_wet_target[PART_B] == 0.f) {
                    _reverb->clear(); _rev_asleep = true;
                }
                _rev_primed = true;
            }
            const float dga = _rev_dry[PART_A].process(_rev_dry_target[PART_A]);
            const float dgb = _rev_dry[PART_B].process(_rev_dry_target[PART_B]);
            const float wga = _rev_wet[PART_A].process(_rev_wet_target[PART_A]);
            const float wgb = _rev_wet[PART_B].process(_rev_wet_target[PART_B]);
            // Per-deck dry: each deck's dry gain rides its own cos before the
            // MORPH sum, so one deck can be wet-only while the other stays dry.
            l = al * ga * dga + bl * gb * dgb;
            r = ar * ga * dga + br * gb * dgb;
            if (!_rev_asleep) {
                // Per-deck send: the equal-power wet curve (sin) rides the SEND
                // -- one shared room has only one return. MORPH fades the send
                // too (M4 rule): a morphed-away deck injects no new reverb.
                float wl, wr;
                _reverb->process(asl * ga * wga + bsl * gb * wgb,
                                 asr * ga * wga + bsr * gb * wgb, wl, wr);
                l += wl;   // wl already carries kWetGain; the return joins at unity
                r += wr;
                if (wga == 0.f && wgb == 0.f &&
                    _rev_wet_target[PART_A] == 0.f && _rev_wet_target[PART_B] == 0.f) {
                    _reverb->clear();        // clear-on-sleep: waking starts empty
                    _rev_asleep = true;      // Oliverb CPU is off until a MIX reopens
                }
            }
            // asleep: dga/dgb have snapped to 1 (both decks mix 0), so l/r stay full dry
        }
        _limiter.process(l, r);   // master ceiling (M6 engine delta 3, delivered early)
        outL[i] = l;
        outR[i] = r;
    }
}

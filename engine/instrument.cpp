#include "instrument.h"
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
                        mem.echo[PART_A][0], mem.echo[PART_A][1]);
    _parts[PART_B].init(sample_rate, 0x9e3779b9u,
                        mem.echo[PART_B][0], mem.echo[PART_B][1]);
    if (_reverb) _reverb->init(sample_rate);
    _rev_dry.init(sample_rate, kMixSmoothS);
    _rev_wet.init(sample_rate, kMixSmoothS);
    _rev_primed = false;
    _rev_asleep = false;
    set_reverb_mix(kDefaultReverbMix);
    _limiter.init();
    _center.init(sample_rate, 0x5ce47e12u);
    _ctrl_ctr = 0;
    _choke_rng.seed(0xc0edbabeu);
    // Boot: the FLOW auto-drone makes a part audible before its first pitch
    // lane fire ever rolls a claim, so start claimed. Inert at choke 0 (the
    // bypass path forces _claim = false) and in zone 1 (window is gate-only,
    // and the gate-opening fire re-rolls the claim first).
    _claim = true;
    set_tempo_bpm(_bpm);
}

void Instrument::set_reverb_mix(float n) {
    n = clampf(n, 0.f, 1.f);
    if (n <= 0.f)      { _rev_dry_target = 1.f; _rev_wet_target = 0.f; }
    else if (n >= 1.f) { _rev_dry_target = 0.f; _rev_wet_target = 1.f; }
    else {
        _rev_dry_target = std::cos(n * kHalfPi);   // equal-power crossfade
        _rev_wet_target = std::sin(n * kHalfPi);
    }
    if (_rev_wet_target > 0.f) _rev_asleep = false;   // wake into the cleared room
}

void Instrument::set_tempo_bpm(float bpm) {
    _bpm = bpm;
    _center.set_tempo_bpm(bpm);
    for (auto& p : _parts) p.mod().set_tempo_bpm(bpm);
}

void Instrument::process(const float* /*inL*/, const float* /*inR*/,
                         float* outL, float* outR, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (_ctrl_ctr == 0) {                 // control-rate center update (per 96 samples)
            _center.update(_parts[PART_A].mod(), _parts[PART_B].mod(),
                           _parts[PART_A], _parts[PART_B]);
            _ctrl_ctr = Center::kCtrlInterval;
        }
        --_ctrl_ctr;

        // CHOKE: event-priority between the decks (spec 2026-07-16
        // choke-priority). Zone 1 (|c| <= 0.25): each priority-side fire
        // claims with p = |c|/0.25 and blocks the other side while its gate
        // is high. Zone 2: all fires claim and the window grows into the
        // envelope decay (env threshold 1-w, floored so w=1 means "audible").
        const int pri = _choke > 0.f ? PART_B : PART_A;
        const int yld = 1 - pri;
        const float amt = _choke < 0.f ? -_choke : _choke;

        float pl[PART_COUNT], prr[PART_COUNT];
        float psl[PART_COUNT], psr[PART_COUNT];
        _parts[pri].set_inhibit(false);   // knob flips must never strand a part
        _parts[pri].process(pl[pri], prr[pri], psl[pri], psr[pri]);
        if (amt > 0.f) {
            if (_parts[pri].lane_fired(LANE_PITCH)) {
                const float p = amt >= 0.25f ? 1.f : amt * 4.f;
                _claim = _choke_rng.next_unipolar() < p;
            }
            bool window = _parts[pri].gate();
            if (!window && amt > 0.25f) {
                const float w = (amt - 0.25f) * (1.f / 0.75f);
                window = _parts[pri].max_voice_env() > 1.f - w + 1e-4f;
            }
            _parts[yld].set_inhibit(_claim && window);
        } else {
            _claim = false;
            _parts[yld].set_inhibit(false);
        }
        _parts[yld].process(pl[yld], prr[yld], psl[yld], psr[yld]);

        const float al = pl[PART_A],  ar = prr[PART_A];
        const float bl = pl[PART_B],  br = prr[PART_B];
        const float asl = psl[PART_A], asr = psr[PART_A];
        const float bsl = psl[PART_B], bsr = psr[PART_B];

        const float ga = _center.gain_a();
        const float gb = _center.gain_b();
        float l = al * ga + bl * gb;          // MORPH: equal-power A<->B blend
        float r = ar * ga + br * gb;
        if (_reverb) {
            if (!_rev_primed) {              // snap a mix set before the first block
                _rev_dry.reset(_rev_dry_target);
                _rev_wet.reset(_rev_wet_target);
                if (_rev_wet_target == 0.f) { _reverb->clear(); _rev_asleep = true; }
                _rev_primed = true;
            }
            const float dg = _rev_dry.process(_rev_dry_target);
            const float wg = _rev_wet.process(_rev_wet_target);
            if (!_rev_asleep) {
                // MORPH fades dry AND send together (M4 supersedes the M1.6
                // pre-morph-send rule): a fully morphed-away part injects no new
                // reverb; only its already-committed tail rings out.
                float wl, wr;
                _reverb->process(asl * ga + bsl * gb, asr * ga + bsr * gb, wl, wr);
                l = l * dg + wl * wg;
                r = r * dg + wr * wg;
                if (wg == 0.f && dg == 1.f && _rev_wet_target == 0.f) {
                    _reverb->clear();        // clear-on-sleep: waking starts empty
                    _rev_asleep = true;      // Oliverb CPU is off until MIX reopens
                }
            }
            // asleep: dry passes bit-exact (dg has snapped to 1), sends discarded
        }
        _limiter.process(l, r);   // master ceiling (M6 engine delta 3, delivered early)
        outL[i] = l;
        outR[i] = r;
    }
}

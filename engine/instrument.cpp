#include "instrument.h"

using namespace spky;

void Instrument::init(float sample_rate) { init(sample_rate, FxMem{}); }

void Instrument::init(float sample_rate, const FxMem& mem) {
    _sr = sample_rate;
    _reverb = mem.reverb;
    _parts[PART_A].init(sample_rate, 0x1234abcdu,
                        mem.echo[PART_A][0], mem.echo[PART_A][1]);
    _parts[PART_B].init(sample_rate, 0x9e3779b9u,
                        mem.echo[PART_B][0], mem.echo[PART_B][1]);
    if (_reverb) _reverb->init(sample_rate);
    _limiter.init();
    _center.init(sample_rate, 0x5ce47e12u);
    _ctrl_ctr = 0;
    set_tempo_bpm(_bpm);
}

void Instrument::set_tempo_bpm(float bpm) {
    _bpm = bpm;
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

        float al, ar, bl, br;
        float asl, asr, bsl, bsr;
        _parts[PART_A].process(al, ar, asl, asr);
        _parts[PART_B].process(bl, br, bsl, bsr);

        const float ga = _center.gain_a();
        const float gb = _center.gain_b();
        float l = al * ga + bl * gb;          // MORPH: equal-power A<->B blend
        float r = ar * ga + br * gb;
        if (_reverb) {
            // MORPH fades dry AND send together (M4 supersedes the M1.6
            // pre-morph-send rule): a fully morphed-away part injects no new
            // reverb; only its already-committed tail rings out.
            float wl, wr;
            _reverb->process(asl * ga + bsl * gb, asr * ga + bsr * gb, wl, wr);
            l += wl;
            r += wr;
        }
        _limiter.process(l, r);   // master ceiling (M6 engine delta 3, delivered early)
        outL[i] = l;
        outR[i] = r;
    }
}

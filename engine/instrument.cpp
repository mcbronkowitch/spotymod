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
    set_tempo_bpm(_bpm);
}

void Instrument::set_tempo_bpm(float bpm) {
    _bpm = bpm;
    for (auto& p : _parts) p.mod().set_tempo_bpm(bpm);
}

void Instrument::process(const float* /*inL*/, const float* /*inR*/,
                         float* outL, float* outR, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        float al, ar, bl, br;
        float asl, asr, bsl, bsr;
        _parts[PART_A].process(al, ar, asl, asr);
        _parts[PART_B].process(bl, br, bsl, bsr);
        float l = (al + bl) * 0.5f;   // MORPH/center mixing arrives in M4
        float r = (ar + br) * 0.5f;
        if (_reverb) {
            // sends tapped pre-morph; the shared room joins AFTER the part
            // mix — a part morphed away can still haunt the room (spec).
            float wl, wr;
            _reverb->process(asl + bsl, asr + bsr, wl, wr);
            l += wl;
            r += wr;
        }
        outL[i] = l;
        outR[i] = r;
    }
}

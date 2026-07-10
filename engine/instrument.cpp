#include "instrument.h"

using namespace spky;

void Instrument::init(float sample_rate) {
    _sr = sample_rate;
    _parts[PART_A].init(sample_rate, 0x1234abcdu);
    _parts[PART_B].init(sample_rate, 0x9e3779b9u);
    set_tempo_bpm(_bpm);
}

void Instrument::set_tempo_bpm(float bpm) {
    _bpm = bpm;
    for (auto& p : _parts) p.mod().set_tempo_bpm(bpm);
}

void Instrument::process(const float* /*inL*/, const float* /*inR*/,
                         float* outL, float* outR, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        float al = 0.f, ar = 0.f, bl = 0.f, br = 0.f;
        _parts[PART_A].process(al, ar);
        _parts[PART_B].process(bl, br);
        outL[i] = (al + bl) * 0.5f;   // MORPH/center mixing arrives in M4
        outR[i] = (ar + br) * 0.5f;
    }
}

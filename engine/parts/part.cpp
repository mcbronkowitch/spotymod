#include "parts/part.h"

using namespace spky;

void Part::init(float sample_rate, uint32_t seed_base) {
    _sr = sample_rate;
    _mod.init(sample_rate, seed_base);
    _tone.init(sample_rate);
    _engine = &_tone;
    _gate_len = static_cast<int>(sample_rate * 0.005f);
    _gate_ctr = 0;
}

float Part::target_value(int slot) const {
    float mod = _active[slot] ? _mod.lane_output(slot) * _depth * _tdepth[slot] : 0.f;
    return clampf(_base[slot] + mod, 0.f, 1.f);
}

void Part::process(float& outL, float& outR) {
    _mod.process();

    if (_mod.lane_fired(LANE_PITCH)) _gate_ctr = _gate_len;
    if (_gate_ctr > 0) --_gate_ctr;

    float targets[LANE_COUNT];
    for (int i = 0; i < LANE_COUNT; ++i) targets[i] = target_value(i);

    _engine->set_targets(targets, _tune);
    if (_mod.lane_fired(LANE_PITCH)) _engine->trigger(targets[LANE_PITCH]);
    _engine->process(outL, outR);
}

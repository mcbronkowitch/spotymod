#include "parts/part.h"

using namespace spky;

void Part::init(float sample_rate, uint32_t seed_base) {
    _sr = sample_rate;
    _mod.init(sample_rate, seed_base);
    _tone.init(sample_rate);
    _engine = &_tone;
    _gate_len = static_cast<int>(sample_rate * 0.005f);
    _gate_ctr = 0;
    _quant.init(sample_rate);                  // boots Dorian / SCALE / root 0
    _pitch_q = _quant.process(pitch_pre_quant());
}

float Part::target_raw(int slot) const {
    float mod = _active[slot] ? _mod.lane_output(slot) * _depth * _tdepth[slot] : 0.f;
    return clampf(_base[slot] + mod, 0.f, 1.f);
}

// PITCH target + TUNE offset, summed BEFORE quantization so the final audible
// pitch always lands on the scale grid (tune is a bipolar +/-18-semi transpose,
// 0.5 = neutral). Quantizing the sum keeps both parts on one shared grid.
float Part::pitch_pre_quant() const {
    return clampf(target_raw(LANE_PITCH) + (_tune - 0.5f), 0.f, 1.f);
}

float Part::target_value(int slot) const {
    return slot == LANE_PITCH ? _pitch_q : target_raw(slot);
}

void Part::process(float& outL, float& outR) {
    _mod.process();

    if (_mod.lane_fired(LANE_PITCH)) _gate_ctr = _gate_len;
    if (_gate_ctr > 0) --_gate_ctr;

    float targets[LANE_COUNT];
    for (int i = 0; i < LANE_COUNT; ++i) targets[i] = target_raw(i);
    targets[LANE_PITCH] = _quant.process(pitch_pre_quant());
    _pitch_q = targets[LANE_PITCH];

    _engine->set_targets(targets, _tune);
    if (_mod.lane_fired(LANE_PITCH)) _engine->trigger(targets[LANE_PITCH]);
    _engine->process(outL, outR);
}

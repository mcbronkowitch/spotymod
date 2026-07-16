#include "parts/part.h"

using namespace spky;

void Part::init(float sample_rate, uint32_t seed_base,
                float* echo_l, float* echo_r) {
    _sr = sample_rate;
    _mod.init(sample_rate, seed_base);
    _tone.init(sample_rate);
    _synth.set_seed(seed_base ^ 0x5eedC0DEu);   // per-part drift decorrelation
    _synth.init(sample_rate);
    _engine_id = ENGINE_SYNTH;                  // boot default (M2 spec)
    _pending_engine = _engine_id;
    _switching = false;
    _engine = _engine_for(_engine_id);
    _engine_fade.init(sample_rate);
    _engine_fade.set_on(true, true);            // boot: engine fully on
    _step_on = false;
    _engine->set_flow(true);                    // lanes boot in FLOW -> drone
    _last_master_hz = -1.f;                     // force a cycle forward on
                                                // the first process()
    _fx.init(sample_rate, echo_l, echo_r);
    _gate_len = static_cast<int>(sample_rate * 0.005f);
    _gate_ctr = 0;
    _inhibit = false;
    _note_suppressed = false;
    _quant.init(sample_rate);                   // boots Dorian / SCALE / root 0
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

// Same combine rule as target_raw, tapped from the SAME lanes — the FX breathe
// in the part's own character. Never quantized (that is a PITCH-lane concern).
float Part::fx_target_value(int slot) const {
    float mod = _fx_active[slot]
        ? _mod.lane_output(slot) * _depth * _fx_depth[slot] : 0.f;
    return clampf(_fx_base[slot] + mod, 0.f, 1.f);
}

void Part::set_engine(EngineId e) {
    if (_switching ? e == _pending_engine : e == _engine_id) return;
    _pending_engine = e;
    _switching = true;
    _engine_fade.set_on(false);   // fade out; process() swaps at the idle point
}

void Part::set_step(bool on, int steps) {
    _step_on = on;
    _mod.set_step(on, steps);
    _engine->set_flow(!on);
}

void Part::trigger_manual() {
    _gate_ctr = _gate_len;
    _engine->trigger(target_value(LANE_PITCH));   // current quantized pitch
}

float Part::max_voice_env() const {
    float m = 0.f;
    for (int v = 0; v < SynthEngine::kVoices; ++v) {
        const float e = voice_env(v);   // engine-qualified: 0 on test tone
        if (e > m) m = e;
    }
    return m;
}

void Part::process(float& outL, float& outR, float& sendL, float& sendR) {
    _mod.process();

    // click-free engine switch: fade out (4 ms) -> swap -> fade in (4 ms).
    // At hold the multiplier is exactly 1.0, so unswitched runs stay
    // bit-identical (M1.6 bypass invariant).
    const float fade = _engine_fade.process();
    if (_switching && _engine_fade.is_idle()) {
        _engine_id = _pending_engine;
        _engine = _engine_for(_engine_id);
        _engine->set_flow(!_step_on);                          // re-sync state
        _engine->set_hold(_inhibit);
        if (_last_master_hz > 0.f) _engine->set_cycle(1.f / _last_master_hz);
        _switching = false;
        _engine_fade.set_on(true);
    }

    // forward the master-lane cycle length on change, not per sample
    const float hz = _mod.master_hz();
    if (hz != _last_master_hz && hz > 0.f) {
        _last_master_hz = hz;
        _engine->set_cycle(1.f / hz);
    }

    if (_mod.lane_fired(LANE_PITCH)) {
        _note_suppressed = _inhibit;
        if (!_inhibit) _gate_ctr = _gate_len;
    }
    if (_gate_ctr > 0) --_gate_ctr;

    float targets[LANE_COUNT];
    for (int i = 0; i < LANE_COUNT; ++i) targets[i] = target_raw(i);
    targets[LANE_PITCH] = _quant.process(pitch_pre_quant());
    _pitch_q = targets[LANE_PITCH];                              // clean, drives pitch_cv()
    targets[LANE_PITCH] = clampf(_pitch_q + _detune_cents * (1.f / 3600.f), 0.f, 1.f);

    _engine->set_targets(targets, _tune);
    if (_mod.lane_fired(LANE_PITCH) && !_note_suppressed)
        _engine->trigger(targets[LANE_PITCH]);
    _engine->process(outL, outR);
    outL *= fade;
    outR *= fade;

    float fxv[FXT_COUNT];
    for (int i = 0; i < FXT_COUNT; ++i) fxv[i] = fx_target_value(i);
    _fx.process(outL, outR, sendL, sendR, fxv);
}

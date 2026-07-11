#include "fx/grit.h"
#include "Utility/dsp.h"
#include "util/math.h"

using namespace spky;

namespace {
inline float dbfs2lin(float db) { return daisysp::pow10f(db * 0.05f); }
inline float attenuation_for_intensity(float intensity) {
    return dbfs2lin(daisysp::fmap(intensity, -3.f, -24.f));
}
} // namespace

void GritDrive::init(float /*sample_rate*/) {
    _drive.Init();
    _drive.SetDrive(0.f);
    _mix.SetStage(0.33f);
    apply();
}

void GritDrive::set_intensity(float norm) {
    _intensity = clampf(norm, 0.f, 1.f);
    apply();
}

void GritDrive::apply() {
    _attenuation = attenuation_for_intensity(_intensity);
    _drive.SetDrive(dbfs2lin(daisysp::fmap(_intensity, -6.f, 0.f)));
}

void GritDrive::process(float& l, float& r) {
    float d0 = _drive.Process(l) * _attenuation;
    float d1 = _drive.Process(r) * _attenuation;
    _mix.Process(l, r, d0, d1, l, r);
}

void GritReduce::init(float /*sample_rate*/) {
    _decimator.Init();
    _decimator.SetBitsToCrush(16);
    _reducer.Init();
    _reducer.SetFreq(0.6f);
    _mix.SetStage(0.5f);
    apply();
}

void GritReduce::set_intensity(float norm) {
    _intensity = clampf(norm, 0.f, 1.f);
    apply();
}

void GritReduce::apply() {
    _decimator.SetDownsampleFactor(_intensity);
    _decimator.SetBitcrushFactor(
        daisysp::fmap(1.f - _intensity, 0.5f, 0.7f, daisysp::Mapping::EXP));
}

void GritReduce::process(float& l, float& r) {
    float r0 = _reducer.Process(_decimator.Process(l));
    float r1 = _reducer.Process(_decimator.Process(r));
    _mix.Process(l, r, r0, r1, l, r);
}

void Grit::init(float sample_rate) {
    _sw.init(sample_rate);
    _drive.init(sample_rate);
    _reduce.init(sample_rate);
    set_intensity(_intensity);
    set_mix(_mix_norm);
}

void Grit::set_mode(GritMode m) {
    _mode = m;
    set_intensity(_intensity);   // re-apply current values to the new mode
    set_mix(_mix_norm);
}

void Grit::set_intensity(float norm) {
    _intensity = clampf(norm, 0.f, 1.f);
    if (_mode == GritMode::Drive) _drive.set_intensity(_intensity);
    else                          _reduce.set_intensity(_intensity);
}

float Grit::intensity() const {
    return _mode == GritMode::Drive ? _drive.intensity() : _reduce.intensity();
}

void Grit::set_mix(float norm) {
    _mix_norm = clampf(norm, 0.f, 1.f);
    _drive.set_mix(_mix_norm);
    _reduce.set_mix(_mix_norm);
}

void Grit::process(float& l, float& r) {
    float k = _sw.process();
    if (_sw.is_idle()) return;   // fully off: bit-exact dry
    float gl = l, gr = r;
    if (_mode == GritMode::Drive) _drive.process(gl, gr);
    else                          _reduce.process(gl, gr);
    l = gl * k + l * (1.f - k);
    r = gr * k + r * (1.f - k);
}

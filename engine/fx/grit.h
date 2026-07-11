#pragma once
#include <cstdint>
#include "Effects/overdrive.h"
#include "Effects/decimator.h"
#include "Effects/sampleratereducer.h"
#include "fx/fx_util.h"

namespace spky {

enum class GritMode : uint8_t { Drive, Reduce };

// Port of the original firmware's Drive (src/core/fx.drive.*): Overdrive with
// intensity-coupled attenuation, square-law dry/wet mix. Original curves kept.
class GritDrive {
public:
    void init(float sample_rate);
    float intensity() const { return _intensity; }
    void set_intensity(float norm);
    float mix() const { return _mix.Stage(); }
    void set_mix(float norm) { _mix.SetStage(norm); }
    void process(float& l, float& r);

private:
    void apply();

    daisysp::Overdrive _drive;
    XFade _mix;
    float _intensity = 0.2f;
    float _attenuation = 1.f;
};

// Port of the original firmware's Reduce (src/core/fx.reduce.*): Decimator +
// SampleRateReducer. NOTE: like the original, the mono DSP objects are shared
// across L/R — part of the original sound identity, kept as-is.
class GritReduce {
public:
    void init(float sample_rate);
    float intensity() const { return _intensity; }
    void set_intensity(float norm);
    float mix() const { return _mix.Stage(); }
    void set_mix(float norm) { _mix.SetStage(norm); }
    void process(float& l, float& r);

private:
    void apply();

    daisysp::Decimator _decimator;
    daisysp::SampleRateReducer _reducer;
    XFade _mix;
    float _intensity = 0.55f;
};

// GRIT block: Drive <-> Reduce behind one click-free SoftSwitch. Replaces the
// original Fx class's grit half; the mode is set explicitly (the original's
// switch_grit_mode() toggle becomes an M6 gesture on top of set_mode()).
class Grit {
public:
    void init(float sample_rate);
    void set_on(bool on, bool immediate = false) { _sw.set_on(on, immediate); }
    bool is_on() const { return _sw.is_on(); }
    // true while audible: on, or still ramping out after switch-off
    bool engaged() const { return _sw.is_on() || !_sw.is_idle(); }
    void set_mode(GritMode m);
    GritMode mode() const { return _mode; }
    void set_intensity(float norm);   // applies to the active mode (original behavior)
    float intensity() const;
    void set_mix(float norm);         // single GRIT MIX layer param -> both modes
    void process(float& l, float& r);

private:
    GritDrive _drive;
    GritReduce _reduce;
    SoftSwitch _sw;
    GritMode _mode = GritMode::Drive;
    float _intensity = 0.3f;
    float _mix_norm = 0.5f;
};

} // namespace spky

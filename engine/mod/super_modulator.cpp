#include "mod/super_modulator.h"
#include "util/math.h"
#include <cmath>

using namespace spky;

namespace {
constexpr float kLaneRatio[LANE_COUNT] = { 2.f, 0.5f, 1.f, 0.75f, 1.5f };
constexpr float kRateFreeMin = 0.02f;
constexpr float kRateFreeMax = 30.f;

float free_hz(float norm) {
    return kRateFreeMin * std::pow(kRateFreeMax / kRateFreeMin, spky::clampf(norm, 0.f, 1.f));
}

float sync_hz(float norm, float bpm, bool triplet) {
    // cycles per beat: 8 bars ... 1/32 note
    static const float cpb[9] = { 1.f/32, 1.f/16, 1.f/8, 1.f/4, 1.f/2, 1.f, 2.f, 4.f, 8.f };
    int i = static_cast<int>(std::lround(spky::clampf(norm, 0.f, 1.f) * 8.f));
    float hz = (bpm / 60.f) * cpb[i];
    return triplet ? hz * 1.5f : hz;
}
} // namespace

void SuperModulator::init(float sample_rate, uint32_t seed_base) {
    _sr = sample_rate;
    for (int i = 0; i < LANE_COUNT; ++i) {
        _lanes[i].init(sample_rate, seed_base + static_cast<uint32_t>(i) * 2654435761u);
        _out[i] = 0.f;
    }
    _capture.reset();
    _lanes[LANE_PITCH].set_capture_loop(&_capture);   // capture is PITCH-only
    _update_rate();
}

void SuperModulator::_update_rate() {
    switch (_mode) {
        case SyncMode::Free:        _master_hz = free_hz(_rate_norm); break;
        case SyncMode::Sync:        _master_hz = sync_hz(_rate_norm, _bpm, false); break;
        case SyncMode::SyncTriplet: _master_hz = sync_hz(_rate_norm, _bpm, true); break;
    }
    for (int i = 0; i < LANE_COUNT; ++i)
        _lanes[i].set_rate_hz(_master_hz * kLaneRatio[i]);
}

void SuperModulator::set_shape(float s)       { for (auto& l : _lanes) l.set_shape(s); }
void SuperModulator::set_probability(float p) { for (auto& l : _lanes) l.set_probability(p); }
void SuperModulator::set_smooth(float s)      { for (auto& l : _lanes) l.set_smooth(s); }
void SuperModulator::set_range(float r)       { for (auto& l : _lanes) l.set_range(r); }
void SuperModulator::set_entropy(float a)     { for (auto& l : _lanes) l.set_entropy(a); }
void SuperModulator::set_step(bool on, int n) { for (auto& l : _lanes) l.set_step(on, n); }
void SuperModulator::set_fixed_slew(bool on)  { for (auto& l : _lanes) l.set_fixed_slew(on); }

void SuperModulator::process() {
    for (int i = 0; i < LANE_COUNT; ++i)
        _out[i] = _lanes[i].process();
}

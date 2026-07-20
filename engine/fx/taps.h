#pragma once
#include <cstdint>
#include "mod/rhythm_view.h"

namespace spky {

// Read-only view over the FLUX tape. Moved verbatim from fx/dust.h, which is
// deleted in task 4. There is only a `mask` -- no size/mask pair that could
// disagree; the power-of-two contract is checked once at compile time in
// taps.cpp against Flux::kMaxSamples, not per construction.
struct TapeTap {
    const float* l = nullptr;
    const float* r = nullptr;
    int32_t write_ptr = 0;
    int32_t mask = 0;

    int32_t size() const { return mask + 1; }

    // `offset` is samples BEHIND the write head; the head decrements, so a
    // constant offset is exactly 1x forward playback of material that old.
    float read(bool right, int32_t offset) const {
        int32_t i = (write_ptr + offset) & mask;
        return (right ? r : l)[i];
    }
};

namespace tap_tuning {

// A muted tap. 0 is safe as the sentinel because a sounding offset is always
// >= kMinGap; an offset of 0 would read at the write head and is never wanted.
constexpr int32_t kMuted = 0;

// Below this a "gap" is not a rhythm, it is a buzz -- and 0.75 * g would round
// toward a second gap equal to the first, defeating the uniformity guard.
constexpr int32_t kMinGap = 32;

// Gaps count as uniform when both lie within this fraction of their mean. A
// fraction, not an absolute count: at 240 samples a 2-sample jitter must not
// read as non-uniform, at 30000 a 50-sample drift must not read as uniform.
constexpr float kUniformTol = 0.02f;

// The spread applied when the guard fires: the MOTION lane's x3/4 ratio, a
// polyrhythm the instrument already runs. Cumulative offsets become g*{1,1.75}
// -- a limp, not a grid.
constexpr float kUniformSpread = 0.75f;

constexpr int   kTaps        = 2;
// Taste constant for the play test: full DUST sits at parity with a direct
// tape read. The grain cloud's 7.27 dB window/pan makeup died with the cloud.
constexpr float kTapGain     = 0.7f;
// Below this, a jump is inaudible against the tape's own band-limit (64
// samples = 1.3 ms at 48 kHz) and dipping for it would be pure cost.
constexpr int32_t kRelatchMin = 64;
constexpr float kDipSeconds  = 0.002f;   // each side of the jump
constexpr float kGainSlewS   = 0.02f;
// Filter endpoints, ROT 0 -> ROT 1, interpolated geometrically.
constexpr float kLpOpenHz    = 18000.f;
constexpr float kLpSplitHz   = 400.f;
constexpr float kHpOpenHz    = 20.f;
constexpr float kHpSplitHz   = 1500.f;
// Equal-power pan at +-22.5 degrees: a spread, not a hard split.
constexpr float kPanNear     = 0.92388f;
constexpr float kPanFar      = 0.38268f;

}  // namespace tap_tuning

// Turn a lane's published rhythm into two tape offsets, in samples behind the
// write head. Pure: no state, no sample rate, no tape. This is where the rule
// that Zone S lacked lives, and it is unit-testable on its own.
//
// out[i] == tap_tuning::kMuted means "this tap does not sound".
void derive_offsets(const RhythmView& rv, int32_t tape_len, int32_t out[2]);

// Two read taps on the FLUX tape, placed by the other bank's rhythm.
//
// Replaces DustCloud. There is no grain pool, no scheduler, no anchor and no
// RNG: the bank is deterministic, and its worst case is constant -- two mono
// reads and two one-poles, whatever the material does. That constancy is the
// point on an instrument already near its block budget.
class TapBank {
public:
    void init(float sample_rate);

    void set_dust(float d);                     // 0..1 morph (gain, tap count)
    void set_rot(float r);                      // 0..1 spectral spread
    void set_offsets(const int32_t off[tap_tuning::kTaps]);

    // True while the bank still has anything to contribute. Deliberately NOT
    // `_dust > 0`: Flux takes its bit-exact bypass when this is false, so
    // reporting inactive the instant the knob hits zero would drop a
    // full-level tap sum in one sample -- a click, defeating the very gain
    // slew that exists to prevent it. Staying active until the slews have
    // snapped to 0 lets the taps ride out, and the bypass is then reached
    // with nothing left to lose.
    bool active() const {
        if (_dust > 0.f) return true;
        for (const auto& t : _t) if (t.gain > 0.f) return true;
        return false;
    }

    // Reads the tape as it stands at the START of the sample; adds into l/r.
    void process(const TapeTap& tape, float& l, float& r);

    // Test/telemetry: tape reads performed on the last process() call. The
    // "a silent tap costs nothing" claim is otherwise unobservable.
    int reads() const { return _reads; }

private:
    struct OnePoleLp {
        float z = 0.f, a = 1.f;
        float process(float x) { z += a * (x - z); return z; }
        void  reset() { z = 0.f; }
    };

    enum class Dip { run, out, in };

    struct Tap {
        int32_t  off = tap_tuning::kMuted;
        int32_t  next_off = tap_tuning::kMuted;
        Dip      dip = Dip::run;
        int32_t  dip_ctr = 0;
        float    gain = 0.f, gain_target = 0.f;
        OnePoleLp lp;
    };

    void _update_filters();

    Tap   _t[tap_tuning::kTaps];
    float _sr = 48000.f;
    float _dust = 0.f;
    float _rot = -1.f;          // forces the first set_rot to compute
    int32_t _dip_len = 96;
    float _gain_coef = 1.f;
    int   _reads = 0;
};

}  // namespace spky

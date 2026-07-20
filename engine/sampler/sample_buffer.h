#pragma once
#include <cstddef>
#include <cstdint>
#include "fx/fx_util.h"   // SoftSwitch, hann_value_at

namespace spky {

// The record buffer of the M5 texture deck: a port of the original firmware's
// src/core/buffer.* (proven on hardware) with the libDaisy-adjacent pieces
// stripped and six deliberate changes, all listed in the M5a plan, Task 1.
//
// Memory is INJECTED (engine "no heap" contract): the host owns the frames,
// this class owns only the state machine. An un-init'ed buffer is inert.
//
// The read is const and holds no cursor -- up to kGrains readers call
// read_linear() concurrently within one sample, so a shared read head (which
// the original has) would be shared mutable state between them.
class SampleBuffer {
public:
    struct Frame { float l; float r; };

    SampleBuffer() = default;
    SampleBuffer(const SampleBuffer&) = delete;
    SampleBuffer& operator=(const SampleBuffer&) = delete;

    // buf may be nullptr: the buffer then reports !valid() and every
    // operation is a no-op. Hosts that cannot spare the memory rely on this.
    // sample_rate is forwarded to the overdub SoftSwitch -- without it the
    // switch's fade coefficient stays 1.0 and its Hann lookup runs off the
    // end of the table on the first overdub.
    void init(Frame* buf, size_t length, float sample_rate);

    // Linear-interpolated stereo read at a fractional frame index. Folds
    // out-of-range positions back into [0, rec_size). Silent when empty.
    void read_linear(float frame, float& out0, float& out1) const;

    // --- transport ---
    void set_recording(bool on);
    bool is_recording() const { return _state != State::idle; }
    bool is_overdubbing() const { return _cut.is_on() && is_recording(); }
    void set_feedback(float knob);    // 0..1 knob -> -60..0 dB linear factor
    void write(float in0, float in1); // call once per sample, always
    void cut();                       // lock the loop length at the fill
    void clear();
    void set_rec_size(size_t frames); // load path + tests: declare content

    // --- queries ---
    float  fill() const { return _buffer_size ? float(_size) / float(_buffer_size) : 0.f; }
    size_t rec_size() const { return _size; }
    bool   is_empty() const { return _size == 0; }
    bool   valid() const { return _buffer != nullptr && _buffer_size > 0; }
    size_t capacity() const { return _buffer_size; }
    Frame* raw() const { return _buffer; }

private:
    enum class State : uint8_t { idle, fadein, sustain, fadeout };

    SoftSwitch _cut;
    Frame*  _buffer      = nullptr;
    size_t  _buffer_size = 0;
    float   _feedback    = 0.f;   // linear factor; set by set_feedback()
    size_t  _size        = 0;     // recorded content length, in frames
    size_t  _write_head  = 0;
    size_t  _fade_ctr    = 0;
    State   _state       = State::idle;
};

}  // namespace spky

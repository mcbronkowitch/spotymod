#pragma once

namespace spky {

// Two phase-indexed slot buffers for the PITCH capture loop. A dumb fixed
// buffer with no phase or timing of its own — ModLane drives slot indexing,
// calls record() during generative playback, and capture_now() to freeze the
// rolling ring into the frozen loop. Stores the lane's pre-smooth _target
// (bipolar [-1,1]) so the full downstream chain (SMOOTH, RANGE in the lane;
// base/depth/TUNE/quantizer in Part) re-voices the loop live. ~2 KB, static.
class CaptureLoop {
public:
    static constexpr int kSlots = 192;   // divides 8/12/16/24/32 evenly

    // Init state: value 0 (bipolar center) everywhere, a single fired flag on
    // slot 0. Capturing before one full cycle has ever elapsed then yields a
    // held root-ish note, harmless.
    void reset() {
        for (int i = 0; i < kSlots; ++i) {
            _ring[i] = Slot{ 0.f, i == 0 };
            _loop[i] = Slot{ 0.f, i == 0 };
        }
        _valid = false;
    }

    // Rolling record (generative): overwrite one ring slot. Pure write — never
    // touches the RNG, so attaching a loop cannot change the bitstream.
    void record(int slot, float value, bool fired) {
        _ring[slot].value = value;
        _ring[slot].fired = fired;
    }

    // Freeze: copy the rolling ring -> the frozen loop, mark valid.
    void capture_now() {
        for (int i = 0; i < kSlots; ++i) _loop[i] = _ring[i];
        _valid = true;
    }

    float value(int slot) const { return _loop[slot].value; }
    bool  fired(int slot) const { return _loop[slot].fired; }
    bool  valid()         const { return _valid; }

private:
    struct Slot { float value = 0.f; bool fired = false; };
    Slot _ring[kSlots];
    Slot _loop[kSlots];
    bool _valid = false;
};

} // namespace spky

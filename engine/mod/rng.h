#pragma once
#include <cstdint>

namespace spky {

// Deterministic xorshift32 PRNG. No global state, no time seeding — the
// engine must be bit-reproducible across desktop and firmware (capture
// determinism, testable probability statistics).
class Rng {
public:
    void seed(uint32_t s) { _state = s ? s : 0x1u; }

    uint32_t next_u32() {
        uint32_t x = _state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        _state = x;
        return x;
    }

    float next_unipolar() { return next_u32() * (1.f / 4294967296.f); } // [0,1)
    float next_bipolar()  { return next_unipolar() * 2.f - 1.f; }        // [-1,1)

private:
    uint32_t _state = 0x1u;
};

} // namespace spky

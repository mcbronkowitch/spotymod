#pragma once
#include <cstddef>
#include <cstdint>
#include "sampler/sample_buffer.h"
#include "sampler/sampler_config.h"

namespace spky {

// Transient markers over the record buffer (spec 2026-07-22 slice-groove).
// Detection runs where writing runs -- once per written frame, a handful of
// ops -- and readers get a sorted, dense array of slice starts. No heap; no
// SDRAM search at spawn time, ever.
//
// The array is ALWAYS sorted by frame and dense (no tombstones): the live
// write path (Task 2) removes stale markers with a memmove the moment the
// write head passes them, which the refractory spacing bounds to at most one
// removal per kOnsetRefractS of audio.
class SliceMap {
public:
    static constexpr int kMax = sampler_cfg::kMaxSlices;

    void init(float sample_rate);
    void clear();

    // Offline path (load_sample): detect over [0, frames) in one call.
    // Replaces any existing markers.
    void scan(const SampleBuffer::Frame* buf, size_t frames);

    // Live path (Task 2): `frame` was just written; l/r are what actually
    // landed in the buffer (post overdub mix).
    void on_write(size_t frame, float l, float r);

    int count() const { return _n; }
    // Last marker with start <= frame. 0 when frame precedes every marker
    // (the material before the first transient belongs to slice 0), -1 only
    // when the map is empty.
    int index_at(float frame) const;
    uint32_t start(int i) const { return _m[i].frame; }
    // Frames from marker i to the next marker; the last runs to `content`.
    uint32_t length(int i, size_t content) const;
    uint8_t strength(int i) const { return _m[i].strength; }

private:
    struct Marker { uint32_t frame; uint8_t strength; };

    void _reset_detector();
    void _detect(size_t frame, float l, float r);
    void _insert(uint32_t frame, uint8_t strength);
    int  _lower_bound(uint32_t frame) const;   // first marker with frame >= arg

    Marker _m[kMax];
    int    _n = 0;
    int    _sweep = 0;                 // live path (Task 2): next stale marker
    size_t _last_frame = SIZE_MAX;     // live path: discontinuity detection
    float  _env_fast = 0.f, _env_slow = 0.f;
    float  _cf_fast = 1.f, _cf_slow = 1.f;
    int    _refract = 0;
    int    _refract_len = 1920;
    int    _preroll = 96;
    bool   _armed = true;
};

}  // namespace spky

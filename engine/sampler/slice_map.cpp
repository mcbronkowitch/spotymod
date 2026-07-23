#include "sampler/slice_map.h"
#include <cmath>
#include <cstring>

using namespace spky;
using namespace spky::sampler_cfg;

void SliceMap::init(float sample_rate) {
    // One-pole coefficient: 1 - exp(-1 / (tau * sr)), the OnePole idiom.
    _cf_fast = 1.f - std::exp(-1.f / (kOnsetFastS * sample_rate));
    _cf_slow = 1.f - std::exp(-1.f / (kOnsetSlowS * sample_rate));
    _refract_len = static_cast<int>(kOnsetRefractS * sample_rate);
    _preroll     = static_cast<int>(kOnsetPreRollS * sample_rate);
    clear();
}

void SliceMap::clear() {
    _n = 0;
    _sweep = 0;
    _last_frame = SIZE_MAX;
    _reset_detector();
}

void SliceMap::_reset_detector() {
    _env_fast = 0.f;
    _env_slow = 0.f;
    _refract  = 0;
    _armed    = true;
}

void SliceMap::scan(const SampleBuffer::Frame* buf, size_t frames) {
    clear();
    for (size_t i = 0; i < frames; ++i) _detect(i, buf[i].l, buf[i].r);
    _last_frame = SIZE_MAX;   // a following live take re-aims cleanly
}

void SliceMap::_detect(size_t frame, float l, float r) {
    const float x = 0.5f * std::fabs(l) + 0.5f * std::fabs(r);
    _env_fast += _cf_fast * (x - _env_fast);
    _env_slow += _cf_slow * (x - _env_slow);
    if (_refract > 0) { --_refract; return; }
    if (_armed) {
        if (_env_fast > kOnsetFloor && _env_fast > _env_slow * kOnsetThresh) {
            const uint32_t at = frame > static_cast<size_t>(_preroll)
                ? static_cast<uint32_t>(frame - _preroll) : 0u;
            // Strength: the fast/slow ratio, mapped so kOnsetThresh -> 0 and
            // ratio 8 -> 255. Saturating; a slow env of 0 (leading silence)
            // maps to full strength.
            float ratio = _env_slow > 1e-9f ? _env_fast / _env_slow : 8.f;
            float sn = (ratio - kOnsetThresh) / (8.f - kOnsetThresh);
            if (sn < 0.f) sn = 0.f;
            if (sn > 1.f) sn = 1.f;
            _insert(at, static_cast<uint8_t>(sn * 255.f));
            _armed = false;
            _refract = _refract_len;
        }
    } else if (_env_fast < _env_slow * kOnsetRearm) {
        _armed = true;
    }
}

int SliceMap::_lower_bound(uint32_t frame) const {
    int lo = 0, hi = _n;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (_m[mid].frame < frame) lo = mid + 1; else hi = mid;
    }
    return lo;
}

void SliceMap::_insert(uint32_t frame, uint8_t strength) {
    if (_n >= kMax) return;   // full: keep what we have (see kMaxSlices)
    const int p = _lower_bound(frame);
    if (p < _n)
        std::memmove(&_m[p + 1], &_m[p],
                     static_cast<size_t>(_n - p) * sizeof(Marker));
    _m[p].frame = frame;
    _m[p].strength = strength;
    ++_n;
}

int SliceMap::index_at(float frame) const {
    if (_n == 0) return -1;
    if (frame < 0.f) return 0;
    const int p = _lower_bound(static_cast<uint32_t>(frame) + 1u);
    return p > 0 ? p - 1 : 0;
}

uint32_t SliceMap::length(int i, size_t content) const {
    if (i < 0 || i >= _n) return 0;
    const uint32_t end = (i + 1 < _n)
        ? _m[i + 1].frame : static_cast<uint32_t>(content);
    return end > _m[i].frame ? end - _m[i].frame : 0;
}

void SliceMap::_remove(int i) {
    if (i + 1 < _n)
        std::memmove(&_m[i], &_m[i + 1],
                     static_cast<size_t>(_n - i - 1) * sizeof(Marker));
    --_n;
}

// Sequential writes sweep stale markers out from under the head: a marker at
// frame F describes content that no longer exists once the head has written
// F. Removal is a memmove, bounded to at most one per kOnsetRefractS of audio
// by the refractory spacing of the markers themselves. Fresh markers from
// THIS pass sit at frame - preroll <= head and must survive the sweep --
// _insert leaves _sweep pointing past them (see below).
void SliceMap::on_write(size_t frame, float l, float r) {
    if (_last_frame == SIZE_MAX || frame != _last_frame + 1) {
        // New take, punch-in, or ring wrap: re-aim the sweep and reset the
        // detector -- the envelope history belongs to other material.
        _sweep = _lower_bound(static_cast<uint32_t>(frame));
        _reset_detector();
    }
    _last_frame = frame;
    while (_sweep < _n && _m[_sweep].frame <= frame) _remove(_sweep);
    const int n_before = _n;
    _detect(frame, l, r);
    if (_n != n_before) {
        // _detect inserted at some p <= _sweep (its frame <= head).
        // The sweep must stay aimed at the first marker AHEAD of the head.
        _sweep = _lower_bound(static_cast<uint32_t>(frame) + 1u);
    }
}

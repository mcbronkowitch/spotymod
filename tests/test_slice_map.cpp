#include <doctest/doctest.h>
#include <cmath>
#include <vector>
#include "sampler/slice_map.h"
#include "sampler/sampler_config.h"
using namespace spky;

// Content with decaying clicks at the given frame positions: each click is a
// 1.0 impulse decaying over ~5 ms, silence elsewhere. Loud enough to clear
// kOnsetFloor, spaced by the caller.
static std::vector<SampleBuffer::Frame> clicks(size_t frames,
                                               const std::vector<size_t>& at) {
    std::vector<SampleBuffer::Frame> v(frames, SampleBuffer::Frame{0.f, 0.f});
    for (size_t c : at)
        for (size_t i = c; i < c + 240 && i < frames; ++i) {
            const float a = 1.0f * std::exp(-float(i - c) / 60.f);
            v[i].l = a; v[i].r = a;
        }
    return v;
}

TEST_CASE("slice map: offline scan marks each click, pre-rolled") {
    const std::vector<size_t> at = { 4800, 14400, 24000, 33600 };
    auto buf = clicks(48000, at);
    SliceMap m;
    m.init(48000.f);
    m.scan(buf.data(), buf.size());
    REQUIRE(m.count() == 4);
    const int pre = int(sampler_cfg::kOnsetPreRollS * 48000.f);
    for (int i = 0; i < 4; ++i) {
        // marker within [click - preroll, click + 3 ms detector lag]
        CHECK(m.start(i) >= at[i] - pre);
        CHECK(m.start(i) <= at[i] + 144);
    }
}

TEST_CASE("slice map: refractory time swallows a double hit") {
    // second click 20 ms after the first: inside kOnsetRefractS = 40 ms
    auto buf = clicks(48000, { 4800, 4800 + 960 });
    SliceMap m;
    m.init(48000.f);
    m.scan(buf.data(), buf.size());
    CHECK(m.count() == 1);
}

TEST_CASE("slice map: silence yields no markers") {
    auto buf = clicks(48000, {});
    SliceMap m;
    m.init(48000.f);
    m.scan(buf.data(), buf.size());
    CHECK(m.count() == 0);
    CHECK(m.index_at(1000.f) == -1);
}

TEST_CASE("slice map: index_at and length walk the sorted markers") {
    const std::vector<size_t> at = { 4800, 14400, 24000 };
    auto buf = clicks(30000, at);
    SliceMap m;
    m.init(48000.f);
    m.scan(buf.data(), buf.size());
    REQUIRE(m.count() == 3);
    CHECK(m.index_at(0.f) == 0);                      // before the first: slice 0
    CHECK(m.index_at(float(m.start(1))) == 1);
    CHECK(m.index_at(20000.f) == 1);
    CHECK(m.index_at(29000.f) == 2);
    CHECK(m.length(0, 30000) == m.start(1) - m.start(0));
    CHECK(m.length(2, 30000) == 30000 - m.start(2));  // last runs to content
}

TEST_CASE("slice map: clear empties, init resets the detector") {
    auto buf = clicks(48000, { 4800 });
    SliceMap m;
    m.init(48000.f);
    m.scan(buf.data(), buf.size());
    REQUIRE(m.count() == 1);
    m.clear();
    CHECK(m.count() == 0);
}

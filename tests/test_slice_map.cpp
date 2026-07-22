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

TEST_CASE("slice map: live writes detect like the offline scan") {
    const std::vector<size_t> at = { 4800, 14400, 24000 };
    auto buf = clicks(30000, at);
    SliceMap live, off;
    live.init(48000.f);
    off.init(48000.f);
    off.scan(buf.data(), buf.size());
    for (size_t i = 0; i < buf.size(); ++i) live.on_write(i, buf[i].l, buf[i].r);
    REQUIRE(live.count() == off.count());
    for (int i = 0; i < live.count(); ++i) CHECK(live.start(i) == off.start(i));
}

TEST_CASE("slice map: overdub pass replaces the markers it overwrites") {
    auto take1 = clicks(30000, { 4800, 14400, 24000 });
    SliceMap m;
    m.init(48000.f);
    for (size_t i = 0; i < take1.size(); ++i) m.on_write(i, take1[i].l, take1[i].r);
    REQUIRE(m.count() == 3);
    // Second pass overwrites [0, 20000) with ONE click at 9600. The passed
    // region's old markers (4800, 14400) must go; 24000 must survive.
    auto take2 = clicks(20000, { 9600 });
    for (size_t i = 0; i < take2.size(); ++i) m.on_write(i, take2[i].l, take2[i].r);
    REQUIRE(m.count() == 2);
    const int pre = int(sampler_cfg::kOnsetPreRollS * 48000.f);
    CHECK(m.start(0) >= 9600 - size_t(pre));
    CHECK(m.start(0) <= 9600 + 144);
    CHECK(m.start(1) >= 24000 - size_t(pre));
}

TEST_CASE("slice map: a punch-in mid-buffer only clears what it passes") {
    auto take1 = clicks(30000, { 4800, 24000 });
    SliceMap m;
    m.init(48000.f);
    for (size_t i = 0; i < take1.size(); ++i) m.on_write(i, take1[i].l, take1[i].r);
    REQUIRE(m.count() == 2);
    // Punch in at 20000, write 6000 silent frames: passes 24000's marker,
    // leaves 4800's alone. (Silence detects nothing new.)
    for (size_t i = 20000; i < 26000; ++i) m.on_write(i, 0.f, 0.f);
    REQUIRE(m.count() == 1);
    CHECK(m.start(0) <= 4800);
}

TEST_CASE("slice map: a ring wrap clears the region the head re-passes") {
    // First pass covers the whole buffer: two markers, at 4800 and 24000.
    auto take1 = clicks(30000, { 4800, 24000 });
    SliceMap m;
    m.init(48000.f);
    for (size_t i = 0; i < take1.size(); ++i) m.on_write(i, take1[i].l, take1[i].r);
    REQUIRE(m.count() == 2);
    // Write head wraps back to frame 0 and continues for 10000 frames, with a
    // fresh click at 6000 -- AFTER 4800's marker (~4704 with preroll) so the
    // sweep must actually pass over that stale marker before reaching the
    // click, rather than having _insert's post-detect re-aim skip past it for
    // free. The wrap passes over 4800's marker (must go) but never reaches
    // 24000 (must survive), same discontinuity branch as a punch-in but
    // re-aimed at frame 0 instead of mid-buffer.
    auto wrap = clicks(10000, { 6000 });
    for (size_t i = 0; i < wrap.size(); ++i) m.on_write(i, wrap[i].l, wrap[i].r);
    REQUIRE(m.count() == 2);
    const int pre = int(sampler_cfg::kOnsetPreRollS * 48000.f);
    CHECK(m.start(0) >= 6000 - size_t(pre));
    CHECK(m.start(0) <= 6000 + 144);
    CHECK(m.start(1) >= 24000 - size_t(pre));
    CHECK(m.start(1) <= 24000);
}

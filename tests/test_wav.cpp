#include <doctest/doctest.h>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include "render/wav_writer.h"
#include "render/wav_reader.h"
using namespace spky;

TEST_CASE("wav: writes a valid RIFF/WAVE header + PCM data") {
    WavWriter w(48000);
    for (int i = 0; i < 10; ++i) w.push(0.5f, -0.5f);
    const char* path = "test_out.wav";
    REQUIRE(w.write(path));

    FILE* f = std::fopen(path, "rb");
    REQUIRE(f != nullptr);
    char riff[4]; std::fread(riff, 1, 4, f);
    CHECK(riff[0] == 'R'); CHECK(riff[1] == 'I');
    CHECK(riff[2] == 'F'); CHECK(riff[3] == 'F');
    uint32_t chunk = 0; std::fread(&chunk, 4, 1, f);
    CHECK(chunk == 36u + 10u * 2u * sizeof(int16_t));
    char wave[4]; std::fread(wave, 1, 4, f);
    CHECK(wave[0] == 'W'); CHECK(wave[3] == 'E');
    std::fclose(f);
    std::remove(path);
}

TEST_CASE("wav: writer output round-trips through the reader") {
    const std::string path = "test_roundtrip.wav";
    {
        spky::WavWriter w(48000);
        for (int i = 0; i < 1000; ++i) {
            const float s = std::sin(6.2831853f * 440.f * float(i) / 48000.f);
            w.push(s, -s);
        }
        REQUIRE(w.write(path));
    }
    spky::WavData d;
    std::string err;
    REQUIRE(spky::read_wav(path, d, err));
    CHECK(err.empty());
    CHECK(d.sample_rate == 48000);
    REQUIRE(d.l.size() == 1000);
    REQUIRE(d.r.size() == 1000);
    for (int i = 0; i < 1000; ++i) {
        const float s = std::sin(6.2831853f * 440.f * float(i) / 48000.f);
        // 16-bit quantization is the only loss.
        CHECK(d.l[i] == doctest::Approx(s).epsilon(0.001));
        CHECK(d.r[i] == doctest::Approx(-s).epsilon(0.001));
    }
    std::remove(path.c_str());
}

TEST_CASE("wav: a missing or malformed file is an error, not an empty load") {
    spky::WavData d;
    std::string err;
    CHECK_FALSE(spky::read_wav("definitely_not_here.wav", d, err));
    CHECK_FALSE(err.empty());

    const std::string path = "test_garbage.wav";
    { FILE* f = std::fopen(path.c_str(), "wb"); std::fputs("NOTARIFF", f); std::fclose(f); }
    err.clear();
    CHECK_FALSE(spky::read_wav(path, d, err));
    CHECK_FALSE(err.empty());
    std::remove(path.c_str());
}

// The writer emits one fixed 44-byte header with no extra chunks, so the
// round-trip case above cannot exercise chunk-walking: a reader that hard-
// codes "data starts at byte 44" would still pass it. Real files (and
// several DAWs' exports) carry a LIST chunk between fmt and data, so build
// one by hand here to prove the reader walks the chunk list instead of
// assuming a fixed offset.
TEST_CASE("wav: chunks between fmt and data (e.g. LIST) are skipped, not assumed absent") {
    const std::string path = "test_list_chunk.wav";
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        REQUIRE(f != nullptr);
        auto wu32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto wu16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
        auto wtag = [&](const char* s) { std::fwrite(s, 1, 4, f); };

        const int16_t pcm[4] = { 1000, -1000, 2000, -2000 };  // 2 stereo frames
        const uint32_t data_bytes = sizeof(pcm);
        const uint32_t fmt_bytes = 16;
        const uint32_t list_payload_bytes = 4;                // "JUNK", even-sized
        const uint32_t riff_size = 4 /* WAVE */
            + 8 + fmt_bytes
            + 8 + list_payload_bytes
            + 8 + data_bytes;

        wtag("RIFF"); wu32(riff_size); wtag("WAVE");
        wtag("fmt "); wu32(fmt_bytes);
        wu16(1); wu16(2); wu32(48000); wu32(48000u * 2u * 2u); wu16(4); wu16(16);
        wtag("LIST"); wu32(list_payload_bytes); std::fwrite("JUNK", 1, 4, f);
        wtag("data"); wu32(data_bytes);
        std::fwrite(pcm, 1, sizeof(pcm), f);
        std::fclose(f);
    }
    spky::WavData d;
    std::string err;
    REQUIRE(spky::read_wav(path, d, err));
    CHECK(err.empty());
    REQUIRE(d.l.size() == 2);
    REQUIRE(d.r.size() == 2);
    CHECK(d.l[0] == doctest::Approx(1000.f / 32768.f));
    CHECK(d.r[0] == doctest::Approx(-1000.f / 32768.f));
    CHECK(d.l[1] == doctest::Approx(2000.f / 32768.f));
    CHECK(d.r[1] == doctest::Approx(-2000.f / 32768.f));
    std::remove(path.c_str());
}

// The round-trip case above is stereo-only (the writer never emits mono), so
// it cannot exercise "mono normals to both channels" either. Build a mono
// 16-bit PCM file by hand and confirm l/r come back identical.
TEST_CASE("wav: a mono file normals to both channels") {
    const std::string path = "test_mono.wav";
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        REQUIRE(f != nullptr);
        auto wu32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto wu16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
        auto wtag = [&](const char* s) { std::fwrite(s, 1, 4, f); };

        const int16_t pcm[3] = { 1000, -2000, 3000 };  // 3 mono frames
        const uint32_t data_bytes = sizeof(pcm);
        const uint32_t fmt_bytes = 16;
        const uint32_t riff_size = 4 /* WAVE */ + 8 + fmt_bytes + 8 + data_bytes;

        wtag("RIFF"); wu32(riff_size); wtag("WAVE");
        wtag("fmt "); wu32(fmt_bytes);
        wu16(1); wu16(1); wu32(48000); wu32(48000u * 1u * 2u); wu16(2); wu16(16);
        wtag("data"); wu32(data_bytes);
        std::fwrite(pcm, 1, sizeof(pcm), f);
        std::fclose(f);
    }
    spky::WavData d;
    std::string err;
    REQUIRE(spky::read_wav(path, d, err));
    CHECK(err.empty());
    REQUIRE(d.l.size() == 3);
    REQUIRE(d.r.size() == 3);
    for (int i = 0; i < 3; ++i) CHECK(d.l[i] == d.r[i]);
    CHECK(d.l[0] == doctest::Approx(1000.f / 32768.f));
    CHECK(d.l[1] == doctest::Approx(-2000.f / 32768.f));
    CHECK(d.l[2] == doctest::Approx(3000.f / 32768.f));
    std::remove(path.c_str());
}

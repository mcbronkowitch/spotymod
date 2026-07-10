#include <doctest/doctest.h>
#include <cstdio>
#include <cstdint>
#include "render/wav_writer.h"
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

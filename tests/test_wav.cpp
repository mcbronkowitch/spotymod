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

// 24-bit PCM is the one integer branch that needs a right-shift to sign
// extend (int32_t s = bytes shifted up by 8, then >>8 arithmetic shift).
// Hand-verified 24-bit two's-complement encodings used below:
//   0x123456   ->  1193046  (top byte 0x12, MSB clear: positive)
//   0xFFFF9C   ->     -100  (2^24 - 100 = 16777116 = 0xFFFF9C)
//   0x800000   -> -8388608  (minimum 24-bit value, top-of-range sign extend)
//   0x7FFFFF   ->  8388607  (maximum 24-bit value)
TEST_CASE("wav: 24-bit PCM decodes with correct sign extension") {
    const std::string path = "test_pcm24.wav";
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        REQUIRE(f != nullptr);
        auto wu32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto wu16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
        auto wtag = [&](const char* s) { std::fwrite(s, 1, 4, f); };
        auto w24 = [&](uint8_t b0, uint8_t b1, uint8_t b2) {
            uint8_t b[3] = { b0, b1, b2 };
            std::fwrite(b, 1, 3, f);
        };

        const uint32_t data_bytes = 2 /*frames*/ * 2 /*channels*/ * 3 /*bytes*/;  // = 12
        const uint32_t fmt_bytes = 16;
        const uint32_t riff_size = 4 + 8 + fmt_bytes + 8 + data_bytes;

        wtag("RIFF"); wu32(riff_size); wtag("WAVE");
        wtag("fmt "); wu32(fmt_bytes);
        wu16(1); wu16(2); wu32(48000); wu32(48000u * 2u * 3u); wu16(6); wu16(24);
        wtag("data"); wu32(data_bytes);
        // frame 0: L = 0x123456 (1193046), R = 0xFFFF9C (-100)
        w24(0x56, 0x34, 0x12);
        w24(0x9C, 0xFF, 0xFF);
        // frame 1: L = 0x800000 (-8388608, min), R = 0x7FFFFF (8388607, max)
        w24(0x00, 0x00, 0x80);
        w24(0xFF, 0xFF, 0x7F);
        std::fclose(f);
    }
    spky::WavData d;
    std::string err;
    REQUIRE(spky::read_wav(path, d, err));
    CHECK(err.empty());
    REQUIRE(d.l.size() == 2);
    REQUIRE(d.r.size() == 2);
    CHECK(d.l[0] == doctest::Approx(1193046.f / 8388608.f));
    CHECK(d.r[0] == doctest::Approx(-100.f / 8388608.f));
    CHECK(d.l[1] == doctest::Approx(-8388608.f / 8388608.f));   // == -1.0
    CHECK(d.r[1] == doctest::Approx(8388607.f / 8388608.f));
    std::remove(path.c_str());
}

// 32-bit PCM, including a negative value. 2^30 / 2^31 = 0.5 exactly, so
// both channels land on values that are exact in binary32 (no epsilon
// slop needed to hide a wrong shift/mask).
TEST_CASE("wav: 32-bit PCM decodes a known positive and negative value") {
    const std::string path = "test_pcm32.wav";
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        REQUIRE(f != nullptr);
        auto wu32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto wu16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
        auto wtag = [&](const char* s) { std::fwrite(s, 1, 4, f); };

        const int32_t pcm[2] = { 1073741824, -1073741824 };  // +2^30, -2^30
        const uint32_t data_bytes = sizeof(pcm);
        const uint32_t fmt_bytes = 16;
        const uint32_t riff_size = 4 + 8 + fmt_bytes + 8 + data_bytes;

        wtag("RIFF"); wu32(riff_size); wtag("WAVE");
        wtag("fmt "); wu32(fmt_bytes);
        wu16(1); wu16(2); wu32(48000); wu32(48000u * 2u * 4u); wu16(8); wu16(32);
        wtag("data"); wu32(data_bytes);
        std::fwrite(pcm, 1, sizeof(pcm), f);
        std::fclose(f);
    }
    spky::WavData d;
    std::string err;
    REQUIRE(spky::read_wav(path, d, err));
    CHECK(err.empty());
    REQUIRE(d.l.size() == 1);
    REQUIRE(d.r.size() == 1);
    CHECK(d.l[0] == doctest::Approx(0.5f));
    CHECK(d.r[0] == doctest::Approx(-0.5f));
    std::remove(path.c_str());
}

// 32-bit IEEE float, exercising Fix 2 (byte-explicit little-endian
// reassembly rather than a raw memcpy of file bytes into a float).
// Bit patterns hand-derived from IEEE-754 binary32:
//   0.25f  = sign 0, exp 125 (0x7D), mantissa 0        -> 0x3E800000
//  -0.75f  = sign 1, exp 126 (0x7E), mantissa 0x400000  -> 0xBF400000
TEST_CASE("wav: 32-bit float decodes a known positive and negative value") {
    const std::string path = "test_float32.wav";
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        REQUIRE(f != nullptr);
        auto wu32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto wu16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
        auto wtag = [&](const char* s) { std::fwrite(s, 1, 4, f); };
        auto wraw4 = [&](uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
            uint8_t b[4] = { b0, b1, b2, b3 };
            std::fwrite(b, 1, 4, f);
        };

        const uint32_t data_bytes = 8;  // 1 stereo frame * 2 * 4B
        const uint32_t fmt_bytes = 16;
        const uint32_t riff_size = 4 + 8 + fmt_bytes + 8 + data_bytes;

        wtag("RIFF"); wu32(riff_size); wtag("WAVE");
        wtag("fmt "); wu32(fmt_bytes);
        wu16(3); wu16(2); wu32(48000); wu32(48000u * 2u * 4u); wu16(8); wu16(32);
        wtag("data"); wu32(data_bytes);
        wraw4(0x00, 0x00, 0x80, 0x3E);  // L = 0.25f, LE bytes of 0x3E800000
        wraw4(0x00, 0x00, 0x40, 0xBF);  // R = -0.75f, LE bytes of 0xBF400000
        std::fclose(f);
    }
    spky::WavData d;
    std::string err;
    REQUIRE(spky::read_wav(path, d, err));
    CHECK(err.empty());
    REQUIRE(d.l.size() == 1);
    REQUIRE(d.r.size() == 1);
    CHECK(d.l[0] == 0.25f);
    CHECK(d.r[0] == -0.75f);
    std::remove(path.c_str());
}

TEST_CASE("wav: a data chunk before fmt is an error") {
    const std::string path = "test_data_before_fmt.wav";
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        REQUIRE(f != nullptr);
        auto wu32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto wu16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
        auto wtag = [&](const char* s) { std::fwrite(s, 1, 4, f); };

        const int16_t pcm[2] = { 111, -222 };
        const uint32_t data_bytes = sizeof(pcm);
        const uint32_t fmt_bytes = 16;
        const uint32_t riff_size = 4 + 8 + data_bytes + 8 + fmt_bytes;

        wtag("RIFF"); wu32(riff_size); wtag("WAVE");
        wtag("data"); wu32(data_bytes);
        std::fwrite(pcm, 1, sizeof(pcm), f);
        wtag("fmt "); wu32(fmt_bytes);
        wu16(1); wu16(2); wu32(48000); wu32(48000u * 2u * 2u); wu16(4); wu16(16);
        std::fclose(f);
    }
    spky::WavData d;
    std::string err;
    CHECK_FALSE(spky::read_wav(path, d, err));
    CHECK_FALSE(err.empty());
    std::remove(path.c_str());
}

// The data chunk header claims far more bytes than actually follow in the
// file. A reader that trusts the declared size for anything other than the
// fread-and-check (e.g. that resizes and returns partially-zeroed buffers
// as success) would fail this silently; read_wav must report an error.
TEST_CASE("wav: a truncated data chunk is an error, not a partial load") {
    const std::string path = "test_truncated_data.wav";
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        REQUIRE(f != nullptr);
        auto wu32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto wu16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
        auto wtag = [&](const char* s) { std::fwrite(s, 1, 4, f); };

        const uint32_t claimed_data_bytes = 1000000;  // far more than we write
        const uint32_t fmt_bytes = 16;
        const uint32_t riff_size = 4 + 8 + fmt_bytes + 8 + claimed_data_bytes;

        wtag("RIFF"); wu32(riff_size); wtag("WAVE");
        wtag("fmt "); wu32(fmt_bytes);
        wu16(1); wu16(2); wu32(48000); wu32(48000u * 2u * 2u); wu16(4); wu16(16);
        wtag("data"); wu32(claimed_data_bytes);
        const int16_t pcm[2] = { 1, 2 };  // only 4 actual bytes follow
        std::fwrite(pcm, 1, sizeof(pcm), f);
        std::fclose(f);
    }
    spky::WavData d;
    std::string err;
    CHECK_FALSE(spky::read_wav(path, d, err));
    CHECK_FALSE(err.empty());
    std::remove(path.c_str());
}

TEST_CASE("wav: 3+ channels is rejected with an error") {
    const std::string path = "test_3ch.wav";
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        REQUIRE(f != nullptr);
        auto wu32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto wu16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
        auto wtag = [&](const char* s) { std::fwrite(s, 1, 4, f); };

        const int16_t pcm[3] = { 1, 2, 3 };  // 1 frame, 3 channels
        const uint32_t data_bytes = sizeof(pcm);
        const uint32_t fmt_bytes = 16;
        const uint32_t riff_size = 4 + 8 + fmt_bytes + 8 + data_bytes;

        wtag("RIFF"); wu32(riff_size); wtag("WAVE");
        wtag("fmt "); wu32(fmt_bytes);
        wu16(1); wu16(3); wu32(48000); wu32(48000u * 3u * 2u); wu16(6); wu16(16);
        wtag("data"); wu32(data_bytes);
        std::fwrite(pcm, 1, sizeof(pcm), f);
        std::fclose(f);
    }
    spky::WavData d;
    std::string err;
    CHECK_FALSE(spky::read_wav(path, d, err));
    CHECK_FALSE(err.empty());
    std::remove(path.c_str());
}

// --- WAVE_FORMAT_EXTENSIBLE (Fix 1) -----------------------------------
//
// fmt chunk is 40 bytes: the 16 base fields, then cbSize(2)=22,
// validBitsPerSample(2), channelMask(4), and a 16-byte SubFormat GUID
// whose first uint16 is the real format tag (1 = PCM, 3 = float) and
// whose trailing 14 bytes must equal the standard KSDATAFORMAT suffix.

static void write_extensible_fmt_chunk(FILE* f, uint16_t sub_fmt, uint16_t channels,
                                        uint32_t rate, uint16_t bits,
                                        const uint8_t* guid_suffix14) {
    auto wu32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto wu16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    const uint16_t block = uint16_t(channels * (bits / 8));
    wu16(0xFFFE); wu16(channels); wu32(rate); wu32(rate * block); wu16(block); wu16(bits);
    wu16(22);              // cbSize
    wu16(bits);            // validBitsPerSample
    wu32(3);                // channelMask (front L+R), unused by the reader
    wu16(sub_fmt);          // SubFormat GUID leading uint16 = real format tag
    std::fwrite(guid_suffix14, 1, 14, f);
}

static const uint8_t kStdGuidSuffix[14] = {
    0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00,
    0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71
};

TEST_CASE("wav: WAVE_FORMAT_EXTENSIBLE with IEEE float SubFormat decodes correctly") {
    const std::string path = "test_ext_float.wav";
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        REQUIRE(f != nullptr);
        auto wu32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto wtag = [&](const char* s) { std::fwrite(s, 1, 4, f); };
        auto wraw4 = [&](uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
            uint8_t b[4] = { b0, b1, b2, b3 };
            std::fwrite(b, 1, 4, f);
        };

        const uint32_t fmt_bytes = 40;
        const uint32_t data_bytes = 8;  // 1 stereo frame * 2 * 4B
        const uint32_t riff_size = 4 + 8 + fmt_bytes + 8 + data_bytes;

        wtag("RIFF"); wu32(riff_size); wtag("WAVE");
        wtag("fmt "); wu32(fmt_bytes);
        write_extensible_fmt_chunk(f, /*sub_fmt=*/3, /*channels=*/2, 48000, 32, kStdGuidSuffix);
        wtag("data"); wu32(data_bytes);
        wraw4(0x00, 0x00, 0x00, 0x3F);  // L = 0.5f  (0x3F000000)
        wraw4(0x00, 0x00, 0x80, 0xBE);  // R = -0.25f (0xBE800000)
        std::fclose(f);
    }
    spky::WavData d;
    std::string err;
    REQUIRE(spky::read_wav(path, d, err));
    CHECK(err.empty());
    REQUIRE(d.l.size() == 1);
    CHECK(d.l[0] == 0.5f);
    CHECK(d.r[0] == -0.25f);
    std::remove(path.c_str());
}

TEST_CASE("wav: WAVE_FORMAT_EXTENSIBLE with PCM SubFormat decodes correctly") {
    const std::string path = "test_ext_pcm.wav";
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        REQUIRE(f != nullptr);
        auto wu32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto wtag = [&](const char* s) { std::fwrite(s, 1, 4, f); };

        const uint32_t fmt_bytes = 40;
        const int16_t pcm[2] = { 12345, -12345 };  // 1 stereo frame, 16-bit
        const uint32_t data_bytes = sizeof(pcm);
        const uint32_t riff_size = 4 + 8 + fmt_bytes + 8 + data_bytes;

        wtag("RIFF"); wu32(riff_size); wtag("WAVE");
        wtag("fmt "); wu32(fmt_bytes);
        write_extensible_fmt_chunk(f, /*sub_fmt=*/1, /*channels=*/2, 48000, 16, kStdGuidSuffix);
        wtag("data"); wu32(data_bytes);
        std::fwrite(pcm, 1, sizeof(pcm), f);
        std::fclose(f);
    }
    spky::WavData d;
    std::string err;
    REQUIRE(spky::read_wav(path, d, err));
    CHECK(err.empty());
    REQUIRE(d.l.size() == 1);
    CHECK(d.l[0] == doctest::Approx(12345.f / 32768.f));
    CHECK(d.r[0] == doctest::Approx(-12345.f / 32768.f));
    std::remove(path.c_str());
}

// A trailing data chunk with real, decodable PCM samples is included
// deliberately: a reader that fails to validate the GUID (and just falls
// through, e.g. treating fmt as 0xFFFE and decoding via the >16-bit-int
// path) would otherwise still return false from hitting EOF with no data
// chunk consumed correctly -- passing this test for the wrong reason. With
// a real data chunk present, a permissive implementation succeeds instead
// of erroring, so the assertion is actually observing GUID validation.
TEST_CASE("wav: WAVE_FORMAT_EXTENSIBLE with an unrecognized SubFormat GUID is an error") {
    const std::string path = "test_ext_bad_guid.wav";
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        REQUIRE(f != nullptr);
        auto wu32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto wtag = [&](const char* s) { std::fwrite(s, 1, 4, f); };

        uint8_t bogus_suffix[14];
        for (int i = 0; i < 14; ++i) bogus_suffix[i] = 0xAB;

        const uint32_t fmt_bytes = 40;
        const int16_t pcm[2] = { 111, -222 };  // 1 stereo frame, decodable if not rejected
        const uint32_t data_bytes = sizeof(pcm);
        const uint32_t riff_size = 4 + 8 + fmt_bytes + 8 + data_bytes;

        wtag("RIFF"); wu32(riff_size); wtag("WAVE");
        wtag("fmt "); wu32(fmt_bytes);
        write_extensible_fmt_chunk(f, /*sub_fmt=*/1, /*channels=*/2, 48000, 16, bogus_suffix);
        wtag("data"); wu32(data_bytes);
        std::fwrite(pcm, 1, sizeof(pcm), f);
        std::fclose(f);
    }
    spky::WavData d;
    std::string err;
    CHECK_FALSE(spky::read_wav(path, d, err));
    CHECK_FALSE(err.empty());
    std::remove(path.c_str());
}

// See the comment on the bad-GUID test above: a trailing decodable data
// chunk is required so a reader that fails to validate the SubFormat tag
// doesn't pass this test merely by running out of file.
TEST_CASE("wav: WAVE_FORMAT_EXTENSIBLE with an unsupported SubFormat tag is an error") {
    const std::string path = "test_ext_bad_tag.wav";
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        REQUIRE(f != nullptr);
        auto wu32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto wtag = [&](const char* s) { std::fwrite(s, 1, 4, f); };

        const uint32_t fmt_bytes = 40;
        const int16_t pcm[2] = { 111, -222 };  // 1 stereo frame, decodable if not rejected
        const uint32_t data_bytes = sizeof(pcm);
        const uint32_t riff_size = 4 + 8 + fmt_bytes + 8 + data_bytes;

        wtag("RIFF"); wu32(riff_size); wtag("WAVE");
        wtag("fmt "); wu32(fmt_bytes);
        write_extensible_fmt_chunk(f, /*sub_fmt=*/6 /*A-law, unsupported*/, 2, 48000, 16,
                                    kStdGuidSuffix);
        wtag("data"); wu32(data_bytes);
        std::fwrite(pcm, 1, sizeof(pcm), f);
        std::fclose(f);
    }
    spky::WavData d;
    std::string err;
    CHECK_FALSE(spky::read_wav(path, d, err));
    CHECK_FALSE(err.empty());
    std::remove(path.c_str());
}

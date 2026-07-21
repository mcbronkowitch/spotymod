#include <doctest/doctest.h>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include "shared/wav_writer.h"
#include "shared/wav_reader.h"
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
// --- Fix 1/2/4 hardening tests -----------------------------------------
//
// These exercise the final-review fixes to read_wav: a declared size must
// never be trusted for an allocation or a seek before it's checked against
// the bytes actually left in the file.

// A truncated file that claims a data chunk near the top of the uint32_t
// range (~4 GB). The pre-fix reader computed frames from csize and called
// out.l.resize(frames)/out.r.resize(frames)/std::vector<uint8_t> raw(csize)
// before ever checking the file was that big, which would attempt a ~4-8 GB
// allocation and either throw std::bad_alloc (uncaught at main.cpp's call
// site -> std::terminate) or, if it somehow succeeded, silently eat huge
// amounts of memory. The fix checks the declared size against the real
// remaining byte count first and rejects by name -- this test proves that
// happens promptly (no crash, no hang, no multi-GB allocation) rather than
// merely "the process didn't visibly explode".
TEST_CASE("wav: a data chunk declaring ~4GB is rejected without a huge allocation or crash") {
    const std::string path = "test_huge_data_claim.wav";
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        REQUIRE(f != nullptr);
        auto wu32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto wu16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
        auto wtag = [&](const char* s) { std::fwrite(s, 1, 4, f); };

        const uint32_t claimed_data_bytes = 0xFFFFFFFFu;  // ~4 GB, way past EOF
        const uint32_t fmt_bytes = 16;
        // riff_size deliberately not checked by the reader against claimed
        // size; what matters is the actual bytes on disk after this header.
        const uint32_t riff_size = 4 + 8 + fmt_bytes + 8;

        wtag("RIFF"); wu32(riff_size); wtag("WAVE");
        wtag("fmt "); wu32(fmt_bytes);
        wu16(1); wu16(2); wu32(48000); wu32(48000u * 2u * 2u); wu16(4); wu16(16);
        wtag("data"); wu32(claimed_data_bytes);
        const int16_t pcm[2] = { 7, -7 };  // a handful of real bytes, nowhere near 4GB
        std::fwrite(pcm, 1, sizeof(pcm), f);
        std::fclose(f);
    }
    spky::WavData d;
    std::string err;
    CHECK_FALSE(spky::read_wav(path, d, err));
    CHECK_FALSE(err.empty());
    // Must fail via Fix 1's own remaining-bytes guard, not merely because
    // fread() later notices too few bytes came back -- that fallback still
    // fires *after* out.l/out.r/raw have already been sized to ~4GB worth
    // of frames, which is exactly the allocation this fix exists to avoid.
    // A vague "err contains 'data'" check passes for either reason, so
    // require the specific wording of the Fix 1 guard.
    CHECK(err.find("remain in") != std::string::npos);
    std::remove(path.c_str());
}

// A generic (non-fmt, non-data) chunk whose declared size has the high bit
// set. csize is uint32_t and `long` is 32-bit on Windows, so the old skip
// computation `long(csize + (csize & 1))` silently became negative for any
// csize >= 0x80000000: fseek would then walk backwards and the read loop
// could re-parse the same bytes as a "new" chunk forever. The fix computes
// the skip in a 64-bit type and validates it against real remaining bytes
// before seeking at all, so this must return false promptly instead of
// looping. (If the old bug regresses, this test hangs rather than fails
// cleanly -- that itself is the signal.)
TEST_CASE("wav: an unknown chunk with the high bit set in its size is rejected, not looped") {
    const std::string path = "test_high_bit_chunk.wav";
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        REQUIRE(f != nullptr);
        auto wu32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto wu16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
        auto wtag = [&](const char* s) { std::fwrite(s, 1, 4, f); };

        const uint32_t fmt_bytes = 16;
        const uint32_t junk_claimed = 0x80000000u;  // high bit set
        const uint32_t riff_size = 4 + 8 + fmt_bytes + 8;  // junk payload not included

        wtag("RIFF"); wu32(riff_size); wtag("WAVE");
        wtag("fmt "); wu32(fmt_bytes);
        wu16(1); wu16(2); wu32(48000); wu32(48000u * 2u * 2u); wu16(4); wu16(16);
        wtag("JUNK"); wu32(junk_claimed);
        // No payload follows -- file ends right after the chunk header, so
        // "junk_claimed" bytes are nowhere near actually present.
        std::fclose(f);
    }
    spky::WavData d;
    std::string err;
    CHECK_FALSE(spky::read_wav(path, d, err));
    CHECK_FALSE(err.empty());
    // Without the fix, `long(csize + (csize & 1))` truncates to a negative
    // seek offset; fseek() then fails to seek before the start of the file,
    // leaves the position unchanged, and the next fread() simply hits EOF
    // -- so an unguarded reader still returns false, just via the generic
    // "no data chunk" message rather than by ever recognizing the
    // oversized chunk. Require the specific wording Fix 2's own guard
    // produces so this test can't pass for that wrong reason.
    CHECK(err.find("runs past end of file") != std::string::npos);
    std::remove(path.c_str());
}

// fmt chunks shorter than the 16 mandatory base fields desync every
// subsequent read (the reader would try to read fmt/channels/rate/etc past
// where the chunk actually ends). Must be rejected immediately by the
// declared size, not merely because too few bytes happen to follow.
//
// To make this decisive rather than incidental: the file below actually
// contains a full, valid-looking 16-byte fmt payload plus a real, decodable
// data chunk right after it -- so a reader that skips the size check would
// happily parse the fmt fields, compute a non-positive/skipped extension,
// and go on to successfully decode the data chunk (returning true). Only
// checking the declared size (10 < 16) catches the lie.
TEST_CASE("wav: a fmt chunk shorter than 16 bytes is rejected") {
    const std::string path = "test_short_fmt.wav";
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        REQUIRE(f != nullptr);
        auto wu32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto wu16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
        auto wtag = [&](const char* s) { std::fwrite(s, 1, 4, f); };

        const uint32_t declared_fmt_bytes = 10;   // lies: < 16, malformed
        const int16_t pcm[2] = { 111, -222 };      // 1 stereo frame, decodable if not rejected
        const uint32_t data_bytes = sizeof(pcm);
        const uint32_t riff_size = 4 + 8 + 16 + 8 + data_bytes;

        wtag("RIFF"); wu32(riff_size); wtag("WAVE");
        wtag("fmt "); wu32(declared_fmt_bytes);
        // A full, otherwise-valid 16-byte base fmt payload follows anyway.
        wu16(1); wu16(2); wu32(48000); wu32(48000u * 2u * 2u); wu16(4); wu16(16);
        wtag("data"); wu32(data_bytes);
        std::fwrite(pcm, 1, sizeof(pcm), f);
        std::fclose(f);
    }
    spky::WavData d;
    std::string err;
    CHECK_FALSE(spky::read_wav(path, d, err));
    CHECK_FALSE(err.empty());
    CHECK(err.find("too short") != std::string::npos);
    std::remove(path.c_str());
}

// An unsupported bit depth (12) with an otherwise well-formed, fully
// present data chunk -- the data bytes match the declared size exactly, so
// a pass here can only be explained by the bit-depth check itself, not by
// truncation or "no data chunk" falling through.
TEST_CASE("wav: an unsupported bit depth (12) is rejected by name") {
    const std::string path = "test_bits12.wav";
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        REQUIRE(f != nullptr);
        auto wu32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto wu16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
        auto wtag = [&](const char* s) { std::fwrite(s, 1, 4, f); };

        const uint8_t pcm[2] = { 0x12, 0x03 };  // 2 real bytes; matches data_bytes exactly
        const uint32_t data_bytes = sizeof(pcm);
        const uint32_t fmt_bytes = 16;
        const uint32_t riff_size = 4 + 8 + fmt_bytes + 8 + data_bytes;

        wtag("RIFF"); wu32(riff_size); wtag("WAVE");
        wtag("fmt "); wu32(fmt_bytes);
        wu16(1); wu16(1); wu32(48000); wu32(48000u * 1u * 2u); wu16(2); wu16(12);  // 12-bit
        wtag("data"); wu32(data_bytes);
        std::fwrite(pcm, 1, sizeof(pcm), f);
        std::fclose(f);
    }
    spky::WavData d;
    std::string err;
    CHECK_FALSE(spky::read_wav(path, d, err));
    CHECK_FALSE(err.empty());
    CHECK(err.find("12") != std::string::npos);
    std::remove(path.c_str());
}

// The existing 24-bit test already covers 0x800000 (min) landing at exactly
// -1.0f, but the UB was in how the value is *constructed* (signed left
// shift of the top byte into the sign bit), not just in the final shift --
// so cover a second, distinct negative encoding here to make sure the Fix 4
// rewrite (build in uint32_t, convert to signed explicitly) didn't just
// happen to still work for the one value already under test.
// Hand-verified: 0xC00000 as 24-bit two's complement = 12582912 - 2^24
// = -4194304 = -2^22. Dividing by 2^23 (8388608) gives exactly -0.5f, a
// power-of-two fraction that's bit-exact in binary32 -- no epsilon needed.
TEST_CASE("wav: 24-bit PCM decodes a second negative value to an exact float") {
    const std::string path = "test_pcm24_neg2.wav";
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

        const uint32_t data_bytes = 1 /*frame*/ * 1 /*channel*/ * 3 /*bytes*/;  // = 3
        const uint32_t fmt_bytes = 16;
        const uint32_t riff_size = 4 + 8 + fmt_bytes + 8 + data_bytes;

        wtag("RIFF"); wu32(riff_size); wtag("WAVE");
        wtag("fmt "); wu32(fmt_bytes);
        wu16(1); wu16(1); wu32(48000); wu32(48000u * 1u * 3u); wu16(3); wu16(24);  // mono
        wtag("data"); wu32(data_bytes);
        w24(0x00, 0x00, 0xC0);  // 0xC00000 = -4194304
        std::fclose(f);
    }
    spky::WavData d;
    std::string err;
    REQUIRE(spky::read_wav(path, d, err));
    CHECK(err.empty());
    REQUIRE(d.l.size() == 1);
    REQUIRE(d.r.size() == 1);
    CHECK(d.l[0] == -0.5f);   // exact: -4194304 / 8388608
    CHECK(d.r[0] == -0.5f);   // mono normals to both channels
    std::remove(path.c_str());
}

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

#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Counterpart to wav_writer.h. Unlike the writer -- which emits one fixed
// header shape -- this walks the chunk list, because real files put LIST and
// fact chunks before data, and accepts 16/24/32-bit PCM and 32-bit float.
namespace spky {

struct WavData {
    int sample_rate = 48000;
    std::vector<float> l, r;
};

inline bool read_wav(const std::string& path, WavData& out, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open " + path; return false; }

    struct Closer { FILE* f; ~Closer() { std::fclose(f); } } closer{ f };

    auto rd_u32 = [&](uint32_t& v) {
        uint8_t b[4];
        if (std::fread(b, 1, 4, f) != 4) return false;
        v = uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
        return true;
    };
    auto rd_u16 = [&](uint16_t& v) {
        uint8_t b[2];
        if (std::fread(b, 1, 2, f) != 2) return false;
        v = uint16_t(uint16_t(b[0]) | (uint16_t(b[1]) << 8));
        return true;
    };

    char tag[4];
    uint32_t riff_size = 0;
    if (std::fread(tag, 1, 4, f) != 4 || std::memcmp(tag, "RIFF", 4) != 0) {
        err = "not a RIFF file: " + path; return false;
    }
    if (!rd_u32(riff_size)) { err = "truncated header"; return false; }
    if (std::fread(tag, 1, 4, f) != 4 || std::memcmp(tag, "WAVE", 4) != 0) {
        err = "not a WAVE file: " + path; return false;
    }

    uint16_t fmt = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    bool have_fmt = false;

    while (true) {
        char cid[4];
        uint32_t csize = 0;
        if (std::fread(cid, 1, 4, f) != 4) break;      // clean EOF
        if (!rd_u32(csize)) break;

        if (std::memcmp(cid, "fmt ", 4) == 0) {
            uint16_t block = 0;
            uint32_t byterate = 0;
            if (!rd_u16(fmt) || !rd_u16(channels) || !rd_u32(rate) ||
                !rd_u32(byterate) || !rd_u16(block) || !rd_u16(bits)) {
                err = "truncated fmt chunk"; return false;
            }
            have_fmt = true;
            // Skip any fmt extension (WAVE_FORMAT_EXTENSIBLE carries one).
            if (csize > 16) std::fseek(f, long(csize - 16), SEEK_CUR);
        } else if (std::memcmp(cid, "data", 4) == 0) {
            if (!have_fmt) { err = "data chunk before fmt chunk"; return false; }
            if (channels < 1 || channels > 2) {
                err = "unsupported channel count"; return false;
            }
            const int bytes = bits / 8;
            if (bytes < 2 || bytes > 4) { err = "unsupported bit depth"; return false; }

            const size_t frames = size_t(csize) / (size_t(bytes) * channels);
            out.sample_rate = int(rate);
            out.l.resize(frames);
            out.r.resize(frames);

            std::vector<uint8_t> raw(csize);
            if (std::fread(raw.data(), 1, csize, f) != csize) {
                err = "truncated data chunk"; return false;
            }
            const uint8_t* p = raw.data();
            for (size_t i = 0; i < frames; ++i) {
                for (int c = 0; c < channels; ++c) {
                    float v = 0.f;
                    if (fmt == 3 && bytes == 4) {           // IEEE float
                        float tmp;
                        std::memcpy(&tmp, p, 4);
                        v = tmp;
                    } else if (bytes == 2) {
                        int16_t s = int16_t(uint16_t(p[0]) | (uint16_t(p[1]) << 8));
                        v = float(s) / 32768.f;
                    } else if (bytes == 3) {
                        int32_t s = (int32_t(p[0]) << 8) | (int32_t(p[1]) << 16) |
                                    (int32_t(p[2]) << 24);
                        v = float(s >> 8) / 8388608.f;
                    } else {                                 // 32-bit PCM
                        int32_t s = int32_t(uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                                            (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24));
                        v = float(s) / 2147483648.f;
                    }
                    if (c == 0) out.l[i] = v;
                    else        out.r[i] = v;
                    p += bytes;
                }
                if (channels == 1) out.r[i] = out.l[i];      // mono normals
            }
            return true;
        } else {
            std::fseek(f, long(csize + (csize & 1)), SEEK_CUR);   // chunks pad to even
        }
    }
    err = "no data chunk in " + path;
    return false;
}

}  // namespace spky

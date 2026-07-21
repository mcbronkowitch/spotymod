#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Counterpart to wav_writer.h. Unlike the writer -- which emits one fixed
// header shape -- this walks the chunk list, because real files put LIST and
// fact chunks before data, and accepts 16/24/32-bit PCM and 32-bit float.
//
// This reader is also the one that will eventually open arbitrary
// user-picked files (M5b "Load sample..."), so it treats every declared
// size in the file as untrusted: nothing is allocated and no seek is taken
// on the strength of a chunk header alone. Everything is checked against
// the file's actual remaining byte count first.
namespace spky {

struct WavData {
    int sample_rate = 48000;
    std::vector<float> l, r;
};

inline bool read_wav(const std::string& path, WavData& out, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open " + path; return false; }

    struct Closer { FILE* f; ~Closer() { std::fclose(f); } } closer{ f };

    // Fix 1: determine the file's real size up front so any declared chunk
    // size can be checked against what's actually there before we trust it
    // for an allocation or a seek.
    if (std::fseek(f, 0, SEEK_END) != 0) { err = "cannot seek in " + path; return false; }
    const long file_size = std::ftell(f);
    if (file_size < 0) { err = "cannot determine size of " + path; return false; }
    if (std::fseek(f, 0, SEEK_SET) != 0) { err = "cannot seek in " + path; return false; }

    // Bytes left between the current file position and EOF, as measured
    // just now. Returns false (and sets err) if ftell fails.
    auto remaining = [&](int64_t& out_remaining) {
        const long pos = std::ftell(f);
        if (pos < 0) { err = "ftell failed on " + path; return false; }
        out_remaining = int64_t(file_size) - int64_t(pos);
        return true;
    };

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
            // Fix 4: anything shorter than the 16-byte base fmt fields would
            // desync every read after it.
            if (csize < 16) { err = "fmt chunk too short (< 16 bytes)"; return false; }

            uint16_t block = 0;
            uint32_t byterate = 0;
            if (!rd_u16(fmt) || !rd_u16(channels) || !rd_u32(rate) ||
                !rd_u32(byterate) || !rd_u16(block) || !rd_u16(bits)) {
                err = "truncated fmt chunk"; return false;
            }
            uint32_t consumed = 16;
            if (fmt == 0xFFFE) {
                // WAVE_FORMAT_EXTENSIBLE: the real format tag is the leading
                // uint16 of a 16-byte SubFormat GUID that follows cbSize,
                // validBitsPerSample and the channel mask. Total fmt chunk
                // is 16 (base) + 2 (cbSize) + 22 (extension) = 40 bytes.
                if (csize < 40) {
                    err = "malformed WAVE_FORMAT_EXTENSIBLE fmt chunk (too short)";
                    return false;
                }
                uint16_t cb_size = 0, valid_bits = 0, sub_fmt = 0;
                uint32_t channel_mask = 0;
                uint8_t guid_suffix[14];
                if (!rd_u16(cb_size) || !rd_u16(valid_bits) || !rd_u32(channel_mask) ||
                    !rd_u16(sub_fmt) || std::fread(guid_suffix, 1, 14, f) != 14) {
                    err = "truncated WAVE_FORMAT_EXTENSIBLE fmt chunk"; return false;
                }
                consumed += 2 + 2 + 4 + 2 + 14;  // == 40
                static const uint8_t kGuidSuffix[14] = {
                    0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00,
                    0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71
                };
                if (cb_size < 22 || std::memcmp(guid_suffix, kGuidSuffix, 14) != 0) {
                    err = "WAVE_FORMAT_EXTENSIBLE with unrecognized SubFormat GUID";
                    return false;
                }
                if (sub_fmt != 1 && sub_fmt != 3) {
                    err = "WAVE_FORMAT_EXTENSIBLE SubFormat is neither PCM nor IEEE float";
                    return false;
                }
                fmt = sub_fmt;  // decode as the effective (real) format tag
            }
            have_fmt = true;
            // Skip any remaining fmt extension bytes, plus the RIFF pad
            // byte if the chunk size is odd (same rule the generic
            // chunk-skip branch below applies). Fix 2/4: do the arithmetic
            // in a 64-bit type and validate against bytes actually left in
            // the file before seeking -- csize is attacker-controlled.
            const int64_t skip = int64_t(csize) - int64_t(consumed) + int64_t(csize & 1);
            if (skip > 0) {
                int64_t rem = 0;
                if (!remaining(rem)) return false;
                if (skip > rem) {
                    err = "fmt chunk extension runs past end of file"; return false;
                }
                if (std::fseek(f, long(skip), SEEK_CUR) != 0) {
                    err = "seek failed while skipping fmt chunk extension"; return false;
                }
            }
        } else if (std::memcmp(cid, "data", 4) == 0) {
            if (!have_fmt) { err = "data chunk before fmt chunk"; return false; }
            if (channels < 1 || channels > 2) {
                err = "unsupported channel count"; return false;
            }
            // Fix 4: reject any bit depth outside what the decoder below
            // actually implements, by name -- this also guarantees bits is
            // a multiple of 8, so there's no separate check needed for that.
            if (bits != 16 && bits != 24 && bits != 32) {
                err = "unsupported bit depth (" + std::to_string(bits) +
                      "); only 16/24/32-bit PCM/float are supported";
                return false;
            }
            const int bytes = bits / 8;

            // Fix 1: a truncated file can declare a data chunk far larger
            // than what's actually left (e.g. 0xFFFFFFFF), which would try
            // to allocate ~20 GB via resize()/vector(csize) before the
            // fread below ever notices the mismatch. Check against the
            // real remaining byte count first and reject by name.
            int64_t rem = 0;
            if (!remaining(rem)) return false;
            if (int64_t(csize) > rem) {
                err = "data chunk declares " + std::to_string(csize) +
                      " bytes but only " + std::to_string(rem) +
                      " remain in " + path;
                return false;
            }

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
                        uint32_t bits32 = uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                                          (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
                        float tmp;
                        std::memcpy(&tmp, &bits32, 4);
                        v = tmp;
                    } else if (bytes == 2) {
                        int16_t s = int16_t(uint16_t(p[0]) | (uint16_t(p[1]) << 8));
                        v = float(s) / 32768.f;
                    } else if (bytes == 3) {
                        // Fix 4: build the 24-bit value in an unsigned type
                        // first -- shifting a signed int32_t's top bit via
                        // `<< 24` is undefined behaviour -- then convert to
                        // signed explicitly before the arithmetic shift.
                        uint32_t u = (uint32_t(p[0]) << 8) | (uint32_t(p[1]) << 16) |
                                     (uint32_t(p[2]) << 24);
                        int32_t s = int32_t(u);
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
            // Fix 2: csize is uint32_t and `long` is 32-bit on Windows, so
            // `long(csize + (csize & 1))` for any csize >= 0x80000000
            // silently becomes negative -- fseek then walks backwards and
            // the read loop can loop indefinitely. Compute in 64-bit and
            // validate against real remaining bytes before seeking.
            // Fix 2: csize is uint32_t and `long` is 32-bit on Windows, so
            // `long(csize + (csize & 1))` for any csize >= 0x80000000
            // silently becomes negative -- fseek then walks backwards and
            // the read loop can loop indefinitely. Compute in 64-bit and
            // validate against real remaining bytes before seeking.
            const int64_t skip = int64_t(csize) + int64_t(csize & 1);
            int64_t rem = 0;
            if (!remaining(rem)) return false;
            if (skip > rem) {
                err = "chunk '" + std::string(cid, 4) + "' size runs past end of file";
                return false;
            }
            if (std::fseek(f, long(skip), SEEK_CUR) != 0) {
                err = "seek failed while skipping chunk '" + std::string(cid, 4) + "'";
                return false;
            }
        }
    }
    err = "no data chunk in " + path;
    return false;
}

}  // namespace spky

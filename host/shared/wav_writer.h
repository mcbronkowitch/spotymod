#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace spky {

// Minimal 16-bit PCM stereo WAV writer (little-endian; desktop host only).
class WavWriter {
public:
    explicit WavWriter(int sample_rate) : _sr(sample_rate) {}

    void push(float l, float r) {
        _samples.push_back(to_i16(l));
        _samples.push_back(to_i16(r));
    }

    bool write(const std::string& path) const {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        uint32_t data_bytes = static_cast<uint32_t>(_samples.size() * sizeof(int16_t));
        uint32_t byte_rate = static_cast<uint32_t>(_sr) * 2u * 2u; // stereo * 2 bytes
        put_tag(f, "RIFF");
        put_u32(f, 36u + data_bytes);
        put_tag(f, "WAVE");
        put_tag(f, "fmt ");
        put_u32(f, 16u);
        put_u16(f, 1u);                              // PCM
        put_u16(f, 2u);                              // channels
        put_u32(f, static_cast<uint32_t>(_sr));
        put_u32(f, byte_rate);
        put_u16(f, 4u);                              // block align
        put_u16(f, 16u);                             // bits per sample
        put_tag(f, "data");
        put_u32(f, data_bytes);
        std::fwrite(_samples.data(), sizeof(int16_t), _samples.size(), f);
        std::fclose(f);
        return true;
    }

private:
    static int16_t to_i16(float v) {
        if (v >  1.f) v =  1.f;
        if (v < -1.f) v = -1.f;
        return static_cast<int16_t>(v * 32767.f);
    }
    static void put_u32(FILE* f, uint32_t v) { std::fwrite(&v, 4, 1, f); }
    static void put_u16(FILE* f, uint16_t v) { std::fwrite(&v, 2, 1, f); }
    static void put_tag(FILE* f, const char* s) { std::fwrite(s, 1, 4, f); }

    int _sr;
    std::vector<int16_t> _samples;
};

} // namespace spky

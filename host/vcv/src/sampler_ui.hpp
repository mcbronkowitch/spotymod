#pragma once
#include <string>
#include <vector>
#include "instrument.h"
#include "shared/wav_reader.h"
#include "shared/wav_writer.h"

// Host-side state of the M5b texture deck, one instance per part. Everything
// here is edit-layer state the engine does not own and the module must
// persist: the engine keeps the audio, this keeps how it got there.
namespace spkyvcv {

struct SamplerPartState {
    std::string path;       // last file loaded, "" if none / live-recorded
    int   tapeIdx  = 1;     // 0 = Digital, 1 = Tape -- an index, because the
                            // context menu binds it with createIndexPtrSubmenuItem
    bool  reverse  = false;
    float feedback = 0.95f; // overdub feedback, ~-3 dB (engine default)
    bool  testTone = false; // dev: ENG's sampler slot plays the test tone instead
    bool  factoryLoaded = false;  // content came from the factory WAV, not the user
};

// Linear resample to the engine's rate. A file recorded at 44.1 kHz would
// otherwise play ~9% sharp in a 48 kHz Rack -- that is an import bug, not the
// instrument's tape idiom (the tape idiom is what the PITCH knob and Tape
// mode do, deliberately, to material that is already in the buffer).
// Linear is enough: this runs once per load, off the audio thread, and the
// grain engine's own read is linear-interpolated too.
inline void resample_linear(std::vector<float>& v, double ratio) {
    if (v.empty()) return;
    const size_t n = (size_t)((double)v.size() * ratio);
    if (n < 2) return;
    std::vector<float> out(n);
    for (size_t i = 0; i < n; ++i) {
        const double src = (double)i / ratio;
        const size_t i0 = (size_t)src;
        const size_t i1 = i0 + 1 < v.size() ? i0 + 1 : i0;
        const float  f  = (float)(src - (double)i0);
        out[i] = v[i0] + (v[i1] - v[i0]) * f;
    }
    v.swap(out);
}

// Read a WAV off disk into a part's record buffer. The reader hands back
// deinterleaved channels, which is exactly what load_sample takes. A mono
// file arrives with l == r from the reader, so nothing special is needed.
// engine_sr is the rate the Instrument is currently running at.
inline bool load_wav_into(spky::Instrument& inst, int part,
                          const std::string& path, float engine_sr,
                          std::string& err) {
    spky::WavData d;
    if (!spky::read_wav(path, d, err)) return false;
    if (d.l.empty()) { err = "file contains no samples"; return false; }
    if (d.sample_rate > 0 && engine_sr > 0.f
        && (float)d.sample_rate != engine_sr) {
        const double ratio = (double)engine_sr / (double)d.sample_rate;
        resample_linear(d.l, ratio);
        resample_linear(d.r, ratio);
    }
    inst.load_sample(part, d.l.data(), d.r.data(), d.l.size());
    return true;
}

// Write a part's recorded content out. The frames belong to the host, and
// rec_size() is the valid length -- reading past it would emit whatever the
// buffer held before. WavWriter is a 16-bit PCM stereo writer that takes
// interleaved pushes, so there is no float vector to build.
inline bool save_wav_from(const spky::Instrument& inst, int part,
                          const std::string& path, float sr, std::string& err) {
    const size_t n = inst.sampler_rec_size(part);
    const spky::SampleBuffer::Frame* f = inst.sampler_data(part);
    if (!n || !f) { err = "nothing recorded"; return false; }
    spky::WavWriter w((int)sr);
    for (size_t i = 0; i < n; ++i) w.push(f[i].l, f[i].r);
    if (!w.write(path)) { err = "could not write " + path; return false; }
    return true;
}

}  // namespace spkyvcv

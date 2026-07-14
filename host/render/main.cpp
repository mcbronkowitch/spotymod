#include <cstdio>
#include <cstddef>
#include <string>
#include "instrument.h"
#include "render/scenario.h"
#include "render/wav_writer.h"

using namespace spky;

// FX memory, injected per the engine's no-heap contract (FxMem pattern).
static float s_echo[spky::PART_COUNT][2][spky::Flux::kMaxSamples];
static spky::AmbientReverb s_reverb;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: render <scenario.json> [out.wav] [mods.csv]\n");
        return 1;
    }
    std::string scen_path = argv[1];
    std::string wav_path  = argc > 2 ? argv[2] : "out.wav";
    std::string csv_path  = argc > 3 ? argv[3] : "mods.csv";

    Scenario scen;
    std::string err;
    if (!load_scenario(scen_path, scen, err)) {
        std::printf("scenario error: %s\n", err.c_str());
        return 2;
    }

    Instrument inst;
    FxMem fx_mem;
    for (int p = 0; p < PART_COUNT; ++p)
        for (int c = 0; c < 2; ++c) fx_mem.echo[p][c] = s_echo[p][c];
    fx_mem.reverb = &s_reverb;
    inst.init(static_cast<float>(scen.sample_rate), fx_mem);
    inst.set_tempo_bpm(scen.bpm);
    for (const auto& e : scen.init_events) apply_event(inst, e);

    WavWriter wav(scen.sample_rate);
    FILE* csv = std::fopen(csv_path.c_str(), "wb");
    if (csv) {
        std::fprintf(csv, "t,"
            "a_src,a_size,a_pitch,a_motion,a_level,a_pcv,a_gate,"
            "a_fx0,a_fx1,a_fx2,a_fx3,a_fx4,a_voices,a_v0,a_v1,a_v2,a_v3,"
            "b_src,b_size,b_pitch,b_motion,b_level,b_pcv,b_gate,"
            "b_fx0,b_fx1,b_fx2,b_fx3,b_fx4,b_voices,b_v0,b_v1,b_v2,b_v3,"
            "morph,couple,drift,weather,phase_err\n");
    }

    const size_t total = static_cast<size_t>(scen.duration_s * scen.sample_rate);
    const int    csv_decim = 64;
    size_t next_event = 0;

    for (size_t i = 0; i < total; ++i) {
        double t = static_cast<double>(i) / scen.sample_rate;
        while (next_event < scen.events.size() && scen.events[next_event].time_s <= t) {
            apply_event(inst, scen.events[next_event]);
            ++next_event;
        }

        float l = 0.f, r = 0.f;
        inst.process(nullptr, nullptr, &l, &r, 1);
        wav.push(l, r);

        if (csv && (i % csv_decim == 0)) {
            std::fprintf(csv, "%.5f", t);
            for (int p = 0; p < 2; ++p) {
                for (int s = 0; s < LANE_COUNT; ++s)
                    std::fprintf(csv, ",%.4f", inst.lane_output(p, s));
                std::fprintf(csv, ",%.4f,%d", inst.pitch_cv(p), inst.gate(p) ? 1 : 0);
                for (int s = 0; s < FXT_COUNT; ++s)
                    std::fprintf(csv, ",%.4f", inst.fx_target_value(p, s));
                std::fprintf(csv, ",%d", inst.active_voices(p));
                for (int v = 0; v < 4; ++v)
                    std::fprintf(csv, ",%.4f", inst.voice_env(p, v));
            }
            std::fprintf(csv, ",%.4f,%.4f,%.4f,%.4f,%.4f",
                         inst.morph(), inst.couple(), inst.drift(),
                         inst.weather(), inst.phase_err());
            std::fprintf(csv, "\n");
        }
    }

    if (csv) std::fclose(csv);
    if (!wav.write(wav_path)) {
        std::printf("failed to write %s\n", wav_path.c_str());
        return 3;
    }
    std::printf("wrote %s (%zu frames) and %s\n", wav_path.c_str(), total, csv_path.c_str());
    return 0;
}

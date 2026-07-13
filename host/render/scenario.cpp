#include "render/scenario.h"
#include <algorithm>
#include <fstream>
#include <exception>
#include "nlohmann/json.hpp"

using namespace spky;
using json = nlohmann::json;

static Event parse_event(const json& j, bool timed) {
    Event e;
    if (timed) e.time_s = j.value("t", 0.0);
    e.action = j.value("action", std::string());
    e.part   = j.value("part", 0);
    e.slot   = j.value("slot", 0);
    e.flag   = j.value("flag", false);
    e.ivalue = j.value("ivalue", 0);
    if (j.contains("value")) {
        if (j["value"].is_string()) e.svalue = j["value"].get<std::string>();
        else                        e.value  = j["value"].get<float>();
    }
    return e;
}

bool spky::load_scenario(const std::string& path, Scenario& out, std::string& err) {
    std::ifstream in(path);
    if (!in) { err = "cannot open " + path; return false; }
    json j;
    try { in >> j; }
    catch (const std::exception& ex) { err = ex.what(); return false; }

    out.sample_rate = j.value("sample_rate", 48000);
    out.bpm         = j.value("bpm", 120.f);
    out.duration_s  = j.value("duration_s", 10.0);

    if (j.contains("init"))
        for (const auto& e : j["init"]) out.init_events.push_back(parse_event(e, false));
    if (j.contains("events"))
        for (const auto& e : j["events"]) out.events.push_back(parse_event(e, true));

    std::stable_sort(out.events.begin(), out.events.end(),
                     [](const Event& a, const Event& b) { return a.time_s < b.time_s; });
    return true;
}

static SyncMode parse_sync(const std::string& s) {
    if (s == "sync")    return SyncMode::Sync;
    if (s == "triplet") return SyncMode::SyncTriplet;
    return SyncMode::Free;
}

static QuantMode parse_qmode(const std::string& s) {
    if (s == "chrom") return QuantMode::Chrom;
    if (s == "free")  return QuantMode::Free;
    return QuantMode::Scale;
}

static FxBlock parse_fx_block(const std::string& s) {
    return s == "grit" ? FxBlock::Grit : FxBlock::Flux;
}

static GritMode parse_grit_mode(const std::string& s) {
    return s == "reduce" ? GritMode::Reduce : GritMode::Drive;
}

static EngineId parse_engine(const std::string& s) {
    return s == "test_tone" ? ENGINE_TEST_TONE : ENGINE_SYNTH;
}

static int parse_scale_name(const std::string& s) {
    if (s == "min_pent") return SCALE_MIN_PENT;
    if (s == "aeolian")  return SCALE_AEOLIAN;
    if (s == "maj_pent") return SCALE_MAJ_PENT;
    if (s == "lydian")   return SCALE_LYDIAN;
    if (s == "whole")    return SCALE_WHOLE;
    return SCALE_DORIAN;   // "dorian" and anything unknown -> the default
}

void spky::apply_event(Instrument& inst, const Event& e) {
    const std::string& a = e.action;
    if      (a == "set_tempo_bpm")     inst.set_tempo_bpm(e.value);
    else if (a == "set_rate")          inst.set_rate(e.part, e.value);
    else if (a == "set_sync_mode")     inst.set_sync_mode(e.part, parse_sync(e.svalue));
    else if (a == "set_shape")         inst.set_shape(e.part, e.value);
    else if (a == "set_probability")   inst.set_probability(e.part, e.value);
    else if (a == "set_smooth")        inst.set_smooth(e.part, e.value);
    else if (a == "set_range")         inst.set_range(e.part, e.value);
    else if (a == "set_entropy")       inst.set_entropy(e.part, e.value);
    else if (a == "set_depth")         inst.set_depth(e.part, e.value);
    else if (a == "set_tune")          inst.set_tune(e.part, e.value);
    else if (a == "set_step")          inst.set_step(e.part, e.flag, e.ivalue);
    else if (a == "set_fixed_slew")    inst.set_fixed_slew(e.part, e.flag);
    else if (a == "set_target_active") inst.set_target_active(e.part, e.slot, e.flag);
    else if (a == "set_target_base")   inst.set_target_base(e.part, e.slot, e.value);
    else if (a == "set_target_depth")  inst.set_target_depth(e.part, e.slot, e.value);
    else if (a == "set_scale")      inst.set_scale(parse_scale_name(e.svalue));
    else if (a == "set_quant_mode") inst.set_quant_mode(e.part, parse_qmode(e.svalue));
    else if (a == "set_root")       inst.set_root(e.part, e.ivalue);
    else if (a == "set_fx_on")            inst.set_fx_on(e.part, parse_fx_block(e.svalue), e.flag);
    else if (a == "set_grit_mode")        inst.set_grit_mode(e.part, parse_grit_mode(e.svalue));
    else if (a == "set_fx_target_active") inst.set_fx_target_active(e.part, e.slot, e.flag);
    else if (a == "set_fx_target_base")   inst.set_fx_target_base(e.part, e.slot, e.value);
    else if (a == "set_fx_target_depth")  inst.set_fx_target_depth(e.part, e.slot, e.value);
    else if (a == "set_flux_mix")         inst.set_flux_mix(e.part, e.value);
    else if (a == "set_grit_mix")         inst.set_grit_mix(e.part, e.value);
    else if (a == "set_comp")             inst.set_comp(e.part, e.value);
    else if (a == "set_master_drive")     inst.set_master_drive(e.value);
    else if (a == "set_reverb_size")      inst.set_reverb_size(e.value);
    else if (a == "set_reverb_tone")      inst.set_reverb_tone(e.value);
    else if (a == "set_reverb_decay")     inst.set_reverb_decay(e.value);
    else if (a == "set_reverb_depth")     inst.set_reverb_depth(e.value);
    else if (a == "set_engine")          inst.set_engine(e.part, parse_engine(e.svalue));
    else if (a == "set_voice_attack")    inst.set_voice_attack(e.part, e.value);
    else if (a == "set_voice_decay")     inst.set_voice_decay(e.part, e.value);
    else if (a == "set_voice_resonance") inst.set_voice_resonance(e.part, e.value);
    else if (a == "set_voice_sub")       inst.set_voice_sub(e.part, e.value);
    else if (a == "set_voice_detune")    inst.set_voice_detune(e.part, e.value);
    else if (a == "trigger_manual")      inst.trigger_manual(e.part);
    else if (a == "capture_now")         inst.capture_now(e.part);
    else if (a == "set_replay")          inst.set_replay(e.part, e.flag);
    else if (a == "set_morph")           inst.set_morph(e.value);
    else if (a == "set_couple")          inst.set_couple(e.value);
    else if (a == "set_drift")           inst.set_drift(e.value);
    else if (a == "spot")                inst.spot();
    else if (a == "settle")              inst.settle();
    // unknown actions are ignored on purpose (forward-compatible scenarios)
}

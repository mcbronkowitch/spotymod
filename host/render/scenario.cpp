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
    else if (a == "set_evolve")        inst.set_evolve(e.part, e.value);
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
    // unknown actions are ignored on purpose (forward-compatible scenarios)
}

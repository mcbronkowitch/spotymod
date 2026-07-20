#pragma once
#include <string>
#include <vector>
#include "instrument.h"

namespace spky {

struct Event {
    double      time_s = 0.0;
    std::string action;
    int         part = 0;
    int         slot = 0;
    float       value = 0.f;
    bool        flag = false;
    int         ivalue = 0;
    std::string svalue;   // string-valued args (e.g. sync mode)
};

struct Scenario {
    int    sample_rate = 48000;
    float  bpm = 120.f;
    double duration_s = 10.0;
    std::string input_wav;            // fed into Instrument::process inputs
    std::vector<Event> init_events;   // applied at t = 0
    std::vector<Event> events;        // timeline, sorted by time_s
};

// Parse a scenario JSON file. Returns false (and sets err) on read/parse error.
bool load_scenario(const std::string& path, Scenario& out, std::string& err);

// Apply one event to the instrument.
void apply_event(Instrument& inst, const Event& e);

} // namespace spky

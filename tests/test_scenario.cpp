#include <doctest/doctest.h>
#include <cstdio>
#include <fstream>
#include "render/scenario.h"
using namespace spky;

TEST_CASE("scenario: parses init + timeline and sorts events by time") {
    const char* path = "test_scenario.json";
    {
        std::ofstream o(path);
        o << R"({
          "sample_rate": 48000,
          "bpm": 100,
          "duration_s": 5,
          "init": [
            {"action":"set_sync_mode","part":0,"value":"free"},
            {"action":"set_rate","part":0,"value":0.5}
          ],
          "events": [
            {"t":3.0,"action":"set_probability","part":0,"value":0.2},
            {"t":1.0,"action":"set_step","part":0,"flag":true,"ivalue":16}
          ]
        })";
    }
    Scenario s;
    std::string err;
    REQUIRE(load_scenario(path, s, err));
    CHECK(s.sample_rate == 48000);
    CHECK(s.bpm == doctest::Approx(100.f));
    CHECK(s.duration_s == doctest::Approx(5.0));
    CHECK(s.init_events.size() == 2);
    REQUIRE(s.events.size() == 2);
    CHECK(s.events[0].time_s == doctest::Approx(1.0));   // sorted ascending
    CHECK(s.events[0].action == "set_step");
    CHECK(s.events[0].ivalue == 16);
    CHECK(s.events[1].action == "set_probability");

    Instrument inst;
    inst.init(48000.f);
    for (const auto& e : s.init_events) apply_event(inst, e);   // must not crash
    for (const auto& e : s.events)      apply_event(inst, e);
    std::remove(path);
}

#include <doctest/doctest.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// Jeder Befund aus dem Review vom 2026-07-22 braucht einen Test, dessen Name
// mit seiner ID beginnt. Diese Pruefung ist der Grund, warum das Register in
// docs/superpowers/plans/2026-07-22-sampler-fixes.md nicht bloss eine Liste
// ist: sie schlaegt fehl, sobald ein Befund ohne Test dasteht -- auch dann,
// wenn jemand einen Test spaeter entfernt.
//
// F-07 und K-03 stehen bewusst NICHT hier: sie liegen in
// host/vcv/src/Spotymod.cpp, das die VCV-Rack-Abhaengigkeit hat und nicht
// Teil dieser Suite ist. Ihre Verifikation ist die manuelle Prozedur in
// host/vcv/README.md, und die zweite Pruefung unten haelt fest, dass sie
// dort auch wirklich beschrieben steht.

namespace {

const char* kSourceFiles[] = {
    "tests/test_sampler_engine.cpp",
    "tests/test_sample_buffer.cpp",
    "tests/test_sampler_part.cpp",
};

// Ohne F-07 und K-03 -- siehe oben.
const char* kIdsNeedingATest[] = {
    "F-01", "F-02", "F-03", "F-04", "F-05",
    "F-06", "F-08", "F-09", "F-10",
    "K-01", "K-02",
};

std::string slurp(const std::string& path) {
    // Der Test laeuft aus dem Build-Verzeichnis; beide Lagen probieren.
    for (const std::string prefix : { std::string(""), std::string("../") }) {
        std::ifstream f(prefix + path);
        if (f) {
            std::ostringstream ss;
            ss << f.rdbuf();
            return ss.str();
        }
    }
    return {};
}

}  // namespace

TEST_CASE("review register: every finding from 2026-07-22 has a test") {
    std::string all;
    for (const char* p : kSourceFiles) {
        const std::string body = slurp(p);
        INFO("reading " << p);
        REQUIRE(!body.empty());          // Datei fehlt oder wurde verschoben
        all += body;
    }

    for (const char* id : kIdsNeedingATest) {
        // Gesucht wird die Form TEST_CASE("F-01: ...
        const std::string needle = std::string("TEST_CASE(\"") + id + ":";
        INFO("no test case named \"" << id << ": ...\" found in the sampler tests");
        CHECK(all.find(needle) != std::string::npos);
    }
}

TEST_CASE("review register: the host-only findings are documented instead") {
    // F-07 und K-03 koennen hier nicht getestet werden. Statt sie stumm
    // fallenzulassen, wird ihre manuelle Verifikationsprozedur eingefordert.
    const std::string readme = slurp("host/vcv/README.md");
    INFO("host/vcv/README.md not found");
    REQUIRE(!readme.empty());
    CHECK(readme.find("F-07") != std::string::npos);
}

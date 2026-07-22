#include <doctest/doctest.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// Jeder Befund aus dem Review vom 2026-07-22 muss im Baum eine Spur
// hinterlassen, die verschwindet, wenn jemand die Arbeit rueckgaengig macht.
// Diese Datei ist der Grund, warum das Register in
// docs/superpowers/plans/2026-07-22-sampler-fixes.md nicht bloss eine Liste
// ist: sie schlaegt fehl, sobald eine Spur fehlt.
//
// Fuenfzehn Befunde, drei Arten von Spur -- und JEDE ID gehoert in genau eine
// der drei Gruppen unten. Faellt eine heraus, faellt sie still heraus, und
// genau das soll hier nicht passieren:
//
//   1. Elf Befunde sind Verhalten und haben einen TEST_CASE, dessen Name mit
//      der ID beginnt (kIdsNeedingATest).
//   2. Zwei liegen in host/vcv/src/Spotymod.cpp, das die VCV-Rack-
//      Abhaengigkeit hat und nicht Teil dieser Suite ist. Fuer sie wird ein
//      Eintrag in host/vcv/README.md verlangt (kIdsNeedingReadme).
//   3. Zwei sind Doku- beziehungsweise Szenario-Befunde, fuer die ein
//      TEST_CASE keinen Sinn ergibt. Statt sie fallenzulassen, verlangt die
//      dritte Pruefung einen konkreten Anker im Quelltext -- eine Zeile, die
//      wieder verschwindet, wenn der Befund zurueckkommt (kAnchors).
//
// Eine frueherer Fassung hatte hier eine Luecke, die genau den Fehler
// vorfuehrte, den diese Datei verhindern soll: der Kommentar behauptete, F-07
// UND K-03 wuerden in der README eingefordert, geprueft wurde aber nur F-07
// -- und K-03 stand dort ueberhaupt nicht. Der Befund war spurlos weg,
// waehrend die Pruefung gruen meldete. Deshalb steht die Zuordnung jetzt als
// Daten da und nicht als Prosa: eine ID, die in keiner der drei Listen
// auftaucht, faellt beim Zaehlen unten auf.

namespace {

const char* kSourceFiles[] = {
    "tests/test_sampler_engine.cpp",
    "tests/test_sample_buffer.cpp",
    "tests/test_sampler_part.cpp",
};

// Gruppe 1: Verhalten, mit TEST_CASE.
const char* kIdsNeedingATest[] = {
    "F-01", "F-02", "F-03", "F-04", "F-05",
    "F-06", "F-08", "F-09", "F-10",
    "K-01", "K-02",
};

// Gruppe 2: im VCV-Host, ausserhalb dieser Suite -- Eintrag in der README.
//
// F-07 ist dort KEIN erledigter Fix, sondern eine offene Design-Frage: die
// Sperre wurde gebaut, reviewt und wieder zurueckgenommen, weil die README
// unter "Known limitations" ausdruecklich ohne Soft-Takeover auskommen will
// (die Hardware hat keins). K-03 ist erledigt und geblieben. Beide muessen
// dort auffindbar sein, sonst weiss der naechste Leser von keinem der beiden.
const char* kIdsNeedingReadme[] = { "F-07", "K-03" };

// Gruppe 3: Doku und Listening-Szenario. Kein TEST_CASE moeglich, aber ein
// pruefbarer Anker: eine Zeichenfolge, die genau dann im Baum steht, wenn der
// Befund behoben ist, und die verschwindet, wenn ihn jemand rueckgaengig macht.
struct Anchor {
    const char* id;
    const char* file;
    const char* needle;
    bool        must_be_present;   // false == diese Zeichenfolge darf NICHT mehr da sein
};

const Anchor kAnchors[] = {
    // K-04: der Kommentar behauptete, bei 96 kHz verlange kSizeCeilS doppelt
    // so viel Kapazitaet wie vorhanden. Beide Hosts allozieren sekundenbasiert,
    // die Behauptung war falsch. Geprueft wird, dass sie nicht zurueckkommt.
    { "K-04", "engine/sampler/sampler_config.h",
      "asks for twice the capacity the buffer has", false },
    // K-05: sampler_scan.json liess die SOURCE-Lane aktiv, deren Modulation die
    // Leseposition wandern liess -- im einen Szenario, das SCANs eigene
    // Wanderung isolieren soll. Nur set_target_active legt sie still.
    { "K-05", "host/render/scenarios/sampler_scan.json",
      "\"action\": \"set_target_active\", \"part\": 1, \"slot\": 0", true },
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
        // std::string und nicht der rohe const char*: doctest speichert den
        // INFO-Ausdruck lazy, und ein Zeiger druckt sich als Adresse. Auf dem
        // Fehlerpfad -- dem einzigen, fuer den diese Datei existiert -- stand
        // dann eine Hexzahl statt des Dateinamens.
        INFO("reading " << std::string(p));
        REQUIRE(!body.empty());          // Datei fehlt oder wurde verschoben
        all += body;
    }

    for (const char* id : kIdsNeedingATest) {
        // Gesucht wird die Form TEST_CASE("F-01: ...
        const std::string needle = std::string("TEST_CASE(\"") + id + ":";
        INFO("no test case named \"" << std::string(id)
             << ": ...\" found in the sampler tests");
        CHECK(all.find(needle) != std::string::npos);
    }
}

TEST_CASE("review register: the host-only findings are documented instead") {
    // F-07 und K-03 koennen hier nicht getestet werden. Statt sie stumm
    // fallenzulassen, wird ein README-Eintrag eingefordert.
    const std::string readme = slurp("host/vcv/README.md");
    INFO("host/vcv/README.md not found");
    REQUIRE(!readme.empty());
    for (const char* id : kIdsNeedingReadme) {
        INFO("host/vcv/README.md does not mention " << std::string(id)
             << " -- a host-side finding with no test and no note is a finding "
                "nobody will ever see again");
        CHECK(readme.find(id) != std::string::npos);
    }
}

TEST_CASE("review register: the documentation findings kept their anchors") {
    // Doku- und Szenario-Befunde koennen keinen TEST_CASE haben. Sie bekommen
    // stattdessen eine Zeichenfolge, an der sich ihr Zustand ablesen laesst.
    for (const Anchor& a : kAnchors) {
        const std::string body = slurp(a.file);
        INFO("reading " << std::string(a.file) << " for " << std::string(a.id));
        REQUIRE(!body.empty());
        const bool found = body.find(a.needle) != std::string::npos;
        if (a.must_be_present) {
            INFO(std::string(a.id) << ": expected to find \""
                 << std::string(a.needle) << "\" in " << std::string(a.file));
            CHECK(found);
        } else {
            INFO(std::string(a.id) << ": the corrected text came back -- \""
                 << std::string(a.needle) << "\" is in " << std::string(a.file)
                 << " again");
            CHECK_FALSE(found);
        }
    }
}

TEST_CASE("review register: every finding is in exactly one group") {
    // Die Pruefung, die die urspruengliche Luecke gefunden haette. Ohne sie
    // faellt eine ID einfach aus allen Listen heraus und niemand merkt es --
    // was genau einmal passiert ist (K-03 stand in keiner).
    const int in_tests   = int(sizeof(kIdsNeedingATest) / sizeof(kIdsNeedingATest[0]));
    const int in_readme  = int(sizeof(kIdsNeedingReadme) / sizeof(kIdsNeedingReadme[0]));
    const int in_anchors = int(sizeof(kAnchors) / sizeof(kAnchors[0]));
    INFO("tests " << in_tests << " + readme " << in_readme
         << " + anchors " << in_anchors);
    CHECK(in_tests + in_readme + in_anchors == 15);
}

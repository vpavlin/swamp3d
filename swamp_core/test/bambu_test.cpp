// swamp_bambu.hpp against the fake printer (bambu_mock.py): discovery, wrong access code,
// FTPS upload byte-for-byte, MQTT status, starting a print, a print of a missing file.
#include "swamp_bambu.hpp"
#include <iostream>
#include <sstream>
using namespace swamp;
using json = nlohmann::json;
static int fails = 0, passes = 0;
#define CHECK(c, w) do { if (c) passes++; else { fails++; std::cerr << "FAIL: " << w << "\n"; } } while (0)

static std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }

int main(int argc, char** argv) {
    std::string work = argv[1];
    bambu::Printer p;
    p.ip = "127.0.0.1"; p.serial = "03919A3B0000001"; p.accessCode = "12345678";
    p.ftpsPort = atoi(argv[2]); p.mqttPort = atoi(argv[3]);
    int ssdpPort = atoi(argv[4]);

    auto found = bambu::discover(2500, ssdpPort);
    CHECK(found.size() == 1 && found[0].serial == p.serial && found[0].ip == "127.0.0.1" && found[0].model == "N2S" && found[0].name == "Mock A1",
          "discovery finds the printer: ip, serial, model, name");

    std::string err;
    bambu::Printer wrong = p; wrong.accessCode = "00000000";
    json st;
    CHECK(!bambu::status(wrong, st, err) && err.find("access code") != std::string::npos, "a wrong access code is reported as such (" + err + ")");

    err.clear();
    CHECK(bambu::status(p, st, err) && st.value("gcode_state", "") == "IDLE", "status over MQTT: IDLE (" + err + ")");

    std::string src = argv[5];   // a real sliced .gcode.3mf
    long long last = 0;
    err.clear();
    bool up = bambu::upload(p, src, "swamp-test.gcode.3mf", err, [&](long long s, long long) { last = s; });
    CHECK(up && slurp(work + "/sd/swamp-test.gcode.3mf") == slurp(src), "FTPS upload lands on the SD card byte for byte (" + err + ")");
    CHECK(last == (long long)slurp(src).size(), "upload progress reaches the full size");

    err.clear();
    CHECK(bambu::startPrint(p, "swamp-test.gcode.3mf", "Swamp test", false, err), "the print starts (" + err + ")");
    std::string mock = slurp(work + "/mock.log");
    CHECK(mock.find("\"command\": \"project_file\"") != std::string::npos && mock.find("file:///sdcard/swamp-test.gcode.3mf") != std::string::npos &&
          mock.find("Metadata/plate_1.gcode") != std::string::npos, "the project_file command names the SD file and plate 1");
    err.clear();
    CHECK(bambu::status(p, st, err) && st.value("gcode_state", "") == "PREPARE", "the printer now reports PREPARE");

    err.clear();
    CHECK(!bambu::startPrint(p, "missing.gcode.3mf", "x", false, err) && err.find("refused") != std::string::npos, "a refused print is reported (" + err + ")");

    // a printer busy with someone else's job must not read as "accepted" (review 2026-10-07)
    auto setBusy = [&](bool busy) { bambu::Mqtt m; std::string e2; if (m.connect(p, e2)) { m.publishTo("device/" + p.serial + "/mock", json{{"busy", busy}}, e2); m.close(); } };
    setBusy(true);
    err.clear();
    CHECK(!bambu::startPrint(p, "swamp-test.gcode.3mf", "My new job", false, err), "a busy printer's RUNNING report is not taken as accepting our job (" + err + ")");
    setBusy(false);

    err.clear();
    std::string pin;
    CHECK(bambu::status(p, st, err, &pin) && pin.size() == 64, "the printer's certificate fingerprint is captured");
    bambu::Printer pinned = p; pinned.certPinMqtt = pin; pinned.certPinFtps = pin;
    err.clear();
    CHECK(bambu::status(pinned, st, err), "a matching pinned certificate is accepted");
    bambu::Printer impostor = p; impostor.certPinMqtt = std::string(64, 'a');
    err.clear();
    CHECK(!bambu::status(impostor, st, err) && err.find("certificate changed") != std::string::npos, "a different certificate is refused before the access code is sent");

    std::cout << passes << " passed, " << fails << " failed\n";
    return fails ? 1 : 0;
}

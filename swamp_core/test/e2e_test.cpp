// End-to-end over the fake SDK: real SwampCoreImpl instances (creator, maker, hub, late joiner,
// attacker) publish, browse, fetch through a hub after the creator goes offline, verify hashes,
// comment, like, post makes, version, and catch up. Run: swamp_core/test/run-tests.sh
#include "swamp_core_impl.h"
#include "logos_sdk.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <iostream>
#include <filesystem>
#include <fstream>

using json = nlohmann::json;
namespace fs = std::filesystem;
void SwampCoreImpl::stateChanged(const std::string&) {}
static int fails = 0, passes = 0;
#define CHECK(c, w) do { if (c) passes++; else { fails++; std::cerr << "FAIL: " << w << "\n"; } } while (0)

struct Peer { std::string name; FakeLoamNode bus; FakeStoreNode store; SwampCoreImpl core;
              json call(const std::string& r) { json j = json::parse(r); if (!j.value("ok", false)) std::cerr << "  [" << name << "] " << r << "\n"; return j; }
              json snap() { return json::parse(core.snapshot()); } };

static void pump(int ms) { QElapsedTimer t; t.start(); while (t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 10); }
static bool waitFor(std::function<bool()> pred, int ms = 5000) { QElapsedTimer t; t.start(); while (t.elapsed() < ms) { if (pred()) return true; QCoreApplication::processEvents(QEventLoop::AllEvents, 10); } return pred(); }

static std::string root;
static Peer* spawn(const std::string& name, bool hub = false) {
    std::string d = root + "/" + name;
    setenv("SWAMP_CORE_DATA", (d + "/data").c_str(), 1);
    setenv("SWAMP_DOWNLOADS", (d + "/Downloads").c_str(), 1);
    if (hub) setenv("SWAMP_HUB", "1", 1); else unsetenv("SWAMP_HUB");
    Peer* p = new Peer();
    p->name = name; p->bus.name = name; p->store.name = name;
    p->core.modules().loam_core.node = &p->bus;
    p->core.modules().storage_module.node = &p->store;
    FakeLoamBus::get().nodes.push_back(&p->bus);
    FakeStoreNet::get().nodes.push_back(&p->store);
    p->core.fakeStart();
    return p;
}

int main(int argc, char** argv) {
    setenv("SWAMP_EXPERIMENTAL_PRINT", "1", 1);   // the LAN-printing tests below; the last node runs without it
    std::string sharedToolId;   // for the share-link test at the end: a model in Fashion, which every index carries
    QCoreApplication app(argc, argv);
    std::string repo = argc > 1 ? argv[1] : ".";
    root = std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") + "/swamp-e2e";
    fs::remove_all(root);
    setenv("SWAMP_TICK_MS", "60", 1);
    setenv("SWAMP_INDEX_EVERY_MS", "1000", 1);   // the hub indexes every second here
    setenv("SWAMP_INDEX_UPLOAD_TIMEOUT_MS", "8000", 1);
    setenv("SWAMP_HUB_PULL_CHECK_MS", "200", 1);    // the hub's fetch -> local-write cycle, sped up
    setenv("SWAMP_HUB_PULL_WRITE_MS", "400", 1);
    setenv("SWAMP_HUB_PULL_RETRY_MS", "300", 1);   // ...and gives up on a stuck shard upload after 8 s
    setenv("SWAMP_INCLUSION_EVERY_MS", "1000", 1);   // creators audit indexers every second
    setenv("SWAMP_INCLUSION_GRACE_MS", "0", 1);
    setenv("SWAMP_OMISSION_CONFIRM_MS", "1500", 1);
    const std::string stl1 = repo + "/swamp_core/test/meshes/torus.stl", stl2 = repo + "/swamp_core/test/meshes/twist.stl";   // committed test meshes

    Peer* alice = spawn("alice");
    Peer* bob = spawn("bob");
    Peer* hub = spawn("hub", true);
    pump(1500);   // deferred startup (1 s) + connect
    CHECK(alice->snap()["status"] == "Connected" && alice->snap()["storage"]["hostOwned"] == true, "connected; host-owned storage adopted");

    alice->call(alice->core.setProfile(json{{"name", "Alice"}, {"bio", "prints mules"}}.dump()));
    json pub = alice->call(alice->core.publish(json{{"title", "Geometric bracelet"}, {"summary", "a faceted bracelet"}, {"licence", "CC-BY-4.0"},
        {"tags", {"fashion", "bracelet"}}, {"files", {{{"path", stl1}}}}}.dump()));
    std::string mid = pub.value("modelId", "");
    CHECK(pub.value("ok", false) && mid.size() == 32 && pub.value("v", 0) == 1, "publish returns a model id at once (local-first)");

    // bob sees it with a thumbnail and a fingerprint
    CHECK(waitFor([&] { return json::parse(bob->core.listModels("{}"))["models"].size() == 1; }), "bob's catalogue shows the model");
    json card = json::parse(bob->core.listModels(json{{"q", "bracelet"}}.dump()))["models"][0];
    CHECK(card["creatorName"] == "Alice" && card["licence"] == "CC-BY-4.0", "card: creator name + licence");
    {   // pictures are lazy (ADR 0014): nothing is fetched until the view asks for it
        std::string vdir0 = root + "/plugins0/swamp"; fs::create_directories(vdir0);
        json c0 = json::parse(bob->core.listModels("{}"))["models"][0];
        pump(500);
        CHECK(c0["thumb"].is_null() && c0.value("thumbSha", "").size() == 64, "the thumbnail isn't fetched before anyone looks at it");
        json first = json::parse(bob->core.cacheImage(c0["thumbSha"], vdir0));
        CHECK(first.value("pending", false), "asking for it starts the fetch (pending)");
        CHECK(waitFor([&] { return json::parse(bob->core.cacheImage(c0["thumbSha"], vdir0)).value("ok", false); }, 5000), "...and it arrives");
    }
    json model = json::parse(bob->core.getModel(mid))["model"];
    json v1 = model["versions"][0];
    CHECK(v1["files"][0]["name"] == "torus.stl" && v1["images"][0]["kind"] == "thumb", "version lists the STL and an auto thumbnail");
    CHECK(!v1.contains("fp") || v1["fp"].is_null(), "getModel hides the fingerprint blob from the UI");
    {   // views are sandboxed in Basecamp 0.3: pictures are copied into the view's own dir
        std::string tsha = v1["images"][0]["sha256"];
        std::string vdir = root + "/plugins/swamp"; fs::create_directories(vdir);
        json img = json::parse(bob->core.cacheImage(tsha, vdir));
        std::string ip = img.value("path", "");
        CHECK(img.value("ok", false) && ip.rfind(vdir + "/cache/", 0) == 0 && fs::exists(ip), "cacheImage copies the thumbnail into the view's cache dir");
        CHECK(fs::exists(ip) && fs::file_size(ip) < 100000, "the thumbnail is compressed");
        CHECK(!json::parse(bob->core.cacheImage(v1["files"][0]["sha256"], vdir)).value("ok", true), "refuses a blob that isn't a listed picture");
        CHECK(!json::parse(bob->core.cacheImage(tsha, root + "/alice")).value("ok", true), "refuses a directory that isn't the view's");
    }
    CHECK(json::parse(alice->core.listModels(json{{"tag", "Bracelet"}}.dump()))["models"].size() == 1, "tag filter (case-insensitive)");

    // the hub caches everything; then alice goes offline and bob still downloads, verified
    CHECK(waitFor([&] { return hub->snap()["counters"]["fetched"].get<int>() >= 3; }, 8000), "hub cached the STL, thumbnail and fingerprint");
    alice->store.online = false; alice->bus.online = false;
    bob->call(bob->core.download(mid, "1"));
    std::string dir;
    CHECK(waitFor([&] { json m = json::parse(bob->core.getModel(mid))["model"]; json dl = m["versions"][0].value("download", json::object()); dir = dl.value("dir", ""); return dl.value("status", "") == "done"; }, 8000),
          "bob downloads from the hub while the creator is offline");
    std::string a, b;
    { std::ifstream f1(stl1, std::ios::binary), f2(dir + "/torus.stl", std::ios::binary); std::stringstream s1, s2; s1 << f1.rdbuf(); s2 << f2.rdbuf(); a = s1.str(); b = s2.str(); }
    CHECK(!a.empty() && a == b, "downloaded file is byte-identical to the original");
    alice->store.online = true; alice->bus.online = true;

    // community
    bob->call(bob->core.comment(mid, "Printed in PETG, fits well"));
    bob->call(bob->core.like(mid, "true"));
    std::string photo = json::parse(bob->core.listModels("{}"))["models"][0]["thumb"];
    bob->call(bob->core.postMake(mid, json{{"text", "my make"}, {"v", 1}, {"images", {photo}}}.dump()));
    CHECK(waitFor([&] { json m = json::parse(alice->core.getModel(mid))["model"]; return m["comments"].size() == 1 && m["likes"] == 1 && m["makes"] == 1; }),
          "alice sees bob's comment, like and make");

    // bob can't version alice's model; alice can
    json bad = json::parse(bob->core.publish(json{{"modelId", mid}, {"title", "hijack"}, {"licence", "CC0-1.0"}, {"files", {{{"path", stl2}}}}}.dump()));
    CHECK(!bad.value("ok", true) && bad.value("error", "").find("remix") != std::string::npos, "non-creator gets a clear 'remix it instead' error");
    alice->call(alice->core.publish(json{{"modelId", mid}, {"title", "Geometric bracelet (thicker)"}, {"licence", "CC-BY-4.0"}, {"files", {{{"path", stl2}}}}}.dump()));
    CHECK(waitFor([&] { json m = json::parse(bob->core.getModel(mid))["model"]; return m["latest"] == 2 && m["title"] == "Geometric bracelet (thicker)"; }), "v2 arrives");

    // bob remixes
    json rmx = bob->call(bob->core.publish(json{{"title", "Bracelet with clasp"}, {"licence", "CC-BY-4.0"}, {"parents", {{{"modelId", mid}, {"v", 1}}}}, {"files", {{{"path", stl1}}}}}.dump()));
    CHECK(waitFor([&] { return json::parse(alice->core.getModel(mid))["model"]["remixes"].size() == 1; }), "alice's model lists bob's remix");
    CHECK(json::parse(alice->core.listModels("{}"))["models"].size() == 2, "two models in the catalogue");
    {   // a remix's page names and links its parent; a new version can carry files over by hash
        json rm = json::parse(alice->core.getModel(rmx.value("modelId", "")))["model"];
        json par = rm["versions"][0]["parents"][0];
        CHECK(par.value("title", "") == "Geometric bracelet" && par.value("creatorName", "") == "Alice", "remix parent comes with its title and creator");
        json v2 = json::parse(alice->core.getModel(mid))["model"]["versions"][1]["files"][0];
        json keep = json::parse(alice->core.publish(json{{"modelId", mid}, {"title", "Geometric bracelet (v3)"}, {"licence", "CC-BY-4.0"},
            {"files", {{{"sha256", v2["sha256"]}, {"name", v2["name"]}}, {{"path", stl1}}}}}.dump()));
        json v3 = json::parse(alice->core.getModel(mid))["model"]["versions"][2];
        CHECK(keep.value("ok", false) && v3["files"].size() == 2 && v3["files"][0]["name"] == v2["name"] && v3["files"][0]["sha256"] == v2["sha256"], "new version keeps a file carried over by hash");
        json missing = json::parse(alice->core.publish(json{{"modelId", mid}, {"title", "x"}, {"licence", "CC-BY-4.0"},
            {"files", {{{"sha256", std::string(64, 'a')}, {"name", "gone.stl"}}}}}.dump()));
        CHECK(!missing.value("ok", true), "carrying over a file the node doesn't hold is refused");
    }

    // a bogus CID (served bytes don't match the hash) is rejected, never saved
    Peer* mallory = spawn("mallory");
    pump(1300);
    {
        std::string fakeBytes = "this is not the model";
        std::string realSha = json::parse(alice->core.getModel(mid))["model"]["versions"][1]["files"][0]["sha256"];
        // mallory holds unrelated bytes and claims they are v2's STL
        std::string fakeCid = "zFakeBOGUS";
        mallory->store.held[fakeCid] = fakeBytes;
        // inject via mallory's own author path: a profile-less raw event
        swamp::Identity mid2 = swamp::identityFrom(logos_sync::generatePrivateKey());
        swamp::Event ev = swamp::makeEvent(mid2, "blob.cids", json{{"cids", {{realSha, fakeCid}}}}, 1, "bogus1");
        for (auto* n : FakeLoamBus::get().nodes) if (n->onRecv && n != &mallory->bus)
            n->onRecv(swamp::categoryTopic("other"), "x", FakeLoamBus::b64(json{{"t", "ev"}, {"e", logos_sync::eventToJson(ev)}}.dump()), 0);
        pump(200);
        Peer* carol = spawn("carol");
        pump(1300);
        carol->call(carol->core.resync());
        CHECK(waitFor([&] { return json::parse(carol->core.listModels("{}"))["total"] == 2; }, 8000), "late joiner catches up the whole catalogue");
        // the creator's CID is tried first; with every honest holder offline carol falls through
        // to mallory's CID, gets the wrong bytes, and must throw them away
        alice->store.online = false; hub->store.online = false;
        carol->call(carol->core.download(mid, "2"));
        CHECK(waitFor([&] { return carol->snap()["counters"]["verifyFailed"].get<int>() >= 1; }, 5000), "the bogus bytes were detected and thrown away");
        json f2 = json::parse(carol->core.getModel(mid))["model"]["versions"][1]["files"][0];
        CHECK(f2["local"].is_null() && f2.value("fetchError", "") != "", "nothing saved; the file shows why it isn't there yet");
        CHECK(json::parse(carol->core.getModel(mid))["model"]["versions"][1]["download"]["status"] == "fetching", "the download keeps retrying (back-off), not failed");
        alice->store.online = true; hub->store.online = true;
        carol->call(carol->core.download(mid, "2"));   // asking again skips the back-off
        CHECK(waitFor([&] { json m = json::parse(carol->core.getModel(mid))["model"]; return m["versions"][1].value("download", json::object()).value("status", "") == "done"; }, 10000),
              "carol gets v2 once an honest holder is back");
    }

    // hand-off to a desktop slicer: one click downloads (verified) and opens the model files only
    {
        std::string fake = root + "/fake-slicer.sh", log = root + "/slicer-args.txt";
        { std::ofstream f(fake); f << "#!/bin/sh\nfor a in \"$@\"; do echo \"$a\"; done > " << log << "\n"; }
        fs::permissions(fake, fs::perms::owner_all);
        setenv("SWAMP_SLICER", fake.c_str(), 1);
        json sn = bob->snap();
        CHECK(sn["slicer"].is_object() && sn["slicer"]["program"] == fake, "the slicer is detected (SWAMP_SLICER)");
        json r = json::parse(bob->core.openInSlicer(mid, "1"));
        CHECK(r.value("ok", false), "openInSlicer accepts (downloads first if needed)");
        CHECK(waitFor([&] { return fs::exists(log) && fs::file_size(log) > 0; }, 15000), "the slicer is started once the files are verified");
        std::ifstream lf(log); std::string line, all; while (std::getline(lf, line)) all += line + "\n";
        CHECK(all.find("torus.stl") != std::string::npos, "it gets the downloaded model file");
        unsetenv("SWAMP_SLICER");
    }

    // print on a (fake) Bambu Lab A1 over the LAN: find, configure, slice with its profile, confirm,
    // upload over FTPS, start over MQTT (bambu_mock.py, started by run-tests.sh)
    if (getenv("SWAMP_MOCK_PRINTER")) {
        std::string mockDir = getenv("SWAMP_MOCK_PRINTER");
        std::string here = getenv("SWAMP_TEST_DIR") ? getenv("SWAMP_TEST_DIR") : ".";
        fs::create_directories(root + "/orca-profiles/BBL/machine");
        const char* realOrca = getenv("SWAMP_TEST_REAL_ORCA");   // the real OrcaSlicer AppImage, if given
        setenv("SWAMP_ORCA", realOrca ? realOrca : (here + "/fake_orca.sh").c_str(), 1);
        if (!realOrca) setenv("SWAMP_ORCA_PROFILES", (root + "/orca-profiles/BBL").c_str(), 1);
        json fp = json::parse(bob->core.findPrinters());
        CHECK(fp.value("running", false), "finding printers starts listening");
        json found;
        CHECK(waitFor([&] { found = json::parse(bob->core.findPrinters()); return !found.value("running", true) && found["printers"].size() == 1; }, 12000),
              "the printer is found on the LAN (" + found.dump() + ")");
        json pr = found["printers"][0];
        CHECK(pr.value("serial", "") == "03919A3B0000001" && pr.value("model", "") == "N2S", "...with its serial and model");
        CHECK(!json::parse(bob->core.setPrinter(json{{"ip", "127.0.0.1"}, {"serial", pr["serial"]}}.dump())).value("ok", true), "an access code is required");
        json sp = json::parse(bob->core.setPrinter(json{{"ip", "127.0.0.1"}, {"serial", pr["serial"]}, {"accessCode", "12345678"}, {"model", pr["model"]},
                                                         {"mqttPort", atoi(getenv("SWAMP_MOCK_MQTT"))}, {"ftpsPort", atoi(getenv("SWAMP_MOCK_FTPS"))}}.dump()));
        CHECK(sp.value("ok", false) && sp["printer"].value("name", "") == "Bambu Lab A1" && !sp["printer"].contains("accessCode"), "the printer is saved; the access code is never handed back");
        CHECK((fs::status(root + "/bob/data/printer.json").permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none, "printer.json is owner-only");
        CHECK(waitFor([&] { json st = json::parse(bob->core.printerStatus()); return st["printer"]["state"].is_object() && !st["printer"]["state"].value("state", "").empty(); }, 10000),
              "the printer reports its state");
        json pp = json::parse(bob->core.preparePrint(mid, "1"));
        CHECK(pp.value("ok", false), "prepare: accepted");
        json job;
        CHECK(waitFor([&] { job = bob->snap()["printJob"]; return job.is_object() && (job["stage"] == "ready" || job["stage"] == "failed"); }, 180000) && job["stage"] == "ready",
              "prepare: downloaded, verified and sliced (" + job.dump() + ")");
        CHECK(realOrca ? !job["estimate"].value("time", "").empty() : (job["estimate"]["time"] == "1h 5m 3s" && job["estimate"]["layers"] == 42),
              "the estimate comes from the slicer (" + job["estimate"].dump() + ")");
        CHECK(job["estimate"].value("filamentG", 0) > 0, "...with the filament it needs");
        if (!realOrca) {
            std::string args; { std::ifstream f(here + "/fake-orca-args.txt"); std::stringstream ss; ss << f.rdbuf(); args = ss.str(); }
            CHECK(args.find("Bambu Lab A1 0.4 nozzle.json") != std::string::npos && args.find("0.20mm Standard @BBL A1.json") != std::string::npos &&
                  args.find("Bambu PLA Basic @BBL A1.json") != std::string::npos, "sliced with the A1's default printer, process and PLA profiles");
        }
        CHECK(!json::parse(bob->core.startPrint("no")).value("ok", true), "nothing is sent without an explicit confirmation");
        CHECK(json::parse(bob->core.startPrint("yes")).value("ok", false), "confirmed: sending");
        CHECK(waitFor([&] { job = bob->snap()["printJob"]; return job["stage"] == "sent" || job["stage"] == "failed"; }, 20000) && job["stage"] == "sent",
              "the printer accepted the job (" + job.dump() + ")");
        std::string remote = job.value("remote", "");
        CHECK(fs::exists(mockDir + "/sd/" + remote), "the sliced file is on the printer's SD card");
        CHECK(waitFor([&] { json st = json::parse(bob->core.printerStatus()); return st["printer"]["state"].is_object() && st["printer"]["state"]["state"] == "PREPARE"; }, 15000),
              "...and the printer moved to PREPARE");
        // a slicer that can't start here: the job says what's wrong and offers to fix it, in plain steps
        bob->call(bob->core.cancelPrint());
        setenv("SWAMP_ORCA", (here + "/broken_orca.sh").c_str(), 1);
        CHECK(json::parse(bob->core.preparePrint(mid, "1")).value("ok", false), "prepare with a broken slicer: accepted");
        CHECK(waitFor([&] { job = bob->snap()["printJob"]; return job.is_object() && (job["stage"] == "ready" || job["stage"] == "failed"); }, 60000) && job["stage"] == "failed",
              "...and it fails (" + job.dump() + ")");
        json fix = job.value("fix", json());
        CHECK(fix.is_object() && fix.value("title", "") == "This OrcaSlicer is built for older Linux" && fix.value("action", "") == "installSlicer",
              "...saying the slicer is built for older Linux and offering to set up the one that works (" + fix.dump() + ")");
        bool hasCmd = false; for (const auto& st : fix.value("steps", json::array())) hasCmd = hasCmd || st.value("command", "").find("curl -L") != std::string::npos;
        CHECK(hasCmd, "...with the manual way as a command to copy");
        CHECK(job.value("log", "").find("libwebkit2gtk-4.0.so.37") != std::string::npos, "...and the slicer's own output to copy");
        bob->call(bob->core.cancelPrint());
        unsetenv("SWAMP_ORCA"); unsetenv("SWAMP_ORCA_PROFILES");
        // the real thing: download (~140 MB), check, unpack and start OrcaSlicer 2.4.2 (opt-in: it's slow)
        if (getenv("SWAMP_TEST_INSTALL_SLICER")) {
            CHECK(json::parse(bob->core.installSlicer()).value("ok", false), "installSlicer: started");
            json in;
            CHECK(waitFor([&] { in = bob->snap()["slicerInstall"]; return in["stage"] == "done" || in["stage"] == "failed"; }, 15 * 60 * 1000) && in["stage"] == "done",
                  "OrcaSlicer 2.4.2 is downloaded, hash-checked, unpacked and starts (" + in.dump() + ")");
            CHECK(fs::exists(root + "/bob/data/slicers/orca-2.4.2/squashfs-root/AppRun"), "...unpacked into Swamp's own data (no FUSE needed)");
            pump(100);
            json ps = bob->snap()["printSlicer"];
            CHECK(ps.is_object() && ps.value("managed", false), "...and printing uses it from now on (" + ps.dump() + ")");
        }
    }

    // malformed and hostile frames: dropped and counted, the node keeps working (review C2)
    {
        long badBefore = bob->snap()["counters"]["rxBad"].get<long>();
        auto inject = [&](const json& f) { bob->bus.onRecv(swamp::categoryTopic("other"), "x", FakeLoamBus::b64(f.dump()), 0); };
        inject(json{{"t", "fp"}, {"from", "zz"}, {"fps", {"a", "b"}}});                  // bounds missing
        inject(json{{"t", "fp"}, {"from", "zz"}, {"fps", {1, 2}}, {"bounds", {3}}});     // wrong types
        inject(json{{"t", "ids"}, {"from", "zz"}, {"ids", "notalist"}});
        inject(json{{"t", "need"}, {"from", "zz"}, {"ids", {{{"x", 1}}}}});
        inject(json{{"t", "ev"}, {"e", "garbage"}});
        inject(json{{"t", "evs"}, {"es", {1, "two", json::object()}}});
        bob->bus.onRecv(swamp::categoryTopic("other"), "x", "!!!not base64 or json", 0);
        pump(300);
        CHECK(bob->snap()["counters"]["rxBad"].get<long>() - badBefore >= 7, "malformed frames are counted, not crashed on");
        CHECK(json::parse(bob->core.listModels("{}"))["total"] == 2, "catalogue intact after hostile frames");
    }

    // CIDs are announced in batches, and a known CID is never re-announced (review H4)
    {
        std::string aliceAddr0 = alice->snap()["me"]["address"];
        auto countCids = [&] {
            std::ifstream f(root + "/alice/data/catalog.json");
            json a = json::parse(std::string(std::istreambuf_iterator<char>(f), {}), nullptr, false);
            int n = 0;
            if (a.is_array()) for (const auto& e : a) if (e.value("type", "") == "blob.cids" && e.value("dev", "") == aliceAddr0) n++;
            return n;
        };
        pump(2200);   // let the save throttle flush
        int before = countCids();
        pump(1500);
        CHECK(countCids() == before, "no blob.cids re-announced while the catalogue already has them");
        CHECK(before <= 4, "alice's CIDs went out in a few batched events (" + std::to_string(before) + ")");
    }

    // a draft that fails validation leaves no orphan model behind (review M7)
    {
        size_t n0 = alice->snap()["catalog"]["events"];
        json bad2 = json::parse(alice->core.publish(json{{"title", "x"}, {"licence", "CC0-1.0"}, {"tags", "not-a-list"}, {"files", {{{"path", stl1}}}}}.dump()));
        CHECK(!bad2.value("ok", true) && alice->snap()["catalog"]["events"] == n0, "invalid draft: refused, nothing authored");
    }
    CHECK((fs::status(root + "/alice/data/identity.json").permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none,
          "identity key file is owner-only (review M8)");

    // restart: alice's identity, catalogue and her CIDs survive
    pump(2200);   // received events are saved on a short throttle
    std::string aliceAddr = alice->snap()["me"]["address"];
    setenv("SWAMP_CORE_DATA", (root + "/alice/data").c_str(), 1);
    SwampCoreImpl again; FakeLoamNode nb; FakeStoreNode ns; nb.name = ns.name = "alice2";
    again.modules().loam_core.node = &nb; again.modules().storage_module.node = &ns;
    again.fakeStart();
    pump(100);   // the stored log loads just after onContextReady
    json s2 = json::parse(again.snapshot());
    CHECK(s2["me"]["address"] == aliceAddr && s2["catalog"]["models"] == 2, "restart keeps identity and catalogue");

        // categories (ADR 0014): a node mirrors only the categories it subscribes to
    {
        json toy = alice->call(alice->core.publish(json{{"title", "Spinning top"}, {"licence", "CC0-1.0"}, {"category", "toys"}, {"files", {{{"path", stl1}}}}}.dump()));
        json tool = alice->call(alice->core.publish(json{{"title", "Wrench holder"}, {"licence", "CC0-1.0"}, {"category", "tools"}, {"files", {{{"path", stl2}}}}}.dump()));
        CHECK(json::parse(alice->core.publish(json{{"title", "x"}, {"licence", "CC0-1.0"}, {"category", "spaceships"}, {"files", {{{"path", stl1}}}}}.dump())).value("ok", true) == false,
              "an unknown category is refused");
        CHECK(waitFor([&] { return json::parse(bob->core.listModels(json{{"category", "tools"}}.dump()))["total"] == 1 &&
                                   json::parse(bob->core.listModels(json{{"category", "toys"}}.dump()))["total"] == 1; }, 5000), "a node subscribed to everything gets both");
        setenv("SWAMP_CATEGORIES", "tools", 1);
        Peer* tess = spawn("tess");
        pump(1300);   // her stored settings/env are read just after onContextReady
        unsetenv("SWAMP_CATEGORIES");
        tess->call(tess->core.resync());
        CHECK(waitFor([&] { return json::parse(tess->core.listModels("{}"))["total"] == 1; }, 8000), "a Tools-only node catches up the Tools model...");
        pump(1500);
        json tl = json::parse(tess->core.listModels("{}"));
        CHECK(tl["total"] == 1 && tl["models"][0]["title"] == "Wrench holder", "...and nothing from other categories");
        size_t tessEvents = tess->snap()["catalog"]["events"], bobEvents = bob->snap()["catalog"]["events"];
        CHECK(tessEvents < bobEvents, "the Tools-only node holds a fraction of the catalogue (" + std::to_string(tessEvents) + " of " + std::to_string(bobEvents) + " events)");
        json subs = json::parse(tess->core.snapshot())["categories"];
        int n = 0; for (const auto& c : subs) n += c.value("subscribed", false);
        CHECK(n == 1, "it is subscribed to exactly one category");
        // publishing in a category subscribes you to it
        tess->call(tess->core.publish(json{{"title", "Tess's balloon"}, {"licence", "CC0-1.0"}, {"category", "toys"}, {"files", {{{"path", stl1}}}}}.dump()));
        CHECK(waitFor([&] { return json::parse(tess->core.listModels(json{{"category", "toys"}}.dump()))["total"].get<int>() >= 2; }, 8000),
              "publishing in Toys subscribed Tess to Toys (she now sees Alice's top too)");
        CHECK(!json::parse(tess->core.setCategories("[\"nope\"]")).value("ok", true), "setCategories refuses an unknown category");
        json after = json::parse(tess->core.setCategories("[\"garden\"]"));
        int toys = 0; for (const auto& c : after["categories"]) if (c["id"] == "toys") toys = c.value("subscribed", false);
        CHECK(after.value("ok", false) && toys == 1, "categories you publish in stay subscribed");
        // global search (ADR 0015): Tess follows Tools (and now Toys), not Fashion - the hub's
        // index still finds Alice's bracelet, and opening it pulls just that model's record
        CHECK(waitFor([&] { return hub->snap()["index"]["built"].get<int>() >= 1; }, 8000), "the hub built and published an index");
        CHECK(waitFor([&] { return tess->snap()["index"]["known"].get<int>() >= 1; }, 8000), "Tess received the hub's index manifest");
        json gs;
        bool sawPending = false;
        CHECK(waitFor([&] {
            gs = json::parse(tess->core.globalSearch(json{{"q", "bracelet"}}.dump()));
            if (gs.value("pending", false)) sawPending = true;
            return !gs.value("pending", true) && gs["results"].size() >= 1; }, 15000), "global search finds a model in a category Tess doesn't follow");
        CHECK(sawPending, "...after fetching the term shard it needed (pending first)");
        std::string found;
        for (const auto& r : gs["results"]) if (r["modelId"] == mid) found = mid;
        CHECK(!found.empty() && gs["index"]["agreeing"] == 1 && gs["index"]["indexers"] == 1, "the result names the index it came from");
        CHECK(json::parse(tess->core.listModels(json{{"q", "bracelet"}}.dump()))["total"] == 0, "it isn't in Tess's own catalogue");
        std::string vdirT = root + "/plugins-tess/swamp"; fs::create_directories(vdirT);
        std::string tsha;
        for (const auto& r : gs["results"]) if (r["modelId"] == found) tsha = r.value("thumbSha", "");
        tess->core.cacheImage(tsha, vdirT);
        CHECK(waitFor([&] { return json::parse(tess->core.cacheImage(tsha, vdirT)).value("ok", false); }, 8000), "a remote result's thumbnail loads (CIDs from the index, hash-verified)");
        json gm;
        CHECK(waitFor([&] { gm = json::parse(tess->core.getModel(found)); return gm.value("ok", false); }, 15000), "opening it fetches the model's record from the index");
        CHECK(gm["model"].value("title", "").rfind("Geometric bracelet", 0) == 0 && gm["model"]["comments"].size() >= 1 && gm["model"]["creatorName"] == "Alice",
              "...with its comments and its creator's name");
        CHECK(json::parse(tess->core.listModels("{}"))["total"].get<int>() == 2, "opened models don't join Browse (Tess still sees only her categories)");
        tess->call(tess->core.download(found, "1"));
        CHECK(waitFor([&] { json m = json::parse(tess->core.getModel(found))["model"]; return m["versions"][0].value("download", json::object()).value("status", "") == "done"; }, 15000),
              "...and she can download a model she found through the index");
        // ADR 0016: a declared exclusion is a policy; a silent omission is caught by the creator
        {
            std::string toyId = toy.value("modelId", ""), toolId = tool.value("modelId", "");
            fs::create_directories(root + "/policy/data");
            { std::ofstream f(root + "/policy/data/exclusions.json"); f << json{{toolId, "we only carry toys"}}.dump(); }
            setenv("SWAMP_INDEXER", "1", 1);
            Peer* policy = spawn("policy");
            setenv("SWAMP_TEST_OMIT", toyId.c_str(), 1);
            Peer* rogue = spawn("rogue");
            pump(1300);
            unsetenv("SWAMP_TEST_OMIT"); unsetenv("SWAMP_INDEXER");
            policy->call(policy->core.setProfile(json{{"name", "Policy hub"}}.dump()));
            rogue->call(rogue->core.setProfile(json{{"name", "Rogue hub"}}.dump()));
            policy->call(policy->core.resync()); rogue->call(rogue->core.resync());
            std::string rogueAddr = rogue->snap()["me"]["address"];
            CHECK(waitFor([&] { return alice->snap()["index"]["known"].get<int>() >= 3; }, 15000), "Alice sees three indexers' manifests");
            // first only a suspect (maybe it never got the model): Alice re-sends it...
            CHECK(waitFor([&] {
                for (const auto& o : json(alice->snap()["index"]["suspects"])) if (o["indexer"] == rogueAddr && o["modelId"] == toyId) return true;
                return false; }, 40000), "a missing model first makes the indexer a suspect, and Alice re-sends it");
            json early = json(alice->snap()["index"]["omissions"]);
            bool notYet = true; for (const auto& o : early) if (o["indexer"] == rogueAddr) notYet = false;
            CHECK(notYet, "...not yet accused");
            // ...the rogue builds a newer index (the catalogue changed) and still leaves it out: caught
            // keep the catalogue changing so every indexer rebuilds past the confirmation window
            QElapsedTimer nudge; nudge.start(); int n = 0;
            CHECK(waitFor([&] {
                if (nudge.elapsed() > 2500) { nudge.restart(); bob->core.comment(toyId, "nudge " + std::to_string(++n)); }
                for (const auto& o : json(alice->snap()["index"]["omissions"])) if (o["indexer"] == rogueAddr && o["modelId"] == toyId) return true;
                return false; }, 90000), "a newer index that still leaves it out gets the rogue caught (signed evidence)");
            json ev;
            for (const auto& o : json(alice->snap()["index"]["omissions"])) if (o["indexer"] == rogueAddr) ev = o;
            CHECK(ev.is_object() && ev.value("manifestEvent", "").size() > 0 && (ev.value("shardSha256", "").size() == 64 || ev.value("shardMissing", false)),
                  "the evidence names the signed manifest and the shard (or that the manifest has no such shard)");
            bool policyNotBlamed = true;
            for (const auto& o : json(alice->snap()["index"]["omissions"])) if (o["indexer"] == policy->snap()["me"]["address"]) policyNotBlamed = false;
            CHECK(policyNotBlamed, "the policy hub's declared exclusion is not counted as an omission");
            // the rogue only drops the toy: anything else it was late to include isn't held against it
            CHECK(waitFor([&] {
                for (const auto& o : json(alice->snap()["index"]["omissions"])) if (o["indexer"] == rogueAddr && o["modelId"] != toyId) return false;
                return true; }, 40000), "no lasting accusation for a model the indexer includes once it has caught up (" + json(alice->snap()["index"]["omissions"]).dump() + ")");
            CHECK(waitFor([&] {
                for (const auto& x : json(alice->snap()["index"]["excludedMine"])) if (x["modelId"] == toolId && x["why"] == "we only carry toys") return true;
                return false; }, 40000), "...and Alice is told which hub excludes her model, and why");
            json gsA = json::parse(alice->core.globalSearch(json{{"q", "spinning"}}.dump()));
            CHECK(gsA["index"]["indexer"] != rogueAddr, "Alice's searches no longer use the rogue indexer");
        }
        sharedToolId = mid;   // the bracelet (Fashion): no test indexer leaves it out
        (void)toy; (void)tool;
    }

    // a node whose 'Connected' event is lost still comes up, by polling loam_core (review M5)
    {
        setenv("SWAMP_CORE_DATA", (root + "/dave/data").c_str(), 1);
        Peer* dave = new Peer(); dave->name = dave->bus.name = dave->store.name = "dave";
        dave->bus.dropStatusEvent = true;
        dave->core.modules().loam_core.node = &dave->bus; dave->core.modules().storage_module.node = &dave->store;
        FakeLoamBus::get().nodes.push_back(&dave->bus); FakeStoreNet::get().nodes.push_back(&dave->store);
        dave->core.fakeStart();
        CHECK(waitFor([&] { return dave->snap()["status"] == "Connected"; }, 4000), "lost Connected event: status poll brings the node up");
        CHECK(waitFor([&] { return json::parse(dave->core.listModels("{}"))["total"].get<int>() >= 2; }, 20000), "...and it catches up (has " << json::parse(dave->core.listModels("{}"))["total"] << ")");
    }

    // share links: a node that follows neither the model's category nor has the model opens it from a link
    {
        std::string sharedId = sharedToolId;
        setenv("SWAMP_CATEGORIES", "garden", 1);
        unsetenv("SWAMP_EXPERIMENTAL_PRINT");
        Peer* lena = spawn("lena");
        pump(1300);
        unsetenv("SWAMP_CATEGORIES");
        CHECK(!json::parse(lena->core.openLink("https://example.com/x")).value("ok", true), "a non-Swamp link is refused");
        CHECK(!json::parse(lena->core.openLink("swamp://model/zz12")).value("ok", true), "a damaged link is refused");
        // the Share button's text: the link first, then the title - which anyone can choose, links and all
        json ol = json::parse(lena->core.openLink("swamp://model/" + sharedId + "?c=other  Free stuff swamp://model/eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee by Alice, on Swamp\n"));
        CHECK(ol.value("ok", false) && ol["modelId"] == sharedId && ol["category"] == "other", "a shared message opens to its model id and category, not to a link planted in the title");
        json lm;
        CHECK(waitFor([&] { lm = json::parse(lena->core.getModel(sharedId)); return lm.value("ok", false); }, 15000), "...and the model opens on a node that doesn't follow its category");
        CHECK(lm.contains("model") && lm["model"].value("modelId", "") == sharedId, "...it's the shared model (" + (lm.contains("model") ? lm["model"].value("title", "") : lm.dump()) + ")");
        CHECK(json::parse(lena->core.listModels("{}"))["total"].get<int>() == 0, "...without adding that category to her Browse");
        // direct printing is off unless asked for (it drove a real A1's head into its frame, 2026-10-08)
        json np = json::parse(lena->core.preparePrint(sharedId, "1"));
        CHECK(!np.value("ok", true) && np.value("error", "").find("switched off") != std::string::npos && lena->snap()["experimentalPrint"] == false,
              "direct printing is switched off by default (" + np.dump() + ")");
        CHECK(!json::parse(lena->core.findPrinters()).value("ok", true) && !json::parse(lena->core.startPrint("yes")).value("ok", true), "...including finding printers and starting a print");
        auto& bn = FakeLoamBus::get().nodes; bn.erase(std::remove(bn.begin(), bn.end(), &lena->bus), bn.end());
        auto& sn = FakeStoreNet::get().nodes; sn.erase(std::remove(sn.begin(), sn.end(), &lena->store), sn.end());
    }
    std::cout << passes << " passed, " << fails << " failed\n";
    return fails ? 1 : 0;
}

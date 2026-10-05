// End-to-end over the fake SDK: real SwampCoreImpl instances (creator, maker, hub, late joiner,
// attacker) publish, browse, fetch through a hub after the creator goes offline, verify hashes,
// comment, like, post makes, version, and catch up. Run: swamp_core/test/run-tests.sh
#include "swamp_core_impl.h"
#include "logos_sdk.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <iostream>
#include <filesystem>

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
    QCoreApplication app(argc, argv);
    std::string repo = argc > 1 ? argv[1] : ".";
    root = std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") + "/swamp-e2e";
    fs::remove_all(root);
    setenv("SWAMP_TICK_MS", "60", 1);
    const std::string stl1 = repo + "/bench/data/raw/74890.stl", stl2 = repo + "/bench/data/raw/278455.stl";

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
    CHECK(waitFor([&] { return !json::parse(bob->core.listModels("{}"))["models"][0]["thumb"].is_null(); }), "bob fetched the thumbnail eagerly");
    json model = json::parse(bob->core.getModel(mid))["model"];
    json v1 = model["versions"][0];
    CHECK(v1["files"][0]["name"] == "74890.stl" && v1["images"][0]["kind"] == "thumb", "version lists the STL and an auto thumbnail");
    CHECK(!v1.contains("fp") || v1["fp"].is_null(), "getModel hides the fingerprint blob from the UI");
    CHECK(json::parse(alice->core.listModels(json{{"tag", "Bracelet"}}.dump()))["models"].size() == 1, "tag filter (case-insensitive)");

    // the hub caches everything; then alice goes offline and bob still downloads, verified
    CHECK(waitFor([&] { return hub->snap()["counters"]["fetched"].get<int>() >= 3; }, 8000), "hub cached the STL, thumbnail and fingerprint");
    alice->store.online = false; alice->bus.online = false;
    bob->call(bob->core.download(mid, "1"));
    std::string dir;
    CHECK(waitFor([&] { json m = json::parse(bob->core.getModel(mid))["model"]; json dl = m["versions"][0].value("download", json::object()); dir = dl.value("dir", ""); return dl.value("status", "") == "done"; }, 8000),
          "bob downloads from the hub while the creator is offline");
    std::string a, b;
    { std::ifstream f1(stl1, std::ios::binary), f2(dir + "/74890.stl", std::ios::binary); std::stringstream s1, s2; s1 << f1.rdbuf(); s2 << f2.rdbuf(); a = s1.str(); b = s2.str(); }
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
            n->onRecv(swamp::CATALOG_TOPIC, "x", FakeLoamBus::b64(json{{"t", "ev"}, {"e", logos_sync::eventToJson(ev)}}.dump()), 0);
        pump(200);
        // the bogus CID sorts first (older HLC); carol (no local copy) must still end up with the right bytes
        Peer* carol = spawn("carol");
        pump(1300);
        carol->call(carol->core.resync());
        CHECK(waitFor([&] { return json::parse(carol->core.listModels("{}"))["total"] == 2; }, 8000), "late joiner catches up the whole catalogue");
        carol->call(carol->core.download(mid, "2"));
        CHECK(waitFor([&] { json m = json::parse(carol->core.getModel(mid))["model"]; return m["versions"][1].value("download", json::object()).value("status", "") == "done"; }, 10000),
              "carol gets v2 despite a bogus first CID");
        CHECK(carol->snap()["counters"]["verifyFailed"].get<int>() >= 1, "the bogus bytes were detected and thrown away");
    }

    // restart: alice's identity, catalogue and her CIDs survive
    std::string aliceAddr = alice->snap()["me"]["address"];
    setenv("SWAMP_CORE_DATA", (root + "/alice/data").c_str(), 1);
    SwampCoreImpl again; FakeLoamNode nb; FakeStoreNode ns; nb.name = ns.name = "alice2";
    again.modules().loam_core.node = &nb; again.modules().storage_module.node = &ns;
    again.fakeStart();
    json s2 = json::parse(again.snapshot());
    CHECK(s2["me"]["address"] == aliceAddr && s2["catalog"]["models"] == 2, "restart keeps identity and catalogue");

    std::cout << passes << " passed, " << fails << " failed\n";
    return fails ? 1 : 0;
}

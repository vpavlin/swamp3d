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
    QCoreApplication app(argc, argv);
    std::string repo = argc > 1 ? argv[1] : ".";
    root = std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") + "/swamp-e2e";
    fs::remove_all(root);
    setenv("SWAMP_TICK_MS", "60", 1);
    setenv("SWAMP_INDEX_EVERY_MS", "1000", 1);   // the hub indexes every second here
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
    CHECK(v1["files"][0]["name"] == "74890.stl" && v1["images"][0]["kind"] == "thumb", "version lists the STL and an auto thumbnail");
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
        CHECK(waitFor([&] { return json::parse(dave->core.listModels("{}"))["total"].get<int>() >= 2; }, 8000), "...and it catches up");
    }

    std::cout << passes << " passed, " << fails << " failed\n";
    return fails ? 1 : 0;
}

// Catalogue fold rules (docs/SPEC.md section 3): creator-only history, immutable versions,
// squatting-proof ids, forged/oversized events, CID candidates, order independence.
#include "swamp_catalog.hpp"
#include <iostream>
#include <random>
using namespace swamp;
static int fails = 0, passes = 0;
#define CHECK(c, w) do { if (c) passes++; else { fails++; std::cerr << "FAIL: " << w << "\n"; } } while (0)

static long long T = 1790000000000;
static int seq = 0;
static std::string eid() { return "e" + std::to_string(++seq); }
static Identity who() { return identityFrom(logos_sync::generatePrivateKey()); }

static json version(const std::string& id, int v, const std::string& title) {
    return json{{"modelId", id}, {"v", v}, {"title", title}, {"licence", "CC-BY-4.0"}, {"tags", {"benchy", "test"}},
                {"files", {{{"name", "part.stl"}, {"kind", "model"}, {"size", 123}, {"sha256", sha256Hex(title)}}}}};
}

int main() {
    Identity alice = who(), bob = who();
    std::vector<Event> log;
    std::string id = modelIdFor(alice.address, "n1");
    log.push_back(makeEvent(alice, "model.create", {{"modelId", id}, {"nonce", "n1"}, {"title", "Mule"}}, T++, eid()));
    log.push_back(makeEvent(alice, "model.version", version(id, 1, "Mule v1"), T++, eid()));
    log.push_back(makeEvent(alice, "model.version", version(id, 2, "Mule v2"), T++, eid()));
    Catalog c = fold(log);
    CHECK(c.models.size() == 1 && c.models[id].versions.size() == 2, "two versions accepted");
    CHECK(c.models[id].versions.back().value("title", "") == "Mule v2", "latest version is v2");

    // bob can't add versions, retract, or squat the id
    std::vector<Event> l2 = log;
    l2.push_back(makeEvent(bob, "model.version", version(id, 3, "Mule hijacked"), T++, eid()));
    l2.push_back(makeEvent(bob, "model.retract", {{"modelId", id}}, T++, eid()));
    l2.push_back(makeEvent(bob, "model.create", {{"modelId", id}, {"nonce", "n1"}, {"title", "Mine now"}}, T++, eid()));
    Catalog c2 = fold(l2);
    CHECK(c2.models[id].versions.size() == 2 && !c2.models[id].retracted && c2.models[id].creator == alice.address, "non-creator can't extend, retract or squat");

    // versions are immutable: a second v2 is ignored; a gap (v4) is ignored
    l2.push_back(makeEvent(alice, "model.version", version(id, 2, "Mule v2 rewritten"), T++, eid()));
    l2.push_back(makeEvent(alice, "model.version", version(id, 4, "Mule v4 gap"), T++, eid()));
    c2 = fold(l2);
    CHECK(c2.models[id].versions.size() == 2 && c2.models[id].versions[1].value("title", "") == "Mule v2", "v2 can't be rewritten, gaps refused");

    // forged event (payload changed after signing) is dropped
    Event forged = log[1];
    forged.payload["title"] = "Forged";
    forged.id = "forged";
    std::vector<Event> l3 = {log[0], forged};
    CHECK(fold(l3).models[id].versions.empty(), "forged version dropped");

    // invalid version content refused
    json bad = version(id, 3, "x"); bad["files"][0]["name"] = "../../etc/passwd";
    l2.push_back(makeEvent(alice, "model.version", bad, T++, eid()));
    CHECK(fold(l2).models[id].versions.size() == 2, "path-traversal file name refused");
    CHECK(validateVersion(bad) == "a file entry is malformed", "validateVersion explains");

    // oversized payload refused
    json huge = version(id, 3, "huge"); huge["description"] = std::string(20000, 'x');
    l2.push_back(makeEvent(alice, "model.version", huge, T++, eid()));
    CHECK(fold(l2).models[id].versions.size() == 2, "oversized event refused");

    // CIDs: anyone may announce; dedup; capped at 4
    std::string sha = sha256Hex("Mule v1");
    for (int i = 0; i < 6; i++) log.push_back(makeEvent(i % 2 ? bob : alice, "blob.cids", {{"cids", {{sha, "zCID" + std::to_string(i / 2)}}}}, T++, eid()));
    Catalog c3 = fold(log);
    CHECK(c3.cids[sha].size() == 3 && c3.cids[sha][0] == "zCID0", "cid candidates deduped in order");

    // community: comments, makes, likes (LWW)
    log.push_back(makeEvent(bob, "comment.post", {{"modelId", id}, {"text", "Printed fine!"}}, T++, eid()));
    log.push_back(makeEvent(bob, "make.post", {{"modelId", id}, {"v", 2}, {"text", "PLA, 0.2 mm"}, {"images", {{{"sha256", sha}, {"size", 10}, {"mime", "image/png"}}}}}, T++, eid()));
    log.push_back(makeEvent(bob, "like.put", {{"modelId", id}, {"on", true}}, T++, eid()));
    log.push_back(makeEvent(alice, "like.put", {{"modelId", id}, {"on", true}}, T++, eid()));
    log.push_back(makeEvent(bob, "like.put", {{"modelId", id}, {"on", false}}, T++, eid()));
    log.push_back(makeEvent(bob, "profile.put", {{"name", "Bob"}}, T++, eid()));
    Catalog c4 = fold(log);
    CHECK(c4.models[id].comments.size() == 1 && c4.models[id].makes.size() == 1 && likeCount(c4.models[id]) == 1, "comments, makes, likes fold");
    CHECK(c4.profiles[bob.address]["name"] == "Bob", "profile");

    // search
    CHECK(matches(c4.models[id], "mule", "") && matches(c4.models[id], "", "Benchy") && !matches(c4.models[id], "cat", ""), "search title + tag");

    // order independence: shuffled delivery folds identically
    std::mt19937 rng(7);
    for (int k = 0; k < 5; k++) {
        std::vector<Event> sh = log;
        std::shuffle(sh.begin(), sh.end(), rng);
        Catalog s = fold(sh);
        CHECK(s.models[id].versions.size() == 2 && s.models[id].comments.size() == 1 && likeCount(s.models[id]) == 1 && s.cids[sha].size() == 3, "shuffle " << k);
    }

    // retract by creator
    log.push_back(makeEvent(alice, "model.retract", {{"modelId", id}, {"reason", "superseded"}}, T++, eid()));
    CHECK(fold(log).models[id].retracted, "creator can retract");

    // content topics must be ones Delivery accepts: 4 parts, or 5 with a numeric generation
    bool allValid = validContentTopic(PEOPLE_TOPIC);
    for (const auto& [cat, label] : categories()) allValid = allValid && validContentTopic(categoryTopic(cat));
    CHECK(allValid, "people + every category topic is a valid content topic");
    CHECK(!validContentTopic("/swamp/2/cat/tools/proto") && validContentTopic("/0/swamp/2/cat/proto")
          && !validContentTopic("swamp/2/x/proto") && !validContentTopic("/swamp//x/proto"), "invalid topics refused");

    std::cout << passes << " passed, " << fails << " failed\n";
    return fails ? 1 : 0;
}

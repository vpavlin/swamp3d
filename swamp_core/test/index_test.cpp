// Global search index (ADR 0015): deterministic shards, prefix + AND search, record shards,
// manifest validation.
#include "swamp_index.hpp"
#include <iostream>
#include <random>
using namespace swamp;
static int fails = 0, passes = 0;
#define CHECK(c, w) do { if (c) passes++; else { fails++; std::cerr << "FAIL: " << w << "\n"; } } while (0)

static long long T = 1790000000000;
static int seq = 0;
static std::string eid() { return "e" + std::to_string(++seq); }

static json version(const std::string& id, const std::string& title, json tags) {
    return json{{"modelId", id}, {"v", 1}, {"title", title}, {"summary", "a thing"}, {"licence", "CC-BY-4.0"}, {"tags", tags},
                {"files", {{{"name", "part.stl"}, {"kind", "model"}, {"size", 123}, {"sha256", sha256Hex(title)}}}},
                {"images", {{{"kind", "thumb"}, {"size", 99}, {"sha256", sha256Hex("img" + title)}, {"mime", "image/png"}}}}};
}

int main() {
    Identity alice = identityFrom(logos_sync::generatePrivateKey()), bob = identityFrom(logos_sync::generatePrivateKey());
    std::vector<Event> log;
    log.push_back(makeEvent(alice, "profile.put", {{"name", "Alice"}}, T++, eid()));
    log.push_back(makeEvent(bob, "profile.put", {{"name", "Bob Builder"}}, T++, eid()));
    auto model = [&](Identity& who, const std::string& nonce, const std::string& title, json tags, const std::string& cat) {
        std::string id = modelIdFor(who.address, nonce);
        log.push_back(makeEvent(who, "model.create", {{"modelId", id}, {"nonce", nonce}, {"title", title}, {"category", cat}}, T++, eid()));
        log.push_back(makeEvent(who, "model.version", version(id, title, tags), T++, eid()));
        return id;
    };
    std::string bracelet = model(alice, "n1", "Geometric bracelet", {"fashion", "jewelry"}, "fashion");
    std::string braceletBox = model(bob, "n2", "Bracelet box", {"storage"}, "organizers");
    std::string wrench = model(bob, "n3", "Wrench holder", {"tools", "wall"}, "tools");
    log.push_back(makeEvent(bob, "comment.post", {{"modelId", bracelet}, {"text", "nice"}}, T++, eid()));
    log.push_back(makeEvent(bob, "like.put", {{"modelId", bracelet}, {"on", true}}, T++, eid()));

    Catalog c = fold(log);
    auto shards = index::build(c, log);
    // determinism: same events in another order -> the same bytes
    std::vector<Event> shuffled = log;
    std::mt19937 rng(7); std::shuffle(shuffled.begin(), shuffled.end(), rng);
    auto again = index::build(fold(shuffled), shuffled);
    bool same = shards.size() == again.size();
    for (const auto& [k, v] : shards) same = same && again.count(k) && again[k].dump() == v.dump();
    CHECK(same, "two builds from the same catalogue are byte-identical (any event order)");

    std::map<std::string, json> terms;
    for (const auto& [k, v] : shards) if (k[0] == 't') terms[k] = v;
    CHECK(terms.count("tb") && terms.count("tw") && terms.count("tf"), "term shards keyed by first letter");
    json r = index::search("brace", terms, "", 10);
    CHECK(r.size() == 2, "prefix 'brace' finds both bracelet models");
    r = index::search("bracelet box", terms, "", 10);
    CHECK(r.size() == 1 && r[0]["m"] == braceletBox, "every word must match (AND)");
    r = index::search("bracelet", terms, "fashion", 10);
    CHECK(r.size() == 1 && r[0]["m"] == bracelet && r[0]["n"] == "Alice" && r[0]["k"] == 1, "category filter; entry carries creator name and likes");
    CHECK(index::search("builder", terms, "", 10).size() == 2, "creator names are searchable");
    CHECK(index::search("jewelry", terms, "", 10).size() == 1, "tags are searchable");
    CHECK(index::search("zzz", terms, "", 10).empty() && index::search("", terms, "", 10).empty(), "no match / empty query");
    CHECK(r[0]["th"]["sha"] == sha256Hex("imgGeometric bracelet"), "entry names the thumbnail");

    // record shard: the model's events + the profiles of everyone involved, and nothing else
    json rec = shards[index::recordKeyFor(bracelet)];
    std::vector<Event> evs;
    for (const auto& j : rec["events"]) { Event e; eventFrom(j, e); evs.push_back(e); }
    Catalog part = fold(evs);
    CHECK(part.models.count(bracelet) && part.models[bracelet].comments.size() == 1 && part.models[bracelet].likes.size() == 1, "record shard rebuilds the model with its comments and likes");
    CHECK(part.profiles.count(alice.address) && part.profiles.count(bob.address), "...and the profiles of its creator and commenters");
    bool onlyThis = true;
    for (const auto& [id, m] : part.models) if (index::recordKeyFor(id) != index::recordKeyFor(bracelet)) onlyThis = false;
    CHECK(onlyThis, "a record shard holds only models of its id bucket");

    // retracted models leave the index
    log.push_back(makeEvent(bob, "model.retract", {{"modelId", wrench}}, T++, eid()));
    auto s2 = index::build(fold(log), log);
    std::map<std::string, json> t2; for (const auto& [k, v] : s2) if (k[0] == 't') t2[k] = v;
    CHECK(index::search("wrench", t2, "", 10).empty(), "a retracted model isn't indexed");

    // manifests
    json mf{{"v", 1}, {"epoch", 5}, {"root", "abc"}, {"models", 3}, {"shards", {{"tb", {{"sha256", std::string(64, 'a')}, {"size", 10}, {"cid", "zX"}}}}}};
    CHECK(validManifest(mf), "a well-formed manifest is valid");
    json bad = mf; bad["shards"]["../x"] = mf["shards"]["tb"];
    CHECK(!validManifest(bad), "a manifest with a strange shard key is rejected");
    log.push_back(makeEvent(bob, "index.manifest", mf, T++, eid()));
    Catalog c3 = fold(log);
    CHECK(c3.indexes.count(bob.address) && c3.indexes[bob.address]["epoch"] == 5, "the fold keeps an indexer's manifest");
    CHECK(topicOf(log.back(), c3) == PEOPLE_TOPIC, "manifests travel on the people topic");

    std::cout << passes << " passed, " << fails << " failed\n";
    return fails ? 1 : 0;
}

#pragma once
// swamp_index.hpp - the global search index (ADR 0015), as pure functions so every indexer builds
// the SAME bytes from the same catalogue (that's what makes omission provable, ADR 0016).
//
// Two kinds of shard, each one file in Logos Storage:
//   term shard "t<c>"   - every normalised term starting with character c -> model ids, plus the
//                         index entries of those models, so one fetch answers a one-word query
//   record shard "r<h>" - every signed event of the models whose id starts with hex digit h, plus
//                         the latest profile of each author involved: opening a result outside
//                         your categories needs only this file
// A manifest (signed event "index.manifest") lists every shard's sha256, size and CID.
#include "swamp_catalog.hpp"
#include <algorithm>
#include <cctype>

namespace swamp {
namespace index {

constexpr int VERSION = 1;

// Lower-case ASCII words of 2+ characters; anything else separates words.
inline std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    std::string w;
    auto flush = [&] { if (w.size() >= 2 && w.size() <= 40) out.push_back(w); w.clear(); };
    for (unsigned char c : s) {
        if (std::isalnum(c)) w += (char)std::tolower(c);
        else flush();
    }
    flush();
    return out;
}

inline std::string shardKeyFor(const std::string& term) { return term.empty() ? "" : std::string("t") + term[0]; }
inline std::string recordKeyFor(const std::string& modelId) { return modelId.empty() ? "" : std::string("r") + (char)std::tolower((unsigned char)modelId[0]); }

// The searchable summary of one model (~200 B).
inline json entryOf(const Model& m, const Catalog& c) {
    const json& v = m.versions.back();
    auto pit = c.profiles.find(m.creator);
    std::string name = pit == c.profiles.end() ? "" : pit->second.value("name", "");
    json thumb = nullptr;
    for (const auto& im : arr(v, "images")) {
        std::string sha = str(im, "sha256");
        auto ci = c.cids.find(sha);
        thumb = json{{"sha", sha}, {"size", num(im, "size")}, {"cids", ci == c.cids.end() ? json::array() : json(ci->second)}};
        break;
    }
    int likes = 0;
    for (const auto& [who, on] : m.likes) likes += on;
    return json{{"m", m.modelId}, {"v", (int)m.versions.size()}, {"t", str(v, "title")}, {"s", clip(v, "summary", 140)},
                {"g", v.contains("tags") ? v["tags"] : json::array()}, {"c", m.category}, {"a", m.creator}, {"n", name},
                {"l", str(v, "licence")}, {"k", likes}, {"mk", (int)m.makes.size()}, {"at", num(v, "published")},
                {"th", thumb}, {"remix", !arr(v, "parents").empty()}};
}

inline std::set<std::string> termsOf(const json& e) {
    std::set<std::string> t;
    for (const char* k : {"t", "s", "n"}) for (const auto& w : words(str(e, k))) t.insert(w);
    for (const auto& g : arr(e, "g")) if (g.is_string()) for (const auto& w : words(g.get<std::string>())) t.insert(w);
    return t;
}

// Which model an event is about (empty for profiles; blob.cids by its first file).
inline std::string modelOfEvent(const Event& e, const Catalog& c) {
    std::string mid = str(e.payload, "modelId");
    if (mid.empty() && e.type == "blob.cids" && e.payload.contains("cids") && e.payload["cids"].is_object() && !e.payload["cids"].empty()) {
        auto bm = c.blobModel.find(e.payload["cids"].begin().key());
        if (bm != c.blobModel.end()) mid = bm->second;
    }
    return mid;
}

// Build every shard from a folded catalogue and its event log. Deterministic: models and terms
// are visited in sorted order, events by id, and json objects serialise with sorted keys.
// `excluded`: models this indexer declines to carry - declared in its manifest (ADR 0016), so
// leaving them out is a published policy, not a silent omission.
inline std::map<std::string, json> build(const Catalog& c, const std::vector<Event>& log, const std::set<std::string>& excluded = {}) {
    std::map<std::string, json> shards;
    std::map<std::string, json> entries;
    for (const auto& [id, m] : c.models) if (!m.versions.empty() && !m.retracted && !excluded.count(id)) entries[id] = entryOf(m, c);
    for (const auto& [id, e] : entries) {
        for (const auto& term : termsOf(e)) {
            json& sh = shards[shardKeyFor(term)];
            if (sh.is_null()) sh = json{{"kind", "terms"}, {"v", VERSION}, {"terms", json::object()}, {"entries", json::object()}};
            json& ids = sh["terms"][term];
            if (ids.is_null()) ids = json::array();
            ids.push_back(id);
            sh["entries"][id] = e;
        }
    }
    // record shards: events sorted by id, then the latest profile of everyone involved
    std::vector<const Event*> evs;
    for (const auto& e : log) evs.push_back(&e);
    std::sort(evs.begin(), evs.end(), [](const Event* a, const Event* b) { return a->id < b->id; });
    std::map<std::string, std::set<std::string>> authors;   // record key -> author addresses
    std::map<std::string, const Event*> latestProfile;
    for (const Event* e : evs) {
        if (e->type == "profile.put") {
            auto it = latestProfile.find(e->dev);
            if (it == latestProfile.end() || logos_sync::compareHlc(it->second->hlc, e->hlc) < 0) latestProfile[e->dev] = e;
            continue;
        }
        std::string mid = modelOfEvent(*e, c);
        if (!entries.count(mid)) continue;   // unknown or retracted models aren't indexed
        std::string key = recordKeyFor(mid);
        json& sh = shards[key];
        if (sh.is_null()) sh = json{{"kind", "records"}, {"v", VERSION}, {"events", json::array()}};
        sh["events"].push_back(logos_sync::eventToJson(*e));
        authors[key].insert(e->dev);
    }
    for (const auto& [key, who] : authors)
        for (const auto& a : who) { auto p = latestProfile.find(a); if (p != latestProfile.end()) shards[key]["events"].push_back(logos_sync::eventToJson(*p->second)); }
    return shards;
}

// A term shard from an indexer, checked and cleaned before anything reads it: the indexer controls
// the bytes, so wrong types must not throw (review 2026-10-07). Returns null if it isn't a term
// shard at all; drops malformed terms and entries; caps sizes.
inline json cleanTermShard(const json& in) {
    if (!in.is_object() || str(in, "kind") != "terms") return nullptr;
    json out{{"kind", "terms"}, {"v", VERSION}, {"terms", json::object()}, {"entries", json::object()}};
    if (in.contains("entries") && in["entries"].is_object())
        for (auto it = in["entries"].begin(); it != in["entries"].end() && out["entries"].size() < 100000; ++it) {
            const json& e = it.value();
            if (!e.is_object() || !isHex(it.key(), 32) || str(e, "m") != it.key()) continue;
            json c{{"m", it.key()}, {"v", num(e, "v")}, {"t", clip(e, "t", 200)}, {"s", clip(e, "s", 140)}, {"c", clip(e, "c", 32)},
                   {"a", clip(e, "a", 64)}, {"n", clip(e, "n", 60)}, {"l", clip(e, "l", 64)}, {"k", std::max(0LL, num(e, "k"))},
                   {"mk", std::max(0LL, num(e, "mk"))}, {"at", num(e, "at")}, {"remix", flag(e, "remix")}, {"g", json::array()}, {"th", nullptr}};
            for (const auto& g : arr(e, "g")) if (g.is_string() && c["g"].size() < 16) c["g"].push_back(g.get<std::string>().substr(0, 32));
            if (e.contains("th") && e["th"].is_object() && isHex(str(e["th"], "sha"), 64)) {
                json th{{"sha", str(e["th"], "sha")}, {"size", num(e["th"], "size")}, {"cids", json::array()}};
                for (const auto& x : arr(e["th"], "cids")) if (x.is_string() && th["cids"].size() < 8 && x.get<std::string>().size() <= 128) th["cids"].push_back(x);
                c["th"] = th;
            }
            out["entries"][it.key()] = c;
        }
    if (in.contains("terms") && in["terms"].is_object())
        for (auto it = in["terms"].begin(); it != in["terms"].end(); ++it) {
            if (!it.value().is_array() || it.key().empty() || it.key().size() > 40) continue;
            json ids = json::array();
            for (const auto& id : it.value()) if (id.is_string() && out["entries"].contains(id.get<std::string>()) && ids.size() < 10000) ids.push_back(id);
            if (!ids.empty()) out["terms"][it.key()] = ids;
        }
    return out;
}

// One query against the term shards a client holds (cleaned with cleanTermShard). Every query word must match an indexed term
// exactly or as a prefix (so "brace" finds "bracelet"); results ranked by how many words matched
// in the title, then likes, then newest.
inline json search(const std::string& query, const std::map<std::string, json>& termShards, const std::string& category, size_t limit) {
    std::vector<std::string> q = words(query);
    json out = json::array();
    if (q.empty()) return out;
    std::map<std::string, json> found;
    std::map<std::string, int> hits;
    bool first = true;
    std::set<std::string> alive;
    for (const auto& w : q) {
        auto sh = termShards.find(shardKeyFor(w));
        std::set<std::string> ids;
        if (sh != termShards.end() && sh->second.contains("terms") && sh->second["terms"].is_object()) {
            const auto& terms = sh->second["terms"].get_ref<const json::object_t&>();   // a sorted map
            for (auto it = terms.lower_bound(w); it != terms.end() && it->first.compare(0, w.size(), w) == 0; ++it)
                for (const auto& id : it->second) {
                    std::string s = id.get<std::string>();
                    ids.insert(s);
                    if (!found.count(s) && sh->second["entries"].contains(s)) found[s] = sh->second["entries"][s];
                }
        }
        if (first) alive = ids;
        else { std::set<std::string> keep; for (const auto& id : alive) if (ids.count(id)) keep.insert(id); alive = keep; }
        first = false;
    }
    std::vector<json> res;
    for (const auto& id : alive) {
        const json& e = found[id];
        if (!category.empty() && str(e, "c") != category) continue;
        int titleHits = 0;
        auto tw = words(str(e, "t"));
        for (const auto& w : q) for (const auto& t : tw) if (t.compare(0, w.size(), w) == 0) { titleHits++; break; }
        json r = e; r["_score"] = titleHits;
        res.push_back(r);
    }
    std::sort(res.begin(), res.end(), [](const json& a, const json& b) {
        if (num(a, "_score") != num(b, "_score")) return num(a, "_score") > num(b, "_score");
        if (num(a, "k") != num(b, "k")) return num(a, "k") > num(b, "k");
        return num(a, "at") > num(b, "at");
    });
    for (size_t i = 0; i < res.size() && i < limit; i++) out.push_back(res[i]);
    return out;
}

} // namespace index
} // namespace swamp

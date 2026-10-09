#pragma once
// swamp_catalog.hpp - the catalogue: signed events -> folded state. Qt-free, pure.
// docs/SPEC.md sections 3 and 5; ADRs 0009-0011.
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <openssl/sha.h>
#include <nlohmann/json.hpp>
#include "logos_sync/event.hpp"
#include "logos_sync/merge.hpp"
#include "logos_sync/signing.hpp"

namespace swamp {

using json = nlohmann::json;
using logos_sync::Event;
inline const std::string DOMAIN = "swamp";
// Topics (ADR 0014). Nobody mirrors the whole catalogue: a model and everything about it lives on
// its category's topic; profiles live on one small people topic.
inline const std::string PEOPLE_TOPIC = "/swamp/2/people/proto";
// Files handed to hubs (only hubs join it; publishers send without joining). See hubHandoff().
inline const std::string HUB_TOPIC = "/swamp/2/hub-files/proto";
// Waku content topics are /app/version/name/encoding (or /generation/app/version/name/encoding with
// a numeric generation): a 5-part "/swamp/2/cat/x/proto" is rejected by Delivery's channels
// ("generation should be a numeric value") - found in review 2026-10-07. Four parts only.
inline std::string categoryTopic(const std::string& cat) { return "/swamp/2/cat-" + cat + "/proto"; }
inline bool validContentTopic(const std::string& t) {
    if (t.empty() || t[0] != '/') return false;
    std::vector<std::string> parts;
    size_t i = 1;
    while (i <= t.size()) { size_t j = t.find('/', i); if (j == std::string::npos) j = t.size(); parts.push_back(t.substr(i, j - i)); i = j + 1; }
    for (const auto& p : parts) if (p.empty()) return false;
    if (parts.size() == 4) return true;
    if (parts.size() == 5) { for (char c : parts[0]) if (c < '0' || c > '9') return false; return true; }
    return false;
}
// The category list is part of the protocol: adding one is an app update. "other" catches the rest.
inline const std::vector<std::pair<std::string, std::string>>& categories() {
    static const std::vector<std::pair<std::string, std::string>> C = {
        {"household", "Household"}, {"kitchen", "Kitchen"}, {"organizers", "Storage & organizers"},
        {"tools", "Tools"}, {"workshop", "Workshop & jigs"}, {"parts", "Parts & repair"},
        {"electronics", "Electronics & enclosures"}, {"printer", "3D printer parts"}, {"toys", "Toys"},
        {"games", "Games & puzzles"}, {"art", "Art & sculpture"}, {"fashion", "Fashion & jewelry"},
        {"cosplay", "Costume & cosplay"}, {"hobby", "Hobby & RC"}, {"garden", "Garden & outdoor"},
        {"education", "Education & science"}, {"accessibility", "Medical & accessibility"},
        {"office", "Office & desk"}, {"other", "Other"}};
    return C;
}
inline bool knownCategory(const std::string& c) {
    for (const auto& [id, label] : categories()) if (id == c) return true;
    return false;
}
constexpr size_t MAX_PAYLOAD = 16 * 1024;

inline std::string sha256Hex(const std::string& data) {
    unsigned char h[32];
    SHA256((const unsigned char*)data.data(), data.size(), h);
    static const char* X = "0123456789abcdef";
    std::string s;
    for (unsigned char c : h) { s += X[c >> 4]; s += X[c & 15]; }
    return s;
}
inline bool isHex(const std::string& s, size_t len) {
    if (s.size() != len) return false;
    for (char c : s) if (!std::isxdigit((unsigned char)c) || std::isupper((unsigned char)c)) return false;
    return true;
}
/** modelId = first 32 hex chars of sha256(creatorAddress | nonce). Can't be squatted. */
inline std::string modelIdFor(const std::string& creator, const std::string& nonce) {
    return sha256Hex(creator + "|" + nonce).substr(0, 32);
}

inline bool knownType(const std::string& t) {
    static const std::set<std::string> T = {"index.manifest", "profile.put", "model.create", "model.version", "blob.cids", "model.retract",
                                            "comment.post", "make.post", "like.put"};
    return T.count(t) > 0;
}

// ── type-safe access to UNTRUSTED json (nlohmann .value() throws on a wrong type) ─────────────
inline std::string str(const json& j, const char* k) {
    if (!j.is_object()) return "";
    auto it = j.find(k);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}
inline long long num(const json& j, const char* k, long long dflt = 0) {
    if (!j.is_object()) return dflt;
    auto it = j.find(k);
    return it != j.end() && it->is_number_integer() ? it->get<long long>() : dflt;
}
inline bool flag(const json& j, const char* k, bool dflt = false) {
    if (!j.is_object()) return dflt;
    auto it = j.find(k);
    return it != j.end() && it->is_boolean() ? it->get<bool>() : dflt;
}
inline const json& arr(const json& j, const char* k) {
    static const json empty = json::array();
    if (!j.is_object()) return empty;
    auto it = j.find(k);
    return it != j.end() && it->is_array() ? *it : empty;
}

// ── validation of the version payload (refuse up front; the fold uses the same rules) ──────────
inline std::string clip(const json& j, const char* k, size_t max) {
    std::string s = str(j, k);
    return s.size() > max ? s.substr(0, max) : s;
}
inline bool safeName(const std::string& n) {
    if (n.empty() || n.size() > 160 || n == "." || n == "..") return false;
    for (char c : n) if (c == '/' || c == '\\' || (unsigned char)c < 0x20) return false;
    return true;
}
constexpr long long MAX_FILE_BYTES = 1024LL * 1024 * 1024;   // 1 GiB per file
inline bool validBlobRef(const json& f, bool needName) {
    if (!f.is_object() || !isHex(str(f, "sha256"), 64)) return false;
    long long size = num(f, "size", 0);
    if (size <= 0 || size > MAX_FILE_BYTES) return false;
    if (needName && !safeName(str(f, "name"))) return false;
    return true;
}
/** Returns "" if valid, else a sentence. */
inline std::string validateVersion(const json& p) {
    if (!p.is_object()) return "version is not an object";
    for (const char* k : {"title", "summary", "description", "licence"})
        if (p.contains(k) && !p[k].is_string()) return std::string(k) + " must be text";
    std::string title = str(p, "title");
    if (title.empty() || title.size() > 140) return "a title (1-140 characters) is required";
    if (str(p, "description").size() > 8192) return "description is longer than 8 KiB";
    if (str(p, "summary").size() > 280) return "summary is longer than 280 characters";
    std::string lic = str(p, "licence");
    if (lic.empty() || lic.size() > 64) return "choose a licence";
    const json& files = arr(p, "files");
    if (files.empty() || files.size() > 64) return "a version needs 1-64 files";
    std::set<std::string> names;
    for (const auto& f : files) {
        if (!validBlobRef(f, true)) return "a file entry is malformed";
        if (!names.insert(str(f, "name")).second) return "two files have the same name: " + str(f, "name");
    }
    if (p.contains("images") && (!p["images"].is_array() || p["images"].size() > 16)) return "at most 16 images";
    for (const auto& f : arr(p, "images")) if (!validBlobRef(f, false) || num(f, "size") > 16 * 1024 * 1024) return "an image entry is malformed (or larger than 16 MiB)";
    if (p.contains("tags") && (!p["tags"].is_array() || p["tags"].size() > 16)) return "at most 16 tags";
    for (const auto& t : arr(p, "tags")) if (!t.is_string() || t.get<std::string>().empty() || t.get<std::string>().size() > 32) return "tags are 1-32 characters";
    if (p.contains("parents") && (!p["parents"].is_array() || p["parents"].size() > 8)) return "at most 8 parents";
    for (const auto& r : arr(p, "parents")) if (!r.is_object() || !isHex(str(r, "modelId"), 32) || num(r, "v", 0) < 1) return "a parent reference is malformed";
    if (p.contains("fp") && !p["fp"].is_null()) {
        const json& f = p["fp"];
        if (!validBlobRef(f, false) || arr(f, "d2").size() != 64 || arr(f, "a3").size() != 32) return "the fingerprint entry is malformed";
        for (const char* k : {"d2", "a3"}) for (const auto& x : arr(f, k)) if (!x.is_number()) return "the fingerprint entry is malformed";
    }
    return "";
}

// An index manifest (ADR 0015): {v, epoch, root, models, shards: {key: {sha256, size, cid}}}.
inline bool validManifest(const json& p) {
    if (!p.is_object() || num(p, "v") != 1 || num(p, "epoch") <= 0 || str(p, "root").size() > 128) return false;
    if (!p.contains("shards") || !p["shards"].is_object() || p["shards"].size() > 128) return false;
    for (auto it = p["shards"].begin(); it != p["shards"].end(); ++it) {
        const std::string& k = it.key();
        if (k.size() != 2 || (k[0] != 't' && k[0] != 'r')) return false;
        if (!isHex(str(it.value(), "sha256"), 64) || num(it.value(), "size") <= 0 || num(it.value(), "size") > 256LL * 1024 * 1024) return false;
        if (str(it.value(), "cid").empty() || str(it.value(), "cid").size() > 128) return false;
    }
    if (p.contains("excluded")) {
        if (!p["excluded"].is_array() || p["excluded"].size() > 200) return false;
        for (const auto& x : p["excluded"]) if (!x.is_object() || !isHex(str(x, "m"), 32) || str(x, "why").size() > 140) return false;
    }
    return true;
}

// ── folded state ────────────────────────────────────────────────────────────────────────────
struct Model {
    std::string modelId, creator, category = "other";
    long long created = 0;
    std::vector<json> versions;   // index v-1
    bool retracted = false;
    std::string retractReason;
    std::map<std::string, bool> likes;          // author -> on
    std::vector<json> comments, makes;
    long long lastActivity = 0;
};
struct Catalog {
    std::map<std::string, Model> models;
    std::map<std::string, json> profiles;        // author -> {name, bio}
    std::map<std::string, std::vector<std::string>> cids;   // sha256 -> candidate CIDs, best first
    std::map<std::string, std::string> owner;
    std::map<std::string, std::string> blobModel;               // sha256 -> first model listing it (topic routing)
    std::map<std::string, json> indexes;                        // indexer address -> its latest index manifest (ADR 0015)                // sha256 -> creator of the first model listing it
    size_t events = 0, rejected = 0;
};

inline bool payloadOk(const Event& e) {
    return e.payload.is_object() && e.payload.dump().size() <= MAX_PAYLOAD;
}

/** Admit an event to the log at all (signature, type, size). Content rules live in the fold. */
inline bool admissible(const Event& e) {
    // The signature covers hlc.dev, not e.dev: the author IS the signer, or anyone could post as
    // anyone (review C1).
    if (e.dev.empty() || e.dev != e.hlc.dev) return false;
    if (e.id.empty() || e.id.size() > 64 || e.pub.size() > 70 || e.sig.size() > 140 || e.type.size() > 32) return false;
    return knownType(e.type) && payloadOk(e) && logos_sync::verifyEvent(DOMAIN, e);
}

/** Parse an event from untrusted JSON without throwing. */
inline bool eventFrom(const json& j, Event& out) {
    if (!j.is_object()) return false;
    try { out = logos_sync::eventFromJson(j); } catch (...) { return false; }
    return true;
}

// Fold is order-independent in practice: pass 1 builds models and versions (HLC order, the
// creator's own events), pass 2 applies everything that only REFERS to a model, so a comment from
// a peer whose clock lags the creator's still lands (review H5).
// `admitted`: every event already passed admissible() on the way in (the module checks each event
// once, at ingest), so the ~0.5 ms signature check isn't paid again on every refold (review H3).
inline Catalog fold(const std::vector<Event>& log, bool admitted = false) {
    Catalog c;
    std::vector<Event> evs = logos_sync::mergeEvents(log, {});
    std::vector<const Event*> ok;
    for (const auto& e : evs) { if (admitted || admissible(e)) ok.push_back(&e); else c.rejected++; }
    c.events = ok.size();
    auto model = [&](const std::string& id) -> Model* { auto it = c.models.find(id); return it == c.models.end() ? nullptr : &it->second; };
    // pass 1: models, versions, retractions, profiles
    for (const Event* ep : ok) {
        const Event& e = *ep; const json& p = e.payload; const std::string& who = e.dev; const long long t = e.hlc.wall;
        if (e.type == "profile.put") {
            c.profiles[who] = json{{"name", clip(p, "name", 60)}, {"bio", clip(p, "bio", 500)}};
        } else if (e.type == "index.manifest") {
            if (!validManifest(p)) { c.rejected++; continue; }
            json mf = p; mf["indexer"] = who; mf["published"] = t; mf["eventId"] = e.id;
            c.indexes[who] = mf;   // events fold in HLC order: the newest manifest wins
        } else if (e.type == "model.create") {
            std::string id = str(p, "modelId"), nonce = str(p, "nonce");
            if (!isHex(id, 32) || nonce.empty() || nonce.size() > 64 || modelIdFor(who, nonce) != id || c.models.count(id)) { c.rejected++; continue; }
            Model m; m.modelId = id; m.creator = who; m.created = t; m.lastActivity = t;
            std::string cat = str(p, "category");
            if (knownCategory(cat)) m.category = cat;   // missing/unknown (e.g. an M1 model) = "other"
            c.models[id] = m;
        } else if (e.type == "model.version") {
            Model* m = model(str(p, "modelId"));
            if (!m || m->creator != who || !validateVersion(p).empty() || num(p, "v", 0) != (long long)m->versions.size() + 1) { c.rejected++; continue; }
            json v = p; v["published"] = t; v["author"] = who;
            m->versions.push_back(v);
            m->lastActivity = std::max(m->lastActivity, t);
            for (const char* k : {"files", "images"}) for (const auto& f : arr(p, k)) { c.owner.emplace(str(f, "sha256"), who); c.blobModel.emplace(str(f, "sha256"), m->modelId); }
            if (p.contains("fp") && p["fp"].is_object()) { c.owner.emplace(str(p["fp"], "sha256"), who); c.blobModel.emplace(str(p["fp"], "sha256"), m->modelId); }
        } else if (e.type == "model.retract") {
            Model* m = model(str(p, "modelId"));
            if (!m || m->creator != who) { c.rejected++; continue; }
            m->retracted = true; m->retractReason = clip(p, "reason", 280);
        }
    }
    // pass 2: references (CIDs, comments, makes, likes)
    std::map<std::string, std::vector<std::string>> fromOwner, fromOthers;
    std::map<std::string, std::map<std::string, int>> perAnnouncer;   // sha -> announcer -> count
    for (const Event* ep : ok) {
        const Event& e = *ep; const json& p = e.payload; const std::string& who = e.dev; const long long t = e.hlc.wall;
        if (e.type == "blob.cids") {
            if (!p.contains("cids") || !p["cids"].is_object() || p["cids"].size() > 96) { c.rejected++; continue; }
            for (auto it = p["cids"].begin(); it != p["cids"].end(); ++it) {
                if (!isHex(it.key(), 64) || !it.value().is_string()) continue;
                std::string cid = it.value().get<std::string>();
                if (cid.empty() || cid.size() > 128) continue;
                // CID policy (review H4): the creator's CIDs always come first; every other announcer
                // gets at most one CID per blob, and at most 8 others are kept - junk can't crowd out
                // the real one, and a lying mirror only costs a failed (hash-checked) fetch.
                auto own = c.owner.find(it.key());
                bool isOwner = own != c.owner.end() && own->second == who;
                auto& list = isOwner ? fromOwner[it.key()] : fromOthers[it.key()];
                if (std::find(list.begin(), list.end(), cid) != list.end()) continue;
                if (isOwner) { if (list.size() < 4) list.push_back(cid); continue; }
                if (perAnnouncer[it.key()][who]++ >= 1 || list.size() >= 8) continue;
                list.push_back(cid);
            }
        } else if (e.type == "comment.post" || e.type == "make.post") {
            Model* m = model(str(p, "modelId"));
            std::string text = clip(p, "text", 4000);
            if (!m || (text.empty() && e.type == "comment.post")) { c.rejected++; continue; }
            json item{{"id", e.id}, {"author", who}, {"text", text}, {"at", t}};
            if (e.type == "make.post") {
                json imgs = json::array();
                for (const auto& f : arr(p, "images")) if (validBlobRef(f, false) && num(f, "size") <= 16 * 1024 * 1024 && imgs.size() < 8) imgs.push_back(f);
                item["images"] = imgs;
                if (num(p, "v", 0) > 0) item["v"] = num(p, "v", 0);
                m->makes.push_back(item);
            } else {
                std::string rt = clip(p, "replyTo", 64);
                if (!rt.empty()) item["replyTo"] = rt;
                m->comments.push_back(item);
            }
            m->lastActivity = std::max(m->lastActivity, t);
        } else if (e.type == "like.put") {
            Model* m = model(str(p, "modelId"));
            if (!m || !p.contains("on") || !p["on"].is_boolean()) { c.rejected++; continue; }
            m->likes[who] = p["on"].get<bool>();
        }
    }
    for (auto& [sha, list] : fromOwner) c.cids[sha] = list;
    for (auto& [sha, list] : fromOthers) { auto& out = c.cids[sha]; for (const auto& x : list) if (std::find(out.begin(), out.end(), x) == out.end()) out.push_back(x); }
    return c;
}

inline int likeCount(const Model& m) { int n = 0; for (const auto& [a, on] : m.likes) n += on; return n; }

/** Case-insensitive substring match over title, summary, tags (latest version). */
inline bool matches(const Model& m, const std::string& q, const std::string& tag) {
    if (m.versions.empty()) return false;
    const json& v = m.versions.back();
    auto lower = [](std::string s) { for (auto& ch : s) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a'); return s; };
    if (!tag.empty()) {
        bool has = false;
        for (const auto& t : arr(v, "tags")) if (t.is_string() && lower(t.get<std::string>()) == lower(tag)) has = true;
        if (!has) return false;
    }
    if (q.empty()) return true;
    std::string hay = lower(str(v, "title") + " " + str(v, "summary"));
    for (const auto& t : arr(v, "tags")) if (t.is_string()) hay += " " + lower(t.get<std::string>());
    return hay.find(lower(q)) != std::string::npos;
}

// ── authoring ───────────────────────────────────────────────────────────────────────────────
struct Identity {
    logos_sync::Bytes priv;
    std::string address;
    logos_sync::Clock clock{""};
};
inline Identity identityFrom(const logos_sync::Bytes& priv) {
    Identity id;
    id.priv = priv;
    id.address = logos_sync::address(logos_sync::pubFromPriv(priv));
    id.clock = logos_sync::Clock(id.address);
    return id;
}
inline Event makeEvent(Identity& id, const std::string& type, const json& payload, long long nowMs, const std::string& eventId) {
    Event e;
    e.v = 1; e.id = eventId; e.type = type;
    e.hlc = id.clock.send(nowMs);
    e.dev = id.address;
    e.payload = payload;
    logos_sync::SoftwareSigner s(id.priv);
    logos_sync::signEvent(s, DOMAIN, e);
    return e;
}

// Which topic an event belongs on, from its content (empty = not known yet: its model hasn't
// arrived). Catch-up on a topic runs over exactly the events that map to it.
inline std::string topicOf(const Event& e, const Catalog& c) {
    if (e.type == "profile.put" || e.type == "index.manifest") return PEOPLE_TOPIC;
    const json& p = e.payload;
    if (e.type == "model.create") { std::string cat = str(p, "category"); return categoryTopic(knownCategory(cat) ? cat : "other"); }
    std::string mid = str(p, "modelId");
    if (mid.empty() && e.type == "blob.cids" && p.contains("cids") && p["cids"].is_object() && !p["cids"].empty()) {
        auto bm = c.blobModel.find(p["cids"].begin().key());
        if (bm != c.blobModel.end()) mid = bm->second;
    }
    auto m = c.models.find(mid);
    return m == c.models.end() ? std::string() : categoryTopic(m->second.category);
}

} // namespace swamp

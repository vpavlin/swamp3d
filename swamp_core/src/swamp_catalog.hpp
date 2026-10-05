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
inline const std::string CATALOG_TOPIC = "/swamp/1/catalog/proto";
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
    static const std::set<std::string> T = {"profile.put", "model.create", "model.version", "blob.cids", "model.retract",
                                            "comment.post", "make.post", "like.put"};
    return T.count(t) > 0;
}

// ── validation of the version payload (refuse up front; the fold uses the same rules) ──────────
inline std::string clip(const json& j, const char* k, size_t max) {
    if (!j.contains(k) || !j[k].is_string()) return "";
    std::string s = j[k].get<std::string>();
    return s.size() > max ? s.substr(0, max) : s;
}
inline bool safeName(const std::string& n) {
    if (n.empty() || n.size() > 160 || n == "." || n == "..") return false;
    for (char c : n) if (c == '/' || c == '\\' || (unsigned char)c < 0x20) return false;
    return true;
}
inline bool validBlobRef(const json& f, bool needName) {
    if (!f.is_object() || !isHex(f.value("sha256", ""), 64)) return false;
    if (!f.contains("size") || !f["size"].is_number_integer() || f["size"].get<long long>() <= 0) return false;
    if (needName && !safeName(f.value("name", ""))) return false;
    return true;
}
/** Returns "" if valid, else a sentence. */
inline std::string validateVersion(const json& p) {
    if (!p.is_object()) return "version is not an object";
    std::string title = p.value("title", "");
    if (title.empty() || title.size() > 140) return "a title (1-140 characters) is required";
    if (p.value("description", "").size() > 8192) return "description is longer than 8 KiB";
    if (p.value("summary", "").size() > 280) return "summary is longer than 280 characters";
    std::string lic = p.value("licence", "");
    if (lic.empty() || lic.size() > 64) return "choose a licence";
    if (!p.contains("files") || !p["files"].is_array() || p["files"].empty() || p["files"].size() > 64) return "a version needs 1-64 files";
    for (const auto& f : p["files"]) if (!validBlobRef(f, true)) return "a file entry is malformed";
    if (p.contains("images") && (!p["images"].is_array() || p["images"].size() > 16)) return "at most 16 images";
    if (p.contains("images")) for (const auto& f : p["images"]) if (!validBlobRef(f, false)) return "an image entry is malformed";
    if (p.contains("tags") && (!p["tags"].is_array() || p["tags"].size() > 16)) return "at most 16 tags";
    if (p.contains("tags")) for (const auto& t : p["tags"]) if (!t.is_string() || t.get<std::string>().empty() || t.get<std::string>().size() > 32) return "tags are 1-32 characters";
    if (p.contains("parents") && (!p["parents"].is_array() || p["parents"].size() > 8)) return "at most 8 parents";
    if (p.contains("parents")) for (const auto& r : p["parents"]) if (!r.is_object() || !isHex(r.value("modelId", ""), 32)) return "a parent reference is malformed";
    return "";
}

// ── folded state ────────────────────────────────────────────────────────────────────────────
struct Model {
    std::string modelId, creator;
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
    std::map<std::string, std::vector<std::string>> cids;   // sha256 -> candidate CIDs (<= 4)
    size_t events = 0, rejected = 0;
};

inline bool payloadOk(const Event& e) {
    return e.payload.is_object() && e.payload.dump().size() <= MAX_PAYLOAD;
}

/** Admit an event to the log at all (signature, type, size). Content rules live in the fold. */
inline bool admissible(const Event& e) {
    return knownType(e.type) && payloadOk(e) && logos_sync::verifyEvent(DOMAIN, e);
}

inline Catalog fold(const std::vector<Event>& log) {
    Catalog c;
    std::vector<Event> evs = logos_sync::mergeEvents(log, {});
    for (const auto& e : evs) {
        if (!admissible(e)) { c.rejected++; continue; }
        c.events++;
        const json& p = e.payload;
        const std::string& who = e.dev;
        const long long t = e.hlc.wall;
        auto model = [&](const std::string& id) -> Model* { auto it = c.models.find(id); return it == c.models.end() ? nullptr : &it->second; };
        if (e.type == "profile.put") {
            c.profiles[who] = json{{"name", clip(p, "name", 60)}, {"bio", clip(p, "bio", 500)}};
        } else if (e.type == "model.create") {
            std::string id = p.value("modelId", ""), nonce = p.value("nonce", "");
            if (!isHex(id, 32) || nonce.empty() || modelIdFor(who, nonce) != id || c.models.count(id)) { c.rejected++; continue; }
            Model m; m.modelId = id; m.creator = who; m.created = t; m.lastActivity = t;
            c.models[id] = m;
        } else if (e.type == "model.version") {
            Model* m = model(p.value("modelId", ""));
            if (!m || m->creator != who || !validateVersion(p).empty()) { c.rejected++; continue; }
            if (!p.contains("v") || !p["v"].is_number_integer() || p["v"].get<long long>() != (long long)m->versions.size() + 1) { c.rejected++; continue; }
            json v = p;
            v["published"] = t;
            v["author"] = who;
            m->versions.push_back(v);
            m->lastActivity = std::max(m->lastActivity, t);
        } else if (e.type == "blob.cids") {
            if (!p.contains("cids") || !p["cids"].is_object() || p["cids"].size() > 96) { c.rejected++; continue; }
            for (auto it = p["cids"].begin(); it != p["cids"].end(); ++it) {
                if (!isHex(it.key(), 64) || !it.value().is_string()) continue;
                std::string cid = it.value().get<std::string>();
                if (cid.empty() || cid.size() > 128) continue;
                auto& v = c.cids[it.key()];
                if (std::find(v.begin(), v.end(), cid) == v.end() && v.size() < 4) v.push_back(cid);
            }
        } else if (e.type == "model.retract") {
            Model* m = model(p.value("modelId", ""));
            if (!m || m->creator != who) { c.rejected++; continue; }
            m->retracted = true; m->retractReason = clip(p, "reason", 280);
        } else if (e.type == "comment.post" || e.type == "make.post") {
            Model* m = model(p.value("modelId", ""));
            std::string text = clip(p, "text", 4000);
            if (!m || (text.empty() && e.type == "comment.post")) { c.rejected++; continue; }
            json item{{"id", e.id}, {"author", who}, {"text", text}, {"at", t}};
            if (e.type == "make.post") {
                json imgs = json::array();
                if (p.contains("images") && p["images"].is_array())
                    for (const auto& f : p["images"]) if (validBlobRef(f, false) && imgs.size() < 8) imgs.push_back(f);
                item["images"] = imgs;
                if (p.contains("v") && p["v"].is_number_integer()) item["v"] = p["v"];
                m->makes.push_back(item);
            } else {
                if (p.contains("replyTo") && p["replyTo"].is_string()) item["replyTo"] = p["replyTo"];
                m->comments.push_back(item);
            }
            m->lastActivity = std::max(m->lastActivity, t);
        } else if (e.type == "like.put") {
            Model* m = model(p.value("modelId", ""));
            if (!m || !p.contains("on") || !p["on"].is_boolean()) { c.rejected++; continue; }
            m->likes[who] = p["on"].get<bool>();
        }
    }
    // A model whose first version never arrived isn't shown (create without content).
    return c;
}

inline int likeCount(const Model& m) { int n = 0; for (const auto& [a, on] : m.likes) n += on; return n; }

/** Case-insensitive substring match over title, summary, tags (latest version). */
inline bool matches(const Model& m, const std::string& q, const std::string& tag) {
    if (m.versions.empty()) return false;
    const json& v = m.versions.back();
    auto lower = [](std::string s) { std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; };
    if (!tag.empty()) {
        bool has = false;
        if (v.contains("tags")) for (const auto& t : v["tags"]) if (lower(t.get<std::string>()) == lower(tag)) has = true;
        if (!has) return false;
    }
    if (q.empty()) return true;
    std::string hay = lower(v.value("title", "") + " " + v.value("summary", ""));
    if (v.contains("tags")) for (const auto& t : v["tags"]) hay += " " + lower(t.get<std::string>());
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

} // namespace swamp

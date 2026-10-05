#include "swamp_core_impl.h"
#include "logos_sdk.h"
#include "logos_sync/catchup.hpp"
#include "swamp_fp.hpp"
#include "swamp_thumb.hpp"
#include <QTimer>
#include <QObject>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <pwd.h>
#include <unistd.h>

using namespace swamp;
namespace fs = std::filesystem;

static const char* SWAMP_VERSION = "0.1.0";
static constexpr int kStartDelayMs = 1000;          // 0.3 runtime rejects calls from inside onContextReady
static constexpr int kStorageTimeoutMs = 60000;     // storage 3.x waits up to 30 s for a manifest
static constexpr long long kCatchupEveryMs = 120000; // each SDS frame ~19-25 KB, RLN budget (logos-rln-budget)
static constexpr long long kTransferStaleMs = 10 * 60 * 1000;
static constexpr long long kStallMs = 2 * 60 * 1000;          // a download that stops growing this long is cancelled
static constexpr long long kUploadRetryMs = 60000;
static constexpr long long kManifestPollMs = 5000;
static constexpr long long kReannounceMs = 10 * 60 * 1000;
static constexpr long long kStatusPollMs = 15000;
static constexpr long long kSaveEveryMs = 5000;
static constexpr long long kServeWindowMs = 60000;
static constexpr long long kCatchupMaxAgeMs = 2 * 60 * 1000;
static constexpr long long kAnswerEveryMs = 20000;
static constexpr int kServePerWindow = 200;          // events served per minute, all peers together
static constexpr size_t kFrameBytes = 48 * 1024;     // events batched per frame up to this size
static constexpr size_t kCidsPerEvent = 64;
static constexpr long long kAnnounceHoldMs = 10000;
static constexpr int kMaxFetches = 6;
static constexpr int kPreviewFetches = 2;
static constexpr long long kPreviewMaxBytes = 2 * 1024 * 1024;
static constexpr int kHubConcurrency = 3;
static constexpr long long kMaxClockLeadMs = 5 * 60 * 1000;
static constexpr int kJobRounds = 3;                 // full passes over a file's CIDs before a download fails

// ---- helpers --------------------------------------------------------------------------------
static std::string b64std(const std::string& s) {
    std::string out(4 * ((s.size() + 2) / 3) + 1, '\0');
    int n = EVP_EncodeBlock((unsigned char*)out.data(), (const unsigned char*)s.data(), (int)s.size());
    out.resize(n < 0 ? 0 : n);
    return out;
}
static bool unb64(const std::string& in, std::string& out) {
    // standard or URL-safe alphabet (delivery 0.3 emits URL-safe), padding optional
    std::string s;
    for (char c : in) {
        if (isspace((unsigned char)c)) continue;
        s += c == '-' ? '+' : c == '_' ? '/' : c;
    }
    while (s.size() % 4) s += '=';
    if (s.empty()) return false;
    std::string buf(s.size() / 4 * 3 + 1, '\0');
    int n = EVP_DecodeBlock((unsigned char*)buf.data(), (const unsigned char*)s.data(), (int)s.size());
    if (n < 0) return false;
    size_t pad = 0;
    if (s[s.size() - 1] == '=') pad++;
    if (s[s.size() - 2] == '=') pad++;
    buf.resize(n - pad);
    out = buf;
    return true;
}
static std::string ok(json extra = json::object()) { extra["ok"] = true; return extra.dump(); }
// $HOME first: the 0.3 runtime gives each session/profile its own HOME, and module state must
// follow it (a separate Basecamp test profile, or two logosctl nodes on one machine, must not
// share data). Fall back to the account's home only when HOME is unset or relative.
static std::string homeDir() {
    const char* h = getenv("HOME");
    if (h && *h == '/') return h;
    if (struct passwd* pw = getpwuid(getuid())) if (pw->pw_dir && *pw->pw_dir == '/') return pw->pw_dir;
    return "/tmp";
}
static bool readFile(const std::string& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf(); out = ss.str();
    return true;
}
static bool writeFile(const std::string& p, const std::string& data) {
    std::string tmp = p + ".tmp";
    { std::ofstream f(tmp, std::ios::binary); if (!f) return false; f << data; if (!f) return false; }
    std::error_code ec; fs::rename(tmp, p, ec);
    return !ec;
}
static json parseArg(const std::string& s) {
    json j = json::parse(s, nullptr, false);
    for (int i = 0; i < 2 && j.is_string(); i++) j = json::parse(j.get<std::string>(), nullptr, false);
    return j;
}
static std::string unquote(std::string s) {
    while (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return s;
}
static std::string resVal(const StdLogosResult& r) { return r.value.is_string() ? r.value.get<std::string>() : std::string(); }
static std::string safeDirName(const std::string& s) {
    std::string out;
    for (char c : s) out += (isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '-';
    while (out.find("--") != std::string::npos) out.replace(out.find("--"), 2, "-");
    if (out.size() > 60) out.resize(60);
    return out.empty() ? "model" : out;
}
// Image type from magic bytes, not the file name.
static std::string sniffImage(const std::string& b) {
    if (b.size() > 8 && b.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0) return "image/png";
    if (b.size() > 3 && (unsigned char)b[0] == 0xff && (unsigned char)b[1] == 0xd8 && (unsigned char)b[2] == 0xff) return "image/jpeg";
    if (b.size() > 12 && b.compare(0, 4, "RIFF") == 0 && b.compare(8, 4, "WEBP") == 0) return "image/webp";
    return "";
}
static std::string lowerExt(const std::string& name) {
    auto i = name.find_last_of('.');
    std::string e = i == std::string::npos ? "" : name.substr(i + 1);
    for (auto& c : e) c = (char)tolower((unsigned char)c);
    return e;
}

SwampCoreImpl::~SwampCoreImpl() {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    *m_life = false;
    if (m_unsaved || m_dirty) saveLog();
    if (m_timer) { m_timer->stop(); delete m_timer; }
}
long long SwampCoreImpl::nowMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string SwampCoreImpl::fail(const std::string& why) { return json{{"ok", false}, {"error", why}}.dump(); }
std::string SwampCoreImpl::newId() {
    unsigned char b[16]; RAND_bytes(b, 16);
    static const char* X = "0123456789abcdef";
    std::string s; for (unsigned char c : b) { s += X[c >> 4]; s += X[c & 15]; }
    return s;
}
void SwampCoreImpl::onLoop(std::function<void()> fn) {
    if (m_timer) QTimer::singleShot(0, m_timer, std::move(fn));
    else QTimer::singleShot(0, std::move(fn));
}

// ---- persistence ----------------------------------------------------------------------------
void SwampCoreImpl::setupDataDir() {
    const char* ov = getenv("SWAMP_CORE_DATA");
    m_dataDir = ov && *ov ? ov : homeDir() + "/.swamp-core";
    const char* dl = getenv("SWAMP_DOWNLOADS");
    m_downloadsDir = dl && *dl ? dl : homeDir() + "/Swamp";
    std::error_code ec;
    fs::create_directories(m_dataDir + "/files", ec);
    fs::create_directories(m_dataDir + "/parts", ec);
    m_storageOk = !ec;
    if (ec) fprintf(stderr, "[swamp] cannot create %s: %s\n", m_dataDir.c_str(), ec.message().c_str());
    const char* hub = getenv("SWAMP_HUB");
    m_hub = hub && (std::string(hub) == "1" || std::string(hub) == "true");
}
std::string SwampCoreImpl::blobPath(const std::string& sha) const { return m_dataDir + "/files/" + sha; }
bool SwampCoreImpl::haveBlob(const std::string& sha) const { std::error_code ec; return fs::exists(blobPath(sha), ec); }
std::string SwampCoreImpl::storeBlob(const std::string& bytes) {
    std::string sha = sha256Hex(bytes);
    if (!haveBlob(sha)) writeFile(blobPath(sha), bytes);
    return sha;
}

void SwampCoreImpl::loadAll() {
    std::string s;
    json id = readFile(m_dataDir + "/identity.json", s) ? json::parse(s, nullptr, false) : json();
    logos_sync::Bytes priv;
    if (id.is_object() && id.contains("priv")) priv = logos_sync::fromHexB(id.value("priv", ""));
    if (priv.size() != 32) {
        priv = logos_sync::generatePrivateKey();
        writeFile(m_dataDir + "/identity.json", json{{"priv", logos_sync::toHexS(priv.data(), priv.size())}}.dump());
    }
    std::error_code pec;   // the signing key: owner-only
    fs::permissions(m_dataDir + "/identity.json", fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, pec);
    m_id = identityFrom(priv);
    m_log.clear(); m_logIds.clear();
    if (readFile(m_dataDir + "/catalog.json", s)) {
        json a = json::parse(s, nullptr, false);
        // checked once here (the file could have been edited); refold() then trusts m_log
        if (a.is_array()) for (const auto& j : a) { Event ev; if (eventFrom(j, ev) && admissible(ev) && m_logIds.insert(ev.id).second) m_log.push_back(ev); }
    }
    long long now = nowMs();
    for (const auto& e : m_log) if (e.dev == m_id.address || e.hlc.wall <= now + kMaxClockLeadMs) m_id.clock.receive(e.hlc);
    if (readFile(m_dataDir + "/jobs.json", s)) {
        json a = json::parse(s, nullptr, false);
        if (a.is_object()) for (auto it = a.begin(); it != a.end(); ++it) {
            const json& j = it.value();
            DownloadJob d; d.modelId = j.value("modelId", ""); d.v = j.value("v", 0); d.dir = j.value("dir", "");
            d.status = j.value("status", ""); d.error = j.value("error", "");
            if (!j.is_object()) continue;
            if (j.contains("files") && j["files"].is_array()) for (const auto& f : j["files"]) if (f.is_object()) d.files.push_back({str(f, "sha"), str(f, "name")});
            if (d.status == "fetching") d.status = "queued";   // resume after a restart
            m_jobs[it.key()] = d;
        }
    }
    if (readFile(m_dataDir + "/mycids.json", s)) {
        json a = json::parse(s, nullptr, false);
        if (a.is_object()) for (auto it = a.begin(); it != a.end(); ++it) if (it.value().is_string()) m_myCids[it.key()] = it.value().get<std::string>();
    }
}
void SwampCoreImpl::saveLog() {
    if (!m_storageOk || m_dataDir.empty()) return;
    json a = json::array();
    for (const auto& e : m_log) a.push_back(logos_sync::eventToJson(e));
    writeFile(m_dataDir + "/catalog.json", a.dump());
    m_lastSave = nowMs();
    m_unsaved = false;
}
void SwampCoreImpl::saveJobs() {
    json o = json::object();
    for (const auto& [k, d] : m_jobs) {
        json files = json::array();
        for (const auto& [sha, name] : d.files) files.push_back({{"sha", sha}, {"name", name}});
        o[k] = {{"modelId", d.modelId}, {"v", d.v}, {"dir", d.dir}, {"status", d.status}, {"error", d.error}, {"files", files}};
    }
    writeFile(m_dataDir + "/jobs.json", o.dump());
    json c = json::object();
    for (const auto& [sha, cid] : m_myCids) c[sha] = cid;
    writeFile(m_dataDir + "/mycids.json", c.dump());
}

// ---- catalogue ------------------------------------------------------------------------------
void SwampCoreImpl::refold() { m_cat = fold(m_log, true); m_dirty = false; }
bool SwampCoreImpl::ingest(const Event& e) {
    if (m_logIds.count(e.id) || !admissible(e)) return false;   // duplicates don't pay for a verify
    m_logIds.insert(e.id);
    m_log.push_back(e);
    // stay causally after what we've seen, but don't let one future-dated event drag our clock along
    if (e.hlc.wall <= nowMs() + kMaxClockLeadMs) m_id.clock.receive(e.hlc);
    m_dirty = true;
    return true;
}
// Local writes fold and save at once (the caller reads the result straight back); received events
// only mark the catalogue dirty and tick() folds once for the whole batch.
Event SwampCoreImpl::author(const std::string& type, const json& payload) {
    Event e = makeEvent(m_id, type, payload, nowMs(), newId());
    ingest(e);
    refold();
    saveLog();
    sendFrame(json{{"t", "ev"}, {"e", logos_sync::eventToJson(e)}});
    return e;
}
std::string SwampCoreImpl::nameOf(const std::string& address) {
    auto it = m_cat.profiles.find(address);
    std::string n = it == m_cat.profiles.end() ? "" : it->second.value("name", "");
    return n.empty() ? address.substr(0, 10) : n;
}
// Every blob this node is responsible for uploading: its own versions' files, images and
// fingerprints, and the photos of its makes.
std::set<std::string> SwampCoreImpl::myBlobs() {
    std::set<std::string> out;
    for (const auto& [id, m] : m_cat.models) {
        if (m.creator == m_id.address)
            for (const auto& v : m.versions) {
                for (const char* k : {"files", "images"}) if (v.contains(k)) for (const auto& f : v[k]) out.insert(f.value("sha256", ""));
                if (v.contains("fp") && v["fp"].is_object()) out.insert(v["fp"].value("sha256", ""));
            }
        for (const auto& mk : m.makes)
            if (mk.value("author", "") == m_id.address && mk.contains("images"))
                for (const auto& im : mk["images"]) out.insert(im.value("sha256", ""));
    }
    out.erase("");
    return out;
}

json SwampCoreImpl::card(const Model& m) {
    const json& v = m.versions.back();
    json thumb = nullptr;
    if (v.contains("images")) for (const auto& im : v["images"]) {
        std::string sha = im.value("sha256", "");
        if (haveBlob(sha)) { thumb = blobPath(sha); break; }
    }
    int made = (int)m.makes.size();
    return json{{"modelId", m.modelId}, {"title", v.value("title", "")}, {"summary", v.value("summary", "")},
                {"creator", m.creator}, {"creatorName", nameOf(m.creator)}, {"latest", m.versions.size()},
                {"licence", v.value("licence", "")}, {"tags", v.value("tags", json::array())},
                {"likes", likeCount(m)}, {"makes", made}, {"comments", m.comments.size()},
                {"published", v.value("published", 0LL)}, {"created", m.created}, {"thumb", thumb},
                {"mine", m.creator == m_id.address}, {"retracted", m.retracted},
                {"remix", v.contains("parents") && !v["parents"].empty()}};
}

// ---- transport ------------------------------------------------------------------------------
void SwampCoreImpl::sendFrame(const json& frame) {
    if (!m_ready) return;   // catch-up delivers anything we authored offline
    m_tx++;
    try { modules().loam_core.sendSealedAsync(CATALOG_TOPIC, b64std(frame.dump()), [](std::string) {}); }
    catch (const std::exception& e) { fprintf(stderr, "[swamp] send failed: %s\n", e.what()); }
}

// A catch-up frame from the wire, checked before logos_sync::catchup::respond() touches it
// (respond() assumes well-formed input and throws on anything else).
static bool wellFormedCatchup(const json& f) {
    auto strArr = [](const json& a, size_t max) {
        if (!a.is_array() || a.size() > max) return false;
        for (const auto& x : a) if (!x.is_string() || x.get_ref<const std::string&>().size() > 128) return false;
        return true;
    };
    for (const char* k : {"from", "lo", "hi"}) if (f.contains(k) && !f[k].is_string()) return false;
    const std::string t = str(f, "t");
    if (t == "fp") {
        if (!f.contains("fps") || !f.contains("bounds") || !strArr(f["fps"], 64) || !strArr(f["bounds"], 64)) return false;
        return !f["fps"].empty() && f["bounds"].size() + 1 == f["fps"].size();
    }
    if (t == "ids" || t == "need") return f.contains("ids") && strArr(f["ids"], 512);
    return false;
}

// Message timestamps come in s, ms, us or ns depending on the layer; 0 = unknown.
static long long toMs(int64_t t) {
    if (t <= 0) return 0;
    if (t > 100000000000000000LL) return t / 1000000;
    if (t > 100000000000000LL) return t / 1000;
    if (t > 100000000000LL) return t;
    return t * 1000;
}

void SwampCoreImpl::onFrame(const std::string& topic, const std::string& payloadB64, int64_t sentAt) {
    if (topic != CATALOG_TOPIC) return;
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    m_rx++;
    if (payloadB64.size() > 512 * 1024) { m_rxBad++; return; }
    std::string s = payloadB64, dec;
    json f;
    for (int i = 0; i < 3; i++) {
        size_t p = s.find_first_not_of(" \n\r\t");
        if (p != std::string::npos && s[p] == '{') { f = json::parse(s, nullptr, false); break; }
        if (!unb64(s, dec)) break;
        s = dec;
    }
    if (!f.is_object()) { m_rxBad++; return; }
    long long at = toMs(sentAt);
    bool live = at == 0 || nowMs() - at < kCatchupMaxAgeMs;
    try { handleFrame(f, live); }
    catch (const std::exception& e) { m_rxBad++; fprintf(stderr, "[swamp] bad frame dropped: %s\n", e.what()); }
}

void SwampCoreImpl::handleFrame(const json& f, bool live) {
    const std::string t = str(f, "t");
    if (t == "ev" || t == "evs") {
        json list = t == "ev" ? json::array({f.value("e", json())}) : f.value("es", json::array());
        if (!list.is_array()) { m_rxBad++; return; }
        for (const auto& j : list) {
            Event e;
            if (!eventFrom(j, e)) { m_rxBad++; continue; }
            if (ingest(e)) m_rxEvents++;
        }
    } else if (wellFormedCatchup(f)) {
        // Store replays old catch-up requests on every (re)connect: answering them is pure cost.
        // Events always count, whatever their age.
        if (!live) { m_staleCatchup++; return; }
        // A peer's opening fp gets one answer per kAnswerEveryMs; its follow-ups (ids/need) always do.
        const std::string from = str(f, "from");
        long long now = nowMs();
        if (str(f, "t") == "fp" && !f.contains("lo") && !f.contains("hi")) {
            auto a = m_answeredAt.find(from);
            if (a != m_answeredAt.end() && now - a->second < kAnswerEveryMs) { m_throttled++; return; }
            m_answeredAt[from] = now;
        }
        auto step = logos_sync::catchup::respond(m_log, f, m_id.address);
        for (const auto& r : step.replies) sendFrame(r);
        serveEvents(step.serve);
    } else m_rxBad++;
}

// Serve what a peer asked for in batches, within a budget: every node answering every request at
// full speed would flood the topic (and spend the RLN allowance). Anything cut off is asked
// for again in the peer's next catch-up round.
void SwampCoreImpl::serveEvents(const std::vector<Event>& evs) {
    long long now = nowMs();
    if (now - m_serveWindow > kServeWindowMs) { m_serveWindow = now; m_servedInWindow = 0; }
    json batch = json::array();
    size_t bytes = 0;
    auto flush = [&] { if (!batch.empty()) sendFrame(json{{"t", "evs"}, {"es", batch}}); batch = json::array(); bytes = 0; };
    for (const auto& e : evs) {
        if (m_servedInWindow >= kServePerWindow) { m_throttled += 1; continue; }
        json j = logos_sync::eventToJson(e);
        size_t n = j.dump().size();
        if (bytes + n > kFrameBytes) flush();
        batch.push_back(std::move(j));
        bytes += n;
        m_servedInWindow++;
        m_servedEvents++;
    }
    flush();
}

void SwampCoreImpl::catchupRound() {
    m_lastCatchup = nowMs();
    sendFrame(logos_sync::catchup::buildInitial(m_log, m_id.address));
}

// loam_core reports "Connected" by event; the event can arrive before we subscribed or get lost,
// so tick() also asks for the status until we're ready.
void SwampCoreImpl::onStatus(const std::string& s) {
    if (s.empty()) return;
    m_status = s;
    if (s == "Connected" && !m_ready) {
        m_ready = true;
        try { modules().loam_core.joinAsync(CATALOG_TOPIC, [](std::string) {}); } catch (...) {}
        for (int ms : {3000, 10000, 25000}) QTimer::singleShot(ms, m_timer, [this] { std::lock_guard<std::recursive_mutex> l(m_mtx); catchupRound(); });
    } else if (s == "Connected") {
        catchupRound();
    }
    publishState();
}

void SwampCoreImpl::startTransport() {
    if (m_transportStarted) return;
    m_transportStarted = true;
    json cfg = json{{"mode", "Core"}, {"preset", "logos.test"}, {"useChannels", true}};
    if (const char* env = getenv("SWAMP_DELIVERY_CFG")) {
        json p = json::parse(std::string(env), nullptr, false);
        if (p.is_object()) cfg = p;
    }
    try {
        // callbacks arrive on the IPC thread: hand the work to the module's loop
        auto life = m_life;
        modules().loam_core.onReceived([this, life](const std::string& topic, const std::string&, const std::string& payloadB64, int64_t at) {
            if (*life) onLoop([this, life, topic, payloadB64, at] { if (*life) onFrame(topic, payloadB64, at); });
        });
        modules().loam_core.onStatusChanged([this, life](const std::string& s) {
            if (*life) onLoop([this, life, s] { std::lock_guard<std::recursive_mutex> lk(m_mtx); if (*life) onStatus(s); });
        });
        modules().loam_core.setSenderIdAsync(m_id.address, [](std::string) {});
        modules().loam_core.startAsync(cfg.dump(), [this, life](std::string err) {
            if (!*life || err.empty() || err == "{}" || err.find("\"ok\":true") != std::string::npos) return;
            onLoop([this, err] { std::lock_guard<std::recursive_mutex> lk(m_mtx); if (!m_ready) { m_status = "Transport error: " + err; publishState(); } });
        });
        m_status = "Connecting...";
    } catch (const std::exception& e) {
        m_status = std::string("loam_core unavailable: ") + e.what();
    }
}

// ---- storage --------------------------------------------------------------------------------
void SwampCoreImpl::ensureStorage() {
    if (m_storageStarted) return;
    m_storageStarted = true;
    try {
        auto life = m_life;
        modules().storage_module.onStorageUploadDone([this, life](const std::string& payload) {
            if (*life) onLoop([this, payload] { std::lock_guard<std::recursive_mutex> lk(m_mtx); completeUpload(payload); });
        });
        modules().storage_module.onStorageDownloadDone([this, life](const std::string& payload) {
            if (*life) onLoop([this, payload] {
                std::lock_guard<std::recursive_mutex> lk(m_mtx);
                json p = json::parse(payload, nullptr, false);
                if (p.is_object()) completeDownload(str(p, "sessionId"), flag(p, "success"), str(p, "error"));
            });
        });
        // Basecamp 0.3 / logosctl own the Storage node and configure it from the host's
        // ~/.logos_storage/config.json. We never init() it - a successful init() would replace the
        // host's config for every module. Only with an explicit SWAMP_STORAGE_CFG (a host that
        // doesn't run Storage itself) do we configure the node.
        const char* own = getenv("SWAMP_STORAGE_CFG");
        bool inited = false;
        if (own && *own) {
            json cfg = json::parse(std::string(own), nullptr, false);
            if (cfg.is_object()) { try { inited = modules().storage_module.init(cfg.dump()); } catch (...) { inited = false; } }
        }
        m_storageHostOwned = !inited;
        try { modules().storage_module.start(); } catch (...) {}
        fprintf(stderr, "[swamp] storage: %s\n", m_storageHostOwned ? "using the host's node" : "configured from SWAMP_STORAGE_CFG");
    } catch (const std::exception& e) {
        fprintf(stderr, "[swamp] storage unavailable: %s\n", e.what());
    }
}

bool SwampCoreImpl::jobWaiting() {
    for (const auto& [key, j] : m_jobs) {
        if (j.status == "done" || j.status == "failed") continue;
        for (const auto& [sha, name] : j.files) {
            if (haveBlob(sha) || !m_cat.cids.count(sha)) continue;
            return true;   // even while it backs off: a preview start would hold Storage ~30 s
        }
    }
    return false;
}

bool SwampCoreImpl::storageFree() {
    if (m_storageBusy && nowMs() - m_storageBusySince > 2LL * kStorageTimeoutMs) m_storageBusy = false;   // a lost callback
    if (m_storageBusy) return false;
    m_storageBusy = true;
    m_storageBusySince = nowMs();
    return true;
}
void SwampCoreImpl::storageDone() { m_storageBusy = false; }

void SwampCoreImpl::uploadBlob(const std::string& sha) {
    if (m_myCids.count(sha) || !haveBlob(sha)) return;
    for (const auto& [sess, u] : m_upSessions) if (u.sha == sha) return;
    if (!storageFree()) return;   // retryUploads() comes back next tick
    m_upTried[sha] = nowMs();
    try {
        modules().storage_module.uploadUrlAsyncResult(blobPath(sha), 65536, true,
            [this, life = m_life, sha](logos::AsyncResult<StdLogosResult> ar) {
                if (*life) onLoop([this, ar, sha] {
                    std::lock_guard<std::recursive_mutex> lk(m_mtx);
                    storageDone();
                    // a timeout may mean "still running": the manifest poll finds it by file name
                    if (!ar.ok()) { m_upSessions["?" + sha] = PendingUpload{sha, nowMs()}; return; }
                    if (!ar.value.success || resVal(ar.value).empty()) {
                        fprintf(stderr, "[swamp] upload %s refused: %s (retrying later)\n", sha.substr(0, 12).c_str(), ar.value.error.c_str());
                        return;
                    }
                    m_upSessions[resVal(ar.value)] = PendingUpload{sha, nowMs()};
                });
            }, kStorageTimeoutMs);
    } catch (const std::exception& e) { storageDone(); fprintf(stderr, "[swamp] upload failed: %s\n", e.what()); }
}

void SwampCoreImpl::completeUpload(const std::string& payload) {
    json p = json::parse(payload, nullptr, false);
    if (!p.is_object()) return;
    auto it = m_upSessions.find(str(p, "sessionId"));
    if (it == m_upSessions.end()) return;
    std::string sha = it->second.sha;
    m_upSessions.erase(it);
    std::string cid = str(p, "cid");
    if (!flag(p, "success") || cid.empty()) {
        fprintf(stderr, "[swamp] upload of %s failed: %s (retrying later)\n", sha.substr(0, 12).c_str(), str(p, "error").c_str());
        return;
    }
    m_uploaded++;
    m_myCids[sha] = cid;
    m_toAnnounce[sha] = cid;
    saveJobs();
}

// Retry any of my blobs that has no CID and no upload in flight (refused, failed, timed out, or
// the app closed mid-upload).
void SwampCoreImpl::retryUploads() {
    long long now = nowMs();
    for (const auto& sha : myBlobs()) {
        if (m_myCids.count(sha)) continue;
        auto t = m_upTried.find(sha);
        if (t != m_upTried.end() && now - t->second < kUploadRetryMs) continue;
        uploadBlob(sha);
    }
}

// CIDs go out as one blob.cids event per batch, not one per upload, and a CID the catalogue still
// lacks is re-announced at most every kReannounceMs (the fold may legitimately drop it).
void SwampCoreImpl::flushAnnouncements() {
    if (!m_ready) return;
    long long now = nowMs();
    for (const auto& [sha, cid] : m_myCids) {
        auto it = m_cat.cids.find(sha);
        bool known = it != m_cat.cids.end() && std::find(it->second.begin(), it->second.end(), cid) != it->second.end();
        if (known) continue;
        auto a = m_announcedAt.find(sha);
        if (a == m_announcedAt.end() || now - a->second > kReannounceMs) m_toAnnounce[sha] = cid;
    }
    if (m_toAnnounce.empty()) { m_announceHeldSince = 0; return; }
    // while more of my uploads are still finishing, wait a little so they share one event
    bool more = !m_upSessions.empty();
    for (const auto& sha : myBlobs()) if (!more && !m_myCids.count(sha)) more = true;
    if (!m_announceHeldSince) m_announceHeldSince = now;
    if (more && now - m_announceHeldSince < kAnnounceHoldMs) return;
    m_announceHeldSince = 0;
    while (!m_toAnnounce.empty()) {
        json m = json::object();
        for (auto it = m_toAnnounce.begin(); it != m_toAnnounce.end() && m.size() < kCidsPerEvent;) {
            m[it->first] = it->second;
            m_announcedAt[it->first] = now;
            it = m_toAnnounce.erase(it);
        }
        author("blob.cids", json{{"cids", m}});
    }
    publishState();
}

// 15 s, 30 s, 1 min, ... 30 min. Short at first: a fresh upload often isn't findable in the DHT
// for the first tens of seconds ("failed to get manifest"), then is.
static long long backoffMs(int rounds) { return std::min<long long>(15000LL << std::min(std::max(rounds - 1, 0), 7), 30LL * 60 * 1000); }

static bool sha256File(const std::string& path, std::string& hex) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    std::vector<char> buf(1 << 16);
    while (f) {
        f.read(buf.data(), buf.size());
        if (f.gcount() > 0) EVP_DigestUpdate(ctx, buf.data(), (size_t)f.gcount());
    }
    unsigned char md[32]; unsigned int n = 0;
    EVP_DigestFinal_ex(ctx, md, &n);
    EVP_MD_CTX_free(ctx);
    static const char* X = "0123456789abcdef";
    hex.clear();
    for (unsigned i = 0; i < n; i++) { hex += X[md[i] >> 4]; hex += X[md[i] & 15]; }
    return true;
}

// Fetch one blob into the local cache: try its candidate CIDs in turn, verify the hash, back off
// between full rounds. `size` is the size the signed version declares; a transfer that grows past
// it is cut off.
void SwampCoreImpl::startFetch(const std::string& sha, long long size) {
    if (haveBlob(sha) || size <= 0 || size > MAX_FILE_BYTES) return;
    auto it = m_cat.cids.find(sha);
    if (it == m_cat.cids.end() || it->second.empty()) return;
    Fetch& F = m_fetch[sha];
    long long now = nowMs();
    if (F.inflight || F.gaveUp || now < F.nextTry) return;
    int inflight = 0;
    for (const auto& [s, f] : m_fetch) inflight += f.inflight;
    if (inflight >= kMaxFetches || !storageFree()) return;
    if (F.cidIdx >= it->second.size()) F.cidIdx = 0;
    std::string cid = it->second[F.cidIdx];
    std::string part = m_dataDir + "/parts/" + sha;
    std::error_code ec; fs::remove(part, ec);
    F.inflight = true; F.since = now; F.size = size; F.cid = cid; F.session.clear(); F.seenSize = 0; F.grewAt = now;
    long long attempt = now;
    try {
        modules().storage_module.downloadToUrlAsyncResult(cid, part, false, 65536, false, true,
            [this, life = m_life, sha, attempt](logos::AsyncResult<StdLogosResult> ar) {
                if (*life) onLoop([this, ar, sha, attempt] {
                    std::lock_guard<std::recursive_mutex> lk(m_mtx);
                    storageDone();
                    auto fi = m_fetch.find(sha);
                    if (fi == m_fetch.end() || !fi->second.inflight || fi->second.since != attempt) return;   // stale
                    if (!ar.ok()) return;   // a timeout may mean "still running": pollStorage watches the file
                    if (!ar.value.success) { fetchFailed(sha, ar.value.error.empty() ? "Storage refused the download" : ar.value.error); return; }
                    fi->second.session = resVal(ar.value);
                });
            }, kStorageTimeoutMs);
    } catch (const std::exception& e) {
        storageDone();
        fetchFailed(sha, std::string("download failed to start: ") + e.what());
    }
}

void SwampCoreImpl::fetchFailed(const std::string& sha, const std::string& why) {
    auto fi = m_fetch.find(sha);
    if (fi == m_fetch.end()) return;
    Fetch& F = fi->second;
    std::string sess = F.session.empty() ? F.cid : F.session;
    if (F.inflight && !sess.empty()) { try { modules().storage_module.downloadCancelAsyncResult(sess, [](logos::AsyncResult<StdLogosResult>) {}, kStorageTimeoutMs); } catch (...) {} }
    std::error_code ec; fs::remove(m_dataDir + "/parts/" + sha, ec);
    F.inflight = false; F.session.clear(); F.error = why;
    fprintf(stderr, "[swamp] fetch %s: %s\n", sha.substr(0, 12).c_str(), why.c_str());
    auto it = m_cat.cids.find(sha);
    size_t n = it == m_cat.cids.end() ? 0 : it->second.size();
    if (++F.cidIdx >= n) { F.cidIdx = 0; F.rounds++; F.nextTry = nowMs() + backoffMs(F.rounds); }
    else F.nextTry = 0;   // another candidate: try it straight away
}

void SwampCoreImpl::completeDownload(const std::string& sessionId, bool success, const std::string& error) {
    if (sessionId.empty()) return;
    // storage 3.x: the download session id is the CID, and the done event can beat the
    // AsyncResult that tells us the session id - match either
    for (auto& [sha, F] : m_fetch) {
        if (!F.inflight || (F.session != sessionId && F.cid != sessionId)) continue;
        if (success) finishFetched(sha);
        else fetchFailed(sha, error.empty() ? "download failed" : error);
        return;
    }
}

void SwampCoreImpl::finishFetched(const std::string& sha) {
    std::string part = m_dataDir + "/parts/" + sha, got;
    std::error_code ec;
    long long sz = fs::exists(part, ec) ? (long long)fs::file_size(part, ec) : -1;
    auto fi = m_fetch.find(sha);
    long long want = fi == m_fetch.end() ? 0 : fi->second.size;
    if (sz != want || !sha256File(part, got) || got != sha) {
        m_verifyFailed++;
        fetchFailed(sha, "the downloaded bytes don't match the published hash - rejected");
        return;
    }
    fs::rename(part, blobPath(sha), ec);
    if (ec) { fetchFailed(sha, "couldn't store the file: " + ec.message()); return; }
    m_fetched++;
    m_fetch.erase(sha);
    publishState();
}

// Storage done-events can be lost: read the same facts back (upload: the manifest of our staged
// file name; download: the part file reached the declared size).
void SwampCoreImpl::pollStorage() {
    long long now = nowMs();
    if (!m_upSessions.empty() && now - m_lastManifestPoll > kManifestPollMs && storageFree()) {
        m_lastManifestPoll = now;
        try {
            modules().storage_module.manifestsAsyncResult([this, life = m_life](logos::AsyncResult<StdLogosResult> ar) {
                if (*life) onLoop([this, ar] {
                    std::lock_guard<std::recursive_mutex> lk(m_mtx);
                    storageDone();
                    if (!ar.ok() || !ar.value.success) return;
                    json arr = ar.value.value;
                    if (arr.is_string()) arr = json::parse(arr.get<std::string>(), nullptr, false);
                    if (!arr.is_array()) return;
                    std::vector<std::string> done;
                    for (const auto& [sess, u] : m_upSessions)
                        for (const auto& m : arr)
                            if (m.is_object() && str(m, "filename") == u.sha && !str(m, "cid").empty())
                                done.push_back(json{{"sessionId", sess}, {"success", true}, {"cid", str(m, "cid")}}.dump());
                    for (const auto& d : done) completeUpload(d);
                });
            }, kStorageTimeoutMs);
        } catch (...) { storageDone(); }
        for (auto it = m_upSessions.begin(); it != m_upSessions.end();)
            it = now - it->second.since > kTransferStaleMs ? m_upSessions.erase(it) : std::next(it);
    }
    std::vector<std::pair<std::string, std::string>> failed;
    std::vector<std::string> finished;
    for (auto& [sha, F] : m_fetch) {
        if (!F.inflight) continue;
        std::error_code ec;
        std::string part = m_dataDir + "/parts/" + sha;
        long long sz = fs::exists(part, ec) ? (long long)fs::file_size(part, ec) : -1;
        if (sz == F.size) finished.push_back(sha);
        else if (sz > F.size) { m_tooBig++; failed.push_back({sha, "the download is larger than the published size - cut off"}); }
        else if (now - F.since > kTransferStaleMs) failed.push_back({sha, "timed out"});
        else if (sz > F.seenSize) { F.seenSize = sz; F.grewAt = now; }
        else if (now - F.grewAt > kStallMs) { m_stalled++; failed.push_back({sha, "the transfer stalled (no holder answering)"}); }
    }
    for (const auto& s : finished) finishFetched(s);
    for (const auto& [s, why] : failed) fetchFailed(s, why);
}

static long long declaredSize(const Catalog& cat, const std::string& modelId, int v, const std::string& sha) {
    auto mi = cat.models.find(modelId);
    if (mi == cat.models.end() || v < 1 || v > (int)mi->second.versions.size()) return 0;
    for (const auto& f : mi->second.versions[v - 1]["files"]) if (f.value("sha256", "") == sha) return f.value("size", 0LL);
    return 0;
}

void SwampCoreImpl::advanceJobs() {
    bool changed = false;
    for (auto& [key, j] : m_jobs) {
        if (j.status == "done" || j.status == "failed") continue;
        size_t have = 0;
        std::string prevStatus = j.status, prevError = j.error;
        j.error.clear();
        for (const auto& [sha, name] : j.files) {
            if (haveBlob(sha)) { have++; continue; }
            startFetch(sha, declaredSize(m_cat, j.modelId, j.v, sha));
            auto fi = m_fetch.find(sha);
            if (fi == m_fetch.end()) continue;
            if (fi->second.rounds >= kJobRounds) { j.status = "failed"; j.error = "Couldn't fetch " + name + ": " + fi->second.error; break; }
            if (!fi->second.error.empty() && j.error.empty()) j.error = "Retrying " + name + ": " + fi->second.error;
        }
        if (j.status != "failed") {
            if (have == j.files.size()) {
                std::error_code ec;
                fs::create_directories(j.dir, ec);
                std::string err = ec ? ec.message() : "";
                for (const auto& [sha, name] : j.files) {
                    if (!err.empty()) break;
                    fs::copy_file(blobPath(sha), j.dir + "/" + name, fs::copy_options::overwrite_existing, ec);
                    if (ec) err = name + ": " + ec.message();
                }
                j.status = err.empty() ? "done" : "failed";
                j.error = err.empty() ? "" : "Couldn't write to " + j.dir + " (" + err + ")";
            } else j.status = "fetching";
        }
        if (j.status != prevStatus || j.error != prevError) changed = true;
    }
    if (changed) { saveJobs(); publishState(); }
}

// Thumbnails and photos are small: fetch them eagerly so listings have pictures.
void SwampCoreImpl::fetchPreviews() {
    int inflight = 0;
    for (const auto& [s, f] : m_fetch) inflight += f.inflight;
    for (const auto& [id, m] : m_cat.models) {
        if (m.versions.empty() || m.retracted) continue;
        const json& v = m.versions.back();
        if (!v.contains("images")) continue;
        for (const auto& im : v["images"]) {
            if (inflight >= kPreviewFetches) return;
            std::string sha = im.value("sha256", "");
            long long size = im.value("size", 0LL);
            if (size > kPreviewMaxBytes || haveBlob(sha) || !m_cat.cids.count(sha)) continue;
            auto fi = m_fetch.find(sha);
            if (fi != m_fetch.end() && (fi->second.inflight || nowMs() < fi->second.nextTry)) continue;
            startFetch(sha, size);
            if (m_fetch.count(sha) && m_fetch[sha].inflight) inflight++;
        }
    }
}

// A hub keeps a copy of every file it sees, so files outlive their creators' desktops.
void SwampCoreImpl::hubSweep() {
    if (!m_hub) return;
    int inflight = 0;
    for (const auto& [s, f] : m_fetch) inflight += f.inflight;
    for (const auto& [id, m] : m_cat.models) {
        for (const auto& v : m.versions) {
            json blobs = json::array();
            for (const char* k : {"files", "images"}) if (v.contains(k)) for (const auto& f : v[k]) blobs.push_back(f);
            if (v.contains("fp") && v["fp"].is_object()) blobs.push_back(v["fp"]);
            for (const auto& f : blobs) {
                if (inflight >= kHubConcurrency) return;
                std::string sha = f.value("sha256", "");
                if (haveBlob(sha) || !m_cat.cids.count(sha)) continue;
                startFetch(sha, f.value("size", 0LL));
                if (m_fetch.count(sha) && m_fetch[sha].inflight) inflight++;
            }
        }
    }
}

void SwampCoreImpl::tick() {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!m_loaded) return;
    try {
        long long now = nowMs();
        if (m_dirty) { refold(); m_unsaved = true; publishState(); }
        if (m_unsaved && now - m_lastSave > kSaveEveryMs) saveLog();
        if (!m_ready && m_transportStarted && now - m_lastStatusPoll > kStatusPollMs) {
            m_lastStatusPoll = now;
            try {
                modules().loam_core.statusAsync([this, life = m_life](std::string s) {
                    if (*life) onLoop([this, s] { std::lock_guard<std::recursive_mutex> l(m_mtx); if (!m_ready) onStatus(unquote(s)); });
                });
            } catch (...) {}
        }
        if (m_storageStarted) {
            pollStorage();
            advanceJobs();
            retryUploads();
            // a download the user asked for goes first: no background fetches until it's done
            if (!jobWaiting()) { fetchPreviews(); hubSweep(); }
        }
        flushAnnouncements();
        if (m_ready && now - m_lastCatchup > kCatchupEveryMs) catchupRound();
    } catch (const std::exception& e) {
        fprintf(stderr, "[swamp] tick: %s\n", e.what());
    }
}

void SwampCoreImpl::startModules() {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    startTransport();
    ensureStorage();
    retryUploads();   // anything that never got a CID (e.g. the app closed mid-upload)
}

void SwampCoreImpl::onContextReady() {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    setupDataDir();
    m_timer = new QTimer();
    QObject::connect(m_timer, &QTimer::timeout, m_timer, [this] { tick(); });
    int tickMs = 2000;
    if (const char* t = getenv("SWAMP_TICK_MS")) tickMs = std::max(50, atoi(t));
    m_timer->start(tickMs);
    // loading verifies every stored signature (~0.5 ms each): do it just after the hook returns
    QTimer::singleShot(0, m_timer, [this] { std::lock_guard<std::recursive_mutex> l(m_mtx); loadAll(); refold(); m_loaded = true; publishState(); });
    QTimer::singleShot(kStartDelayMs, m_timer, [this] { startModules(); });
}

// ---- read surface -----------------------------------------------------------------------------
void SwampCoreImpl::publishState() {
    try { stateChanged(json{{"events", m_log.size()}, {"models", m_cat.models.size()}, {"status", m_status}}.dump()); } catch (...) {}
}

std::string SwampCoreImpl::snapshot() {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    auto pit = m_cat.profiles.find(m_id.address);
    json profile = pit == m_cat.profiles.end() ? json{{"name", ""}, {"bio", ""}} : pit->second;
    size_t visible = 0;
    for (const auto& [id, m] : m_cat.models) if (!m.versions.empty() && !m.retracted) visible++;
    size_t inflight = 0;
    for (const auto& [s, f] : m_fetch) inflight += f.inflight;
    return json{{"ok", true}, {"version", SWAMP_VERSION}, {"status", m_status}, {"hub", m_hub},
                {"me", {{"address", m_id.address}, {"profile", profile}}},
                {"storage", {{"hostOwned", m_storageHostOwned}, {"started", m_storageStarted}, {"dataOk", m_storageOk}, {"downloads", m_downloadsDir}}},
                {"catalog", {{"events", m_log.size()}, {"models", visible}, {"rejected", m_cat.rejected}, {"cids", m_cat.cids.size()}}},
                {"counters", {{"rx", m_rx}, {"tx", m_tx}, {"rxEvents", m_rxEvents}, {"rxBad", m_rxBad}, {"uploaded", m_uploaded},
                              {"fetched", m_fetched}, {"verifyFailed", m_verifyFailed}, {"tooBig", m_tooBig}, {"served", m_servedEvents}, {"throttled", m_throttled}, {"staleCatchup", m_staleCatchup}, {"stalled", m_stalled},
                              {"uploading", m_upSessions.size()}, {"downloading", inflight}, {"toAnnounce", m_toAnnounce.size()}}}}.dump();
}

std::string SwampCoreImpl::resync() {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (m_ready) catchupRound();
    return snapshot();
}

std::string SwampCoreImpl::listModels(std::string queryJson) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    json q = parseArg(queryJson);
    if (!q.is_object()) q = json::object();
    std::string text = str(q, "q"), tag = str(q, "tag"), sort = (str(q, "sort").empty() ? std::string("new") : str(q, "sort"));
    bool mineOnly = flag(q, "mine");
    size_t limit = (size_t)std::max(1, std::min(500, (int)std::max(-1000LL, std::min(1000LL, num(q, "limit", 100)))));
    std::vector<const Model*> hits;
    for (const auto& [id, m] : m_cat.models) {
        if (m.versions.empty() || m.retracted) continue;
        if (mineOnly && m.creator != m_id.address) continue;
        if (matches(m, text, tag)) hits.push_back(&m);
    }
    std::sort(hits.begin(), hits.end(), [&](const Model* a, const Model* b) {
        if (sort == "likes" && likeCount(*a) != likeCount(*b)) return likeCount(*a) > likeCount(*b);
        if (sort == "makes" && a->makes.size() != b->makes.size()) return a->makes.size() > b->makes.size();
        return a->versions.back().value("published", 0LL) > b->versions.back().value("published", 0LL);
    });
    json out = json::array();
    std::map<std::string, int> tags;
    for (const auto* m : hits) {
        if (out.size() < limit) out.push_back(card(*m));
        for (const auto& t : m->versions.back().value("tags", json::array())) tags[t.get<std::string>()]++;
    }
    json tagList = json::array();
    std::vector<std::pair<std::string, int>> tv(tags.begin(), tags.end());
    std::sort(tv.begin(), tv.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < tv.size() && i < 30; i++) tagList.push_back({{"tag", tv[i].first}, {"count", tv[i].second}});
    return ok(json{{"models", out}, {"total", hits.size()}, {"tags", tagList}});
}

std::string SwampCoreImpl::getModel(std::string modelId) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    modelId = unquote(modelId);
    auto it = m_cat.models.find(modelId);
    if (it == m_cat.models.end() || it->second.versions.empty()) return fail("No such model (it may not have synced yet)");
    const Model& m = it->second;
    json versions = json::array();
    for (const auto& v : m.versions) {
        json vv = v;
        for (const char* k : {"files", "images"}) {
            if (!vv.contains(k)) continue;
            for (auto& f : vv[k]) {
                std::string sha = f.value("sha256", "");
                f["local"] = haveBlob(sha) ? json(blobPath(sha)) : json(nullptr);
                f["cids"] = m_cat.cids.count(sha) ? m_cat.cids.at(sha).size() : 0;
                auto fi = m_fetch.find(sha);
                f["fetching"] = fi != m_fetch.end() && fi->second.inflight;
                if (fi != m_fetch.end() && !fi->second.error.empty()) f["fetchError"] = fi->second.error;
            }
        }
        vv.erase("fp");
        std::string key = modelId + "@" + std::to_string(v.value("v", 0));
        auto jt = m_jobs.find(key);
        if (jt != m_jobs.end()) vv["download"] = {{"status", jt->second.status}, {"dir", jt->second.dir}, {"error", jt->second.error}};
        versions.push_back(vv);
    }
    auto withNames = [&](const std::vector<json>& items) {
        json a = json::array();
        for (const auto& i : items) {
            json x = i; x["authorName"] = nameOf(i.value("author", ""));
            if (x.contains("images")) for (auto& im : x["images"]) { std::string sha = im.value("sha256", ""); im["local"] = haveBlob(sha) ? json(blobPath(sha)) : json(nullptr); }
            a.push_back(x);
        }
        return a;
    };
    bool liked = m.likes.count(m_id.address) && m.likes.at(m_id.address);
    json remixes = json::array();
    for (const auto& [oid, om] : m_cat.models) {
        if (om.versions.empty() || om.retracted) continue;
        for (const auto& p : om.versions.back().value("parents", json::array()))
            if (p.value("modelId", "") == modelId) { remixes.push_back(card(om)); break; }
    }
    json c = card(m);
    c["versions"] = versions;
    c["comments"] = withNames(m.comments);
    c["makesList"] = withNames(m.makes);
    c["likedByMe"] = liked;
    c["remixes"] = remixes;
    c["retractReason"] = m.retractReason;
    return ok(json{{"model", c}});
}

// ---- creators -------------------------------------------------------------------------------
std::string SwampCoreImpl::publish(std::string draftJson) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!m_loaded) return fail("Swamp is still starting - try again in a moment");
    json d = parseArg(draftJson);
    if (!d.is_object()) return fail("The draft is not valid JSON");
    if (!d.contains("files") || !d["files"].is_array() || d["files"].empty()) return fail("Add at least one file");
    std::string modelId = str(d, "modelId");
    const Model* existing = nullptr;
    if (!modelId.empty()) {
        auto it = m_cat.models.find(modelId);
        if (it == m_cat.models.end()) return fail("That model doesn't exist");
        if (it->second.creator != m_id.address) return fail("Only the model's creator can publish a new version - remix it instead");
        existing = &it->second;
    }
    // read + hash every file locally first (local-first: the event goes out before any upload)
    json files = json::array(), images = json::array();
    std::vector<double> firstMesh;
    for (const auto& f : d["files"]) {
        if (!f.is_object()) return fail("Each file needs a path");
        std::string path = str(f, "path"), bytes;
        if (!readFile(path, bytes) || bytes.empty()) return fail("Can't read " + path);
        std::string name = fs::path(path).filename().string();
        if (!safeName(name)) return fail("Unsupported file name: " + name);
        std::string sha = storeBlob(bytes);
        std::string kind = str(f, "kind");
        std::string ext = lowerExt(name);
        if (kind.empty()) kind = (ext == "stl" || ext == "3mf" || ext == "obj") ? "model" : (ext == "step" || ext == "stp" || ext == "scad" || ext == "f3d") ? "source" : "other";
        files.push_back({{"name", name}, {"kind", kind}, {"size", (long long)bytes.size()}, {"sha256", sha}});
        if (firstMesh.empty() && ext == "stl") { try { firstMesh = fp::parseStl(bytes); } catch (...) {} }
    }
    if (d.contains("images") && d["images"].is_array())
        for (const auto& im : d["images"]) {
            if (!im.is_object()) return fail("Each image needs a path");
            std::string path = str(im, "path"), bytes;
            if (!readFile(path, bytes) || bytes.empty()) return fail("Can't read image " + path);
            std::string mime = sniffImage(bytes);
            if (mime.empty()) return fail("Images must be PNG, JPEG or WebP");
            images.push_back({{"kind", (str(im, "kind").empty() ? std::string("photo") : clip(im, "kind", 16))}, {"size", (long long)bytes.size()}, {"sha256", storeBlob(bytes)}, {"mime", mime}});
        }
    json fpj = nullptr;
    if (!firstMesh.empty()) {
        try {
            // thumbnail first in the list, so listings show it
            std::string png = thumb::render(firstMesh, 256);
            images.insert(images.begin(), json{{"kind", "thumb"}, {"size", (long long)png.size()}, {"sha256", storeBlob(png)}, {"mime", "image/png"}});
            fp::Fingerprint F = fp::fingerprint(firstMesh);
            json full{{"v", fp::VERSION}, {"d2", F.d2}, {"a3", F.a3}, {"f3", F.f3}};
            std::string fsha = storeBlob(full.dump());
            auto r4 = [](double x) { return std::round(x * 10000) / 10000; };
            json d2 = json::array(), a3 = json::array();
            for (double x : F.d2) d2.push_back(r4(x));
            for (double x : F.a3) a3.push_back(r4(x));
            fpj = json{{"v", fp::VERSION}, {"sha256", fsha}, {"size", (long long)full.dump().size()}, {"d2", d2}, {"a3", a3}};
        } catch (const std::exception& e) { fprintf(stderr, "[swamp] fingerprint/thumbnail skipped: %s\n", e.what()); }
    }
    std::string nonce;
    if (modelId.empty()) { nonce = newId(); modelId = modelIdFor(m_id.address, nonce); }
    int v = existing ? (int)existing->versions.size() + 1 : 1;
    json ver{{"modelId", modelId}, {"v", v}, {"title", str(d, "title")}, {"summary", str(d, "summary")},
             {"description", str(d, "description")}, {"tags", d.contains("tags") ? d["tags"] : json::array()},
             {"licence", str(d, "licence")}, {"parents", d.contains("parents") ? d["parents"] : json::array()},
             {"files", files}, {"images", images}};
    if (!fpj.is_null()) ver["fp"] = fpj;
    std::string why = validateVersion(ver);
    if (!why.empty()) return fail(why);
    if (ver.dump().size() > MAX_PAYLOAD) return fail("The description and metadata are too long");
    // only now, with a version known to be valid, does the model come into existence
    if (!nonce.empty()) author("model.create", json{{"modelId", modelId}, {"nonce", nonce}, {"title", str(d, "title")}});
    author("model.version", ver);
    // uploads start from tick() (retryUploads), never on this IPC call; CIDs are announced in batches
    publishState();
    return ok(json{{"modelId", modelId}, {"v", v}});
}

std::string SwampCoreImpl::retract(std::string modelId, std::string reason) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!m_loaded) return fail("Swamp is still starting - try again in a moment");
    modelId = unquote(modelId);
    auto it = m_cat.models.find(modelId);
    if (it == m_cat.models.end()) return fail("No such model");
    if (it->second.creator != m_id.address) return fail("Only the creator can retract a model");
    author("model.retract", json{{"modelId", modelId}, {"reason", unquote(reason)}});
    return ok();
}

// ---- makers ---------------------------------------------------------------------------------
std::string SwampCoreImpl::download(std::string modelId, std::string version) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!m_loaded) return fail("Swamp is still starting - try again in a moment");
    modelId = unquote(modelId);
    auto it = m_cat.models.find(modelId);
    if (it == m_cat.models.end() || it->second.versions.empty()) return fail("No such model");
    int v = atoi(unquote(version).c_str());
    if (v <= 0) v = (int)it->second.versions.size();
    if (v > (int)it->second.versions.size()) return fail("No such version");
    const json& ver = it->second.versions[v - 1];
    DownloadJob j;
    j.modelId = modelId; j.v = v; j.status = "queued";
    j.dir = m_downloadsDir + "/" + safeDirName(ver.value("title", "model")) + "-" + modelId.substr(0, 8) + "-v" + std::to_string(v);
    for (const auto& f : ver["files"]) {
        std::string sha = f.value("sha256", "");
        j.files.push_back({sha, f.value("name", "")});
        auto fi = m_fetch.find(sha);   // asking again starts over: fresh rounds, no back-off
        if (fi != m_fetch.end() && !fi->second.inflight) m_fetch.erase(fi);
    }
    m_jobs[modelId + "@" + std::to_string(v)] = j;
    saveJobs();
    advanceJobs();
    return ok(json{{"dir", j.dir}});
}

std::string SwampCoreImpl::comment(std::string modelId, std::string text) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!m_loaded) return fail("Swamp is still starting - try again in a moment");
    modelId = unquote(modelId); text = unquote(text);
    if (!m_cat.models.count(modelId)) return fail("No such model");
    if (text.empty() || text.size() > 4000) return fail("Comments are 1-4000 characters");
    author("comment.post", json{{"modelId", modelId}, {"text", text}});
    return ok();
}

std::string SwampCoreImpl::postMake(std::string modelId, std::string makeJson) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!m_loaded) return fail("Swamp is still starting - try again in a moment");
    modelId = unquote(modelId);
    if (!m_cat.models.count(modelId)) return fail("No such model");
    json mk = parseArg(makeJson);
    if (!mk.is_object()) return fail("The make is not valid JSON");
    json imgs = json::array();
    if (mk.contains("images") && mk["images"].is_array())
        for (const auto& p : mk["images"]) {
            std::string bytes;
            if (!p.is_string() || !readFile(p.get<std::string>(), bytes) || bytes.empty()) return fail("Can't read a photo");
            std::string mime = sniffImage(bytes);
            if (mime.empty()) return fail("Photos must be PNG, JPEG or WebP");
            std::string sha = storeBlob(bytes);
            imgs.push_back({{"sha256", sha}, {"size", (long long)bytes.size()}, {"mime", mime}});
        }
    json payload{{"modelId", modelId}, {"text", str(mk, "text")}, {"images", imgs}};
    if (mk.contains("v") && mk["v"].is_number_integer()) payload["v"] = mk["v"];
    if (payload["text"].get<std::string>().empty() && imgs.empty()) return fail("Add a photo or a few words about your print");
    author("make.post", payload);
    return ok();
}

std::string SwampCoreImpl::like(std::string modelId, std::string on) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!m_loaded) return fail("Swamp is still starting - try again in a moment");
    modelId = unquote(modelId); on = unquote(on);
    if (!m_cat.models.count(modelId)) return fail("No such model");
    bool b = !(on == "false" || on == "0" || on == "no" || on == "off");
    author("like.put", json{{"modelId", modelId}, {"on", b}});
    return ok();
}

std::string SwampCoreImpl::setProfile(std::string profileJson) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!m_loaded) return fail("Swamp is still starting - try again in a moment");
    json p = parseArg(profileJson);
    if (!p.is_object()) return fail("The profile is not valid JSON");
    std::string name = str(p, "name");
    if (name.empty() || name.size() > 60) return fail("Pick a display name (1-60 characters)");
    author("profile.put", json{{"name", name}, {"bio", str(p, "bio")}});
    return ok();
}

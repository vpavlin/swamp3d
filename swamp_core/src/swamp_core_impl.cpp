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
static constexpr int kHubConcurrency = 3;

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
static std::string realHome() {
    if (struct passwd* pw = getpwuid(getuid())) if (pw->pw_dir && *pw->pw_dir == '/') return pw->pw_dir;
    const char* h = getenv("HOME");
    return h ? h : "/tmp";
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

SwampCoreImpl::~SwampCoreImpl() { if (m_timer) { m_timer->stop(); delete m_timer; } }
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
    m_dataDir = ov && *ov ? ov : realHome() + "/.swamp-core";
    const char* dl = getenv("SWAMP_DOWNLOADS");
    m_downloadsDir = dl && *dl ? dl : realHome() + "/Swamp";
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
    m_id = identityFrom(priv);
    m_log.clear(); m_logIds.clear();
    if (readFile(m_dataDir + "/catalog.json", s)) {
        json a = json::parse(s, nullptr, false);
        if (a.is_array()) for (const auto& e : a) { Event ev = logos_sync::eventFromJson(e); if (m_logIds.insert(ev.id).second) m_log.push_back(ev); }
    }
    for (const auto& e : m_log) if (e.dev == m_id.address) m_id.clock.receive(e.hlc);
    if (readFile(m_dataDir + "/jobs.json", s)) {
        json a = json::parse(s, nullptr, false);
        if (a.is_object()) for (auto it = a.begin(); it != a.end(); ++it) {
            const json& j = it.value();
            DownloadJob d; d.modelId = j.value("modelId", ""); d.v = j.value("v", 0); d.dir = j.value("dir", "");
            d.status = j.value("status", ""); d.error = j.value("error", "");
            if (j.contains("files")) for (const auto& f : j["files"]) d.files.push_back({f.value("sha", ""), f.value("name", "")});
            if (d.status == "fetching") d.status = "queued";   // resume after a restart
            m_jobs[it.key()] = d;
        }
    }
    if (readFile(m_dataDir + "/mycids.json", s)) {
        json a = json::parse(s, nullptr, false);
        if (a.is_object()) for (auto it = a.begin(); it != a.end(); ++it) m_myCids[it.key()] = it.value().get<std::string>();
    }
}
void SwampCoreImpl::saveLog() {
    if (!m_storageOk) return;
    json a = json::array();
    for (const auto& e : m_log) a.push_back(logos_sync::eventToJson(e));
    writeFile(m_dataDir + "/catalog.json", a.dump());
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
void SwampCoreImpl::refold() { m_cat = fold(m_log); }
bool SwampCoreImpl::ingest(const Event& e) {
    if (!admissible(e) || m_logIds.count(e.id)) return false;
    m_logIds.insert(e.id);
    m_log.push_back(e);
    if (e.dev == m_id.address) m_id.clock.receive(e.hlc);
    return true;
}
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

void SwampCoreImpl::onFrame(const std::string& topic, const std::string& payloadB64) {
    if (topic != CATALOG_TOPIC) return;
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    m_rx++;
    std::string s = payloadB64, dec;
    json f;
    for (int i = 0; i < 3; i++) {
        size_t p = s.find_first_not_of(" \n\r\t");
        if (p != std::string::npos && s[p] == '{') { f = json::parse(s, nullptr, false); break; }
        if (!unb64(s, dec)) break;
        s = dec;
    }
    if (!f.is_object()) { m_rxBad++; return; }
    const std::string t = f.value("t", "");
    if (t == "ev" && f.contains("e")) {
        if (ingest(logos_sync::eventFromJson(f["e"]))) { m_rxEvents++; refold(); saveLog(); publishState(); }
    } else if (t == "fp" || t == "ids" || t == "need") {
        auto step = logos_sync::catchup::respond(m_log, f, m_id.address);
        for (const auto& r : step.replies) sendFrame(r);
        for (const auto& e : step.serve) sendFrame(json{{"t", "ev"}, {"e", logos_sync::eventToJson(e)}});
    }
}

void SwampCoreImpl::catchupRound() {
    m_lastCatchup = nowMs();
    sendFrame(logos_sync::catchup::buildInitial(m_log, m_id.address));
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
        modules().loam_core.onReceived([this](const std::string& topic, const std::string&, const std::string& payloadB64, int64_t) {
            onFrame(topic, payloadB64);
        });
        modules().loam_core.onStatusChanged([this](const std::string& s) {
            std::lock_guard<std::recursive_mutex> lk(m_mtx);
            m_status = s;
            if (s == "Connected" && !m_ready) {
                m_ready = true;
                modules().loam_core.joinAsync(CATALOG_TOPIC, [](std::string) {});
                for (int ms : {3000, 10000, 25000}) QTimer::singleShot(ms, m_timer, [this] { std::lock_guard<std::recursive_mutex> l(m_mtx); catchupRound(); });
            } else if (s == "Connected") {
                catchupRound();
            }
            publishState();
        });
        modules().loam_core.setSenderIdAsync(m_id.address, [](std::string) {});
        modules().loam_core.startAsync(cfg.dump(), [this](std::string err) {
            if (!err.empty()) { std::lock_guard<std::recursive_mutex> lk(m_mtx); m_status = "Transport error: " + err; publishState(); }
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
        modules().storage_module.onStorageUploadDone([this](const std::string& payload) {
            onLoop([this, payload] { std::lock_guard<std::recursive_mutex> lk(m_mtx); completeUpload(payload); });
        });
        modules().storage_module.onStorageDownloadDone([this](const std::string& payload) {
            onLoop([this, payload] {
                std::lock_guard<std::recursive_mutex> lk(m_mtx);
                json p = json::parse(payload, nullptr, false);
                if (p.is_object()) completeDownload(p.value("sessionId", ""), p.value("success", false), p.value("error", ""));
            });
        });
        // Basecamp 0.3 / logosctl own the Storage node: init() is refused and we adopt it as-is.
        bool inited = false;
        try { inited = modules().storage_module.init(json::object().dump()); } catch (...) { inited = false; }
        m_storageHostOwned = !inited;
        try { modules().storage_module.start(); } catch (...) {}
        fprintf(stderr, "[swamp] storage: %s\n", m_storageHostOwned ? "using the host's node" : "initialised our own node");
    } catch (const std::exception& e) {
        fprintf(stderr, "[swamp] storage unavailable: %s\n", e.what());
    }
}

void SwampCoreImpl::uploadBlob(const std::string& sha) {
    for (const auto& [sess, u] : m_upSessions) if (u.sha == sha) return;
    try {
        StdLogosResult r = modules().storage_module.uploadUrl(blobPath(sha), 65536, true);
        if (!r.success) { fprintf(stderr, "[swamp] upload %s rejected: %s\n", sha.substr(0, 12).c_str(), r.error.c_str()); return; }
        m_upSessions[resVal(r)] = PendingUpload{sha, blobPath(sha), nowMs()};
    } catch (const std::exception& e) { fprintf(stderr, "[swamp] upload failed: %s\n", e.what()); }
}

void SwampCoreImpl::completeUpload(const std::string& payload) {
    json p = json::parse(payload, nullptr, false);
    if (!p.is_object()) return;
    auto it = m_upSessions.find(p.value("sessionId", ""));
    if (it == m_upSessions.end()) return;
    std::string sha = it->second.sha;
    m_upSessions.erase(it);
    if (!p.value("success", false)) { fprintf(stderr, "[swamp] upload of %s failed: %s\n", sha.substr(0, 12).c_str(), p.value("error", "").c_str()); return; }
    std::string cid = p.value("cid", "");
    if (cid.empty()) return;
    m_uploaded++;
    m_myCids[sha] = cid;
    saveJobs();
    announceCids({{sha, cid}});
}

void SwampCoreImpl::announceCids(const std::map<std::string, std::string>& cids) {
    json m = json::object();
    for (const auto& [sha, cid] : cids) {
        auto it = m_cat.cids.find(sha);
        if (it != m_cat.cids.end() && std::find(it->second.begin(), it->second.end(), cid) != it->second.end()) continue;
        m[sha] = cid;
    }
    if (!m.empty()) { author("blob.cids", json{{"cids", m}}); publishState(); }
}

// Fetch one blob into the local cache: try its candidate CIDs in order; verify the hash.
void SwampCoreImpl::startFetch(const std::string& sha, long long size) {
    if (haveBlob(sha) || m_fetching.count(sha)) return;
    auto it = m_cat.cids.find(sha);
    if (it == m_cat.cids.end() || it->second.empty()) return;
    size_t idx = 0;
    auto g = m_fetchGaveUp.find(sha);
    if (g != m_fetchGaveUp.end()) idx = (size_t)g->second;
    if (idx >= it->second.size()) return;
    std::string cid = it->second[idx];
    std::string part = m_dataDir + "/parts/" + sha;
    m_fetching.insert(sha);
    try {
        modules().storage_module.downloadToUrlAsyncResult(cid, part, false, 65536, false, true,
            [this, sha, cid, part, size, idx](logos::AsyncResult<StdLogosResult> ar) {
                onLoop([this, ar, sha, cid, part, size, idx] {
                    std::lock_guard<std::recursive_mutex> lk(m_mtx);
                    StdLogosResult r = ar.value;
                    if (!ar.ok() || !r.success) {
                        // a timeout may mean "still running"; the poll catches a finished file
                        m_downSessions["?" + sha] = PendingDownload{sha, cid, part, size, nowMs(), idx};
                        return;
                    }
                    m_downSessions[resVal(r)] = PendingDownload{sha, cid, part, size, nowMs(), idx};
                });
            }, kStorageTimeoutMs);
    } catch (const std::exception& e) {
        m_fetching.erase(sha);
        fprintf(stderr, "[swamp] download failed to start: %s\n", e.what());
    }
}

void SwampCoreImpl::completeDownload(const std::string& sessionId, bool success, const std::string& error) {
    auto it = m_downSessions.find(sessionId);
    if (it == m_downSessions.end()) return;
    PendingDownload d = it->second;
    m_downSessions.erase(it);
    m_fetching.erase(d.sha);
    if (!success) {
        fprintf(stderr, "[swamp] download %s failed: %s\n", d.sha.substr(0, 12).c_str(), error.c_str());
        m_fetchGaveUp[d.sha] = (long long)d.cidIndex + 1;   // next candidate CID next time
        return;
    }
    finishFetched(d.sha);
}

void SwampCoreImpl::finishFetched(const std::string& sha) {
    std::string part = m_dataDir + "/parts/" + sha, bytes;
    if (!readFile(part, bytes)) return;
    std::error_code ec;
    if (sha256Hex(bytes) != sha) {
        m_verifyFailed++;
        fs::remove(part, ec);
        auto& gi = m_fetchGaveUp[sha];
        gi = gi + 1;   // this CID served the wrong bytes: try the next candidate
        fprintf(stderr, "[swamp] %s: downloaded bytes don't match the hash - rejected\n", sha.substr(0, 12).c_str());
        return;
    }
    fs::rename(part, blobPath(sha), ec);
    m_fetched++;
    m_fetching.erase(sha);
    publishState();
}

// Storage done-events can be lost: read the same facts back (upload: the manifest of our staged
// file name; download: the part file reached the expected size).
void SwampCoreImpl::pollStorage() {
    long long now = nowMs();
    if (!m_upSessions.empty()) {
        try {
            StdLogosResult r = modules().storage_module.manifests();
            json arr = r.value;
            if (arr.is_string()) arr = json::parse(arr.get<std::string>(), nullptr, false);
            if (r.success && arr.is_array()) {
                std::vector<std::string> done;
                for (const auto& [sess, u] : m_upSessions)
                    for (const auto& m : arr)
                        if (m.is_object() && m.value("filename", "") == u.sha && !m.value("cid", "").empty())
                            done.push_back(json{{"sessionId", sess}, {"success", true}, {"cid", m.value("cid", "")}}.dump());
                for (const auto& d : done) completeUpload(d);
            }
        } catch (...) {}
        for (auto it = m_upSessions.begin(); it != m_upSessions.end();)
            it = now - it->second.since > kTransferStaleMs ? m_upSessions.erase(it) : std::next(it);
    }
    std::vector<std::string> finished, stale;
    for (const auto& [sess, d] : m_downSessions) {
        std::error_code ec;
        auto sz = fs::exists(d.part, ec) ? (long long)fs::file_size(d.part, ec) : -1;
        if (d.size > 0 && sz == d.size) finished.push_back(sess);
        else if (now - d.since > kTransferStaleMs) stale.push_back(sess);
    }
    for (const auto& s : finished) completeDownload(s, true, "");
    for (const auto& s : stale) completeDownload(s, false, "timed out");
}

void SwampCoreImpl::advanceJobs() {
    bool changed = false;
    for (auto& [key, j] : m_jobs) {
        if (j.status == "done" || j.status == "failed") continue;
        size_t have = 0;
        for (const auto& [sha, name] : j.files) {
            if (haveBlob(sha)) { have++; continue; }
            auto f = m_cat.cids.find(sha);
            long long size = 0;
            auto mi = m_cat.models.find(j.modelId);
            if (mi != m_cat.models.end() && j.v >= 1 && j.v <= (int)mi->second.versions.size())
                for (const auto& ff : mi->second.versions[j.v - 1]["files"]) if (ff.value("sha256", "") == sha) size = ff.value("size", 0LL);
            if (f == m_cat.cids.end()) continue;   // no CID announced yet - keep waiting
            startFetch(sha, size);
        }
        std::string prev = j.status;
        if (have == j.files.size()) {
            std::error_code ec;
            fs::create_directories(j.dir, ec);
            for (const auto& [sha, name] : j.files) fs::copy_file(blobPath(sha), j.dir + "/" + name, fs::copy_options::overwrite_existing, ec);
            j.status = ec ? "failed" : "done";
            if (ec) j.error = "couldn't write to " + j.dir + ": " + ec.message();
        } else j.status = "fetching";
        if (j.status != prev) changed = true;
    }
    if (changed) { saveJobs(); publishState(); }
}

// Thumbnails and photos are small: fetch them eagerly so listings have pictures.
void SwampCoreImpl::fetchPreviews() {
    int started = 0;
    for (const auto& [id, m] : m_cat.models) {
        if (m.versions.empty() || m.retracted) continue;
        const json& v = m.versions.back();
        if (!v.contains("images")) continue;
        for (const auto& im : v["images"]) {
            std::string sha = im.value("sha256", "");
            if (im.value("size", 0LL) > 2 * 1024 * 1024 || haveBlob(sha) || m_fetching.count(sha)) continue;
            if (started++ >= 4) return;
            startFetch(sha, im.value("size", 0LL));
        }
    }
}

// A hub keeps a copy of every file it sees, so files outlive their creators' desktops.
void SwampCoreImpl::hubSweep() {
    if (!m_hub) return;
    int inflight = (int)m_fetching.size();
    for (const auto& [id, m] : m_cat.models) {
        for (const auto& v : m.versions) {
            json blobs = json::array();
            for (const char* k : {"files", "images"}) if (v.contains(k)) for (const auto& f : v[k]) blobs.push_back(f);
            if (v.contains("fp") && v["fp"].is_object()) blobs.push_back(v["fp"]);
            {
                for (const auto& f : blobs) {
                    if (inflight >= kHubConcurrency) return;
                    std::string sha = f.value("sha256", "");
                    if (haveBlob(sha) || m_fetching.count(sha) || !m_cat.cids.count(sha)) continue;
                    startFetch(sha, f.value("size", 0LL));
                    inflight++;
                }
            }
        }
    }
}

void SwampCoreImpl::tick() {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (m_storageStarted) { pollStorage(); advanceJobs(); fetchPreviews(); hubSweep(); }
    if (m_ready && nowMs() - m_lastCatchup > kCatchupEveryMs) catchupRound();
    // announce CIDs we hold that the catalogue doesn't know yet (e.g. authored while offline)
    std::map<std::string, std::string> missing;
    for (const auto& [sha, cid] : m_myCids) {
        auto it = m_cat.cids.find(sha);
        if (it == m_cat.cids.end() || std::find(it->second.begin(), it->second.end(), cid) == it->second.end()) missing[sha] = cid;
    }
    if (!missing.empty() && m_ready) announceCids(missing);
}

void SwampCoreImpl::startModules() {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    startTransport();
    ensureStorage();
    // re-upload anything that never got a CID (e.g. Basecamp closed mid-upload)
    for (const auto& [id, m] : m_cat.models) {
        if (m.creator != m_id.address) continue;
        for (const auto& v : m.versions) {
            std::vector<std::string> shas;
            for (const char* k : {"files", "images"}) if (v.contains(k)) for (const auto& f : v[k]) shas.push_back(f.value("sha256", ""));
            if (v.contains("fp") && v["fp"].is_object()) shas.push_back(v["fp"].value("sha256", ""));
            for (const auto& sha : shas) if (haveBlob(sha) && !m_myCids.count(sha)) uploadBlob(sha);
        }
    }
}

void SwampCoreImpl::onContextReady() {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    setupDataDir();
    loadAll();
    refold();
    m_timer = new QTimer();
    QObject::connect(m_timer, &QTimer::timeout, m_timer, [this] { tick(); });
    int tickMs = 2000;
    if (const char* t = getenv("SWAMP_TICK_MS")) tickMs = std::max(50, atoi(t));
    m_timer->start(tickMs);
    QTimer::singleShot(kStartDelayMs, m_timer, [this] { startModules(); });
    publishState();
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
    return json{{"ok", true}, {"version", SWAMP_VERSION}, {"status", m_status}, {"hub", m_hub},
                {"me", {{"address", m_id.address}, {"profile", profile}}},
                {"storage", {{"hostOwned", m_storageHostOwned}, {"started", m_storageStarted}, {"dataOk", m_storageOk}, {"downloads", m_downloadsDir}}},
                {"catalog", {{"events", m_log.size()}, {"models", visible}, {"rejected", m_cat.rejected}, {"cids", m_cat.cids.size()}}},
                {"counters", {{"rx", m_rx}, {"tx", m_tx}, {"rxEvents", m_rxEvents}, {"rxBad", m_rxBad}, {"uploaded", m_uploaded},
                              {"fetched", m_fetched}, {"verifyFailed", m_verifyFailed}, {"uploading", m_upSessions.size()}, {"downloading", m_fetching.size()}}}}.dump();
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
    std::string text = q.value("q", ""), tag = q.value("tag", ""), sort = q.value("sort", "new");
    bool mineOnly = q.value("mine", false);
    size_t limit = (size_t)std::max(1, std::min(500, q.value("limit", 100)));
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
                f["fetching"] = m_fetching.count(sha) > 0;
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
    json d = parseArg(draftJson);
    if (!d.is_object()) return fail("The draft is not valid JSON");
    if (!d.contains("files") || !d["files"].is_array() || d["files"].empty()) return fail("Add at least one file");
    std::string modelId = d.value("modelId", "");
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
        std::string path = f.value("path", ""), bytes;
        if (!readFile(path, bytes) || bytes.empty()) return fail("Can't read " + path);
        std::string name = fs::path(path).filename().string();
        if (!safeName(name)) return fail("Unsupported file name: " + name);
        std::string sha = storeBlob(bytes);
        std::string kind = f.value("kind", "");
        std::string ext = lowerExt(name);
        if (kind.empty()) kind = (ext == "stl" || ext == "3mf" || ext == "obj") ? "model" : (ext == "step" || ext == "stp" || ext == "scad" || ext == "f3d") ? "source" : "other";
        files.push_back({{"name", name}, {"kind", kind}, {"size", (long long)bytes.size()}, {"sha256", sha}});
        if (firstMesh.empty() && ext == "stl") { try { firstMesh = fp::parseStl(bytes); } catch (...) {} }
    }
    if (d.contains("images") && d["images"].is_array())
        for (const auto& im : d["images"]) {
            std::string path = im.value("path", ""), bytes;
            if (!readFile(path, bytes) || bytes.empty()) return fail("Can't read image " + path);
            std::string mime = sniffImage(bytes);
            if (mime.empty()) return fail("Images must be PNG, JPEG or WebP");
            images.push_back({{"kind", im.value("kind", "photo")}, {"size", (long long)bytes.size()}, {"sha256", storeBlob(bytes)}, {"mime", mime}});
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
    if (modelId.empty()) {
        std::string nonce = newId();
        modelId = modelIdFor(m_id.address, nonce);
        author("model.create", json{{"modelId", modelId}, {"nonce", nonce}, {"title", d.value("title", "")}});
    }
    int v = existing ? (int)existing->versions.size() + 1 : 1;
    json ver{{"modelId", modelId}, {"v", v}, {"title", d.value("title", "")}, {"summary", d.value("summary", "")},
             {"description", d.value("description", "")}, {"tags", d.value("tags", json::array())},
             {"licence", d.value("licence", "")}, {"parents", d.value("parents", json::array())},
             {"files", files}, {"images", images}};
    if (!fpj.is_null()) ver["fp"] = fpj;
    std::string why = validateVersion(ver);
    if (!why.empty()) return fail(why);
    if (ver.dump().size() > MAX_PAYLOAD) return fail("The description and metadata are too long");
    author("model.version", ver);
    // uploads run in the background; CIDs are announced as they complete
    std::vector<std::string> shas;
    for (const auto& f : files) shas.push_back(f.value("sha256", ""));
    for (const auto& f : images) shas.push_back(f.value("sha256", ""));
    if (!fpj.is_null()) shas.push_back(fpj.value("sha256", ""));
    if (m_storageStarted) for (const auto& s : shas) if (!m_myCids.count(s)) uploadBlob(s);
    publishState();
    return ok(json{{"modelId", modelId}, {"v", v}});
}

std::string SwampCoreImpl::retract(std::string modelId, std::string reason) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
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
    for (const auto& f : ver["files"]) j.files.push_back({f.value("sha256", ""), f.value("name", "")});
    m_jobs[modelId + "@" + std::to_string(v)] = j;
    saveJobs();
    advanceJobs();
    return ok(json{{"dir", j.dir}});
}

std::string SwampCoreImpl::comment(std::string modelId, std::string text) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    modelId = unquote(modelId); text = unquote(text);
    if (!m_cat.models.count(modelId)) return fail("No such model");
    if (text.empty() || text.size() > 4000) return fail("Comments are 1-4000 characters");
    author("comment.post", json{{"modelId", modelId}, {"text", text}});
    return ok();
}

std::string SwampCoreImpl::postMake(std::string modelId, std::string makeJson) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
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
            if (m_storageStarted) uploadBlob(sha);
        }
    json payload{{"modelId", modelId}, {"text", mk.value("text", "")}, {"images", imgs}};
    if (mk.contains("v") && mk["v"].is_number_integer()) payload["v"] = mk["v"];
    if (payload["text"].get<std::string>().empty() && imgs.empty()) return fail("Add a photo or a few words about your print");
    author("make.post", payload);
    return ok();
}

std::string SwampCoreImpl::like(std::string modelId, std::string on) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    modelId = unquote(modelId); on = unquote(on);
    if (!m_cat.models.count(modelId)) return fail("No such model");
    bool b = !(on == "false" || on == "0" || on == "no" || on == "off");
    author("like.put", json{{"modelId", modelId}, {"on", b}});
    return ok();
}

std::string SwampCoreImpl::setProfile(std::string profileJson) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    json p = parseArg(profileJson);
    if (!p.is_object()) return fail("The profile is not valid JSON");
    std::string name = p.value("name", "");
    if (name.empty() || name.size() > 60) return fail("Pick a display name (1-60 characters)");
    author("profile.put", json{{"name", name}, {"bio", p.value("bio", "")}});
    return ok();
}

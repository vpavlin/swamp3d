#include "swamp_core_impl.h"
#include "logos_sdk.h"
#include "logos_sync/catchup.hpp"
#include "swamp_fp.hpp"
#include "swamp_index.hpp"
#include "swamp_bambu.hpp"
#include "swamp_3mf.hpp"
#include <thread>
#include <cmath>
#define SWAMP_THUMB_QT 1   // the module links Qt Core: compress thumbnails with qCompress
#include "swamp_thumb.hpp"
#include <QTimer>
#include <QProcess>
#include <QStandardPaths>
#include <QSysInfo>
#include <QDir>
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

static const char* SWAMP_VERSION = "0.5.9";
static constexpr int kStartDelayMs = 1000;          // 0.3 runtime rejects calls from inside onContextReady
static constexpr int kStorageTimeoutMs = 60000;     // storage 3.x waits up to 30 s for a manifest
static constexpr long long kCatchupBehindMs = 15000;  // while the last round still brought events in
static constexpr long long kCatchupFreshMs = 30000, kCatchupFreshForMs = 5 * 60 * 1000;   // the first minutes after coming up
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
static constexpr long long kWantImgMs = 10 * 60 * 1000;    // a picture stays wanted this long after the view asked
static constexpr int kHubConcurrency = 3;
static constexpr long long kImageMaxBytes = 16 * 1024 * 1024;
static constexpr size_t kImageCacheFiles = 300;
static constexpr long long kMaxClockLeadMs = 5 * 60 * 1000;
static constexpr long long kManifestMaxAgeMs = 30LL * 24 * 3600 * 1000;   // an index older than this isn't used
static constexpr long long kSilentTopicMs = 10LL * 60 * 1000;   // posting with no answer this long = "may not reach anyone"
static constexpr long long kRecordRefreshMs = 60000;   // refresh a model held only through the index
static constexpr int kPrivateRounds = 2;            // failed private (Mix) rounds before a shard fetch goes plain
static constexpr int kJobRounds = 5;                 // full passes over a file's CIDs before a download fails (~8 min: a fresh upload can take 3+ min to be findable)

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
    // Direct printing is an experiment, off unless asked for: in the first real-A1 test the printer
    // drove its head against the top of the frame (2026-10-08, cause not yet known). "Open in slicer"
    // is the supported way to print.
    const char* xp = getenv("SWAMP_EXPERIMENTAL_PRINT");
    m_experimentalPrint = xp && (std::string(xp) == "1" || std::string(xp) == "true");
    const char* ix = getenv("SWAMP_INDEXER");
    m_indexer = m_hub || (ix && (std::string(ix) == "1" || std::string(ix) == "true"));
    if (const char* hp = getenv("SWAMP_HUB_PULL_RETRY_MS")) m_hubPullRetryMs = std::max(100LL, atoll(hp));
    if (const char* hc = getenv("SWAMP_HUB_PULL_CHECK_MS")) m_hubPullCheckMs = std::max(100LL, atoll(hc));
    if (const char* hw = getenv("SWAMP_HUB_PULL_WRITE_MS")) m_hubPullWriteMs = std::max(100LL, atoll(hw));
    if (const char* ev = getenv("SWAMP_INDEX_EVERY_MS")) m_indexEveryMs = std::max(1000LL, atoll(ev));
    if (const char* ut = getenv("SWAMP_INDEX_UPLOAD_TIMEOUT_MS")) m_indexUploadTimeoutMs = std::max(1000LL, atoll(ut));
    if (const char* ie = getenv("SWAMP_INCLUSION_EVERY_MS")) m_inclusionEveryMs = std::max(1000LL, atoll(ie));
    if (const char* ig = getenv("SWAMP_INCLUSION_GRACE_MS")) m_inclusionGraceMs = std::max(0LL, atoll(ig));
    if (const char* oc = getenv("SWAMP_OMISSION_CONFIRM_MS")) m_omissionConfirmMs = std::max(0LL, atoll(oc));
    if (const char* t = getenv("SWAMP_TEST_OMIT")) m_testOmit = t;
    const char* pf = getenv("SWAMP_PRIVATE_FETCH");
    m_privateShards = pf && (std::string(pf) == "1" || std::string(pf) == "true");
}
std::string SwampCoreImpl::blobPath(const std::string& sha) const { return m_dataDir + "/files/" + sha; }
bool SwampCoreImpl::haveBlob(const std::string& sha) const { std::error_code ec; return fs::exists(blobPath(sha), ec); }
std::string SwampCoreImpl::storeBlob(const std::string& bytes) {
    std::string sha = sha256Hex(bytes);
    if (!haveBlob(sha)) writeFile(blobPath(sha), bytes);
    return sha;
}

// Which categories this node mirrors. Until index snapshots (ADR 0015) give global search, a node
// with no saved choice mirrors everything; SWAMP_CATEGORIES ("all" or a comma list) overrides; a
// hub always mirrors everything.
void SwampCoreImpl::loadSettings() {
    m_subs.clear();
    std::string s;
    json st = readFile(m_dataDir + "/settings.json", s) ? json::parse(s, nullptr, false) : json();
    std::vector<std::string> want;
    bool all = !st.is_object() || !st.contains("categories");
    if (st.is_object()) for (const auto& c : arr(st, "categories")) if (c.is_string()) want.push_back(c.get<std::string>());
    if (const char* env = getenv("SWAMP_CATEGORIES")) {
        std::string e = env; want.clear(); all = (e == "all");
        for (size_t i = 0; !all && i <= e.size();) { size_t j = e.find(',', i); if (j == std::string::npos) j = e.size(); if (j > i) want.push_back(e.substr(i, j - i)); i = j + 1; }
    }
    if (m_hub) all = true;
    for (const auto& [id, label] : categories()) if (all || std::find(want.begin(), want.end(), id) != want.end()) m_subs.insert(id);
}
void SwampCoreImpl::saveSettings() {
    json c = json::array();
    for (const auto& id : m_subs) c.push_back(id);
    writeFile(m_dataDir + "/settings.json", json{{"categories", c}}.dump());
}
json SwampCoreImpl::omissionsJson() {
    json a = json::array();
    for (const auto& [who, ev] : m_omissions) a.push_back(ev);
    return a;
}
json SwampCoreImpl::suspectsJson() {
    json a = json::array();
    for (const auto& [k, v] : m_suspects) a.push_back(v);
    return a;
}
json SwampCoreImpl::excludedMineJson() {
    json a = json::array();
    for (const auto& [k, why] : m_excludedMine) {
        std::string who = k.substr(0, k.find('|')), id = k.substr(k.find('|') + 1);
        auto mi = m_cat.models.find(id);
        a.push_back({{"indexer", who}, {"indexerName", nameOf(who)}, {"modelId", id}, {"why", why},
                     {"title", mi != m_cat.models.end() && !mi->second.versions.empty() ? mi->second.versions.back().value("title", "") : ""}});
    }
    return a;
}
// loam_core can't tell us when Delivery refuses a channel (its errors are asynchronous and
// swallowed), so judge by what comes back: posting on a topic for 10 minutes without hearing a
// single frame on it - not even a catch-up answer - means our posts probably reach nobody.
json SwampCoreImpl::transportHealth() {
    json silent = json::array();
    long long now = nowMs();
    for (const auto& [topic, h] : m_topicHealth)
        if (h.tx > 0 && h.lastRx == 0 && h.firstTx && now - h.firstTx > kSilentTopicMs) silent.push_back(topic);
    return json{{"silentTopics", silent}, {"ok", silent.empty()}};
}

json SwampCoreImpl::categoriesJson() {
    std::map<std::string, int> count;
    for (const auto& [id, m] : m_cat.models) if (!m.versions.empty() && !m.retracted) count[m.category]++;
    json a = json::array();
    for (const auto& [id, label] : categories())
        a.push_back({{"id", id}, {"label", label}, {"subscribed", m_subs.count(id) > 0}, {"models", count[id]}});
    return a;
}

void SwampCoreImpl::loadAll() {
    loadSettings();
    { std::string ps; json pj = readFile(m_dataDir + "/printer.json", ps) ? json::parse(ps, nullptr, false) : json(); if (pj.is_object()) m_printer = pj; }
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
    std::string topic = topicOf(e, m_cat);
    // publishing into a category makes you part of it: replies to your models reach you
    if (e.type == "model.create" && topic != PEOPLE_TOPIC) subscribe(str(payload, "category").empty() ? "other" : str(payload, "category"));
    sendFrame(topic, json{{"t", "ev"}, {"e", logos_sync::eventToJson(e)}});
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
    std::string thumbSha;
    if (v.contains("images")) for (const auto& im : v["images"]) {
        std::string sha = im.value("sha256", "");
        if (thumbSha.empty()) thumbSha = sha;
        if (haveBlob(sha)) { thumb = blobPath(sha); break; }
    }
    int made = (int)m.makes.size();
    return json{{"modelId", m.modelId}, {"title", v.value("title", "")}, {"summary", v.value("summary", "")},
                {"creator", m.creator}, {"creatorName", nameOf(m.creator)}, {"latest", m.versions.size()},
                {"licence", v.value("licence", "")}, {"tags", v.value("tags", json::array())},
                {"likes", likeCount(m)}, {"makes", made}, {"comments", m.comments.size()},
                {"published", v.value("published", 0LL)}, {"created", m.created}, {"thumb", thumb},
                {"mine", m.creator == m_id.address}, {"retracted", m.retracted},
                {"remix", v.contains("parents") && !v["parents"].empty()},
                {"thumbSha", thumbSha}, {"category", m.category}};
}

// ---- transport ------------------------------------------------------------------------------
// ---- topics (ADR 0014) ------------------------------------------------------------------------
std::vector<std::string> SwampCoreImpl::subscribedTopics() const {
    std::vector<std::string> t{PEOPLE_TOPIC};
    for (const auto& c : m_subs) t.push_back(categoryTopic(c));
    return t;
}
bool SwampCoreImpl::isSubscribedTopic(const std::string& topic) const {
    if (topic == PEOPLE_TOPIC) return true;
    for (const auto& c : m_subs) if (categoryTopic(c) == topic) return true;
    return false;
}
void SwampCoreImpl::ensureJoined(const std::string& topic) {
    if (topic.empty() || !m_ready || m_joined.count(topic)) return;
    if (!validContentTopic(topic)) { fprintf(stderr, "[swamp] refusing malformed topic %s\n", topic.c_str()); return; }
    m_joined.insert(topic);
    try { modules().loam_core.joinAsync(topic, [](std::string) {}); } catch (...) {}
}
void SwampCoreImpl::subscribe(const std::string& cat) {
    if (!knownCategory(cat) || m_subs.count(cat)) return;
    m_subs.insert(cat);
    saveSettings();
    ensureJoined(categoryTopic(cat));
    if (m_ready) catchupOn(categoryTopic(cat));
}
// The events that belong on a topic - what catch-up on that topic compares and serves.
std::vector<Event> SwampCoreImpl::eventsOn(const std::string& topic) {
    std::vector<Event> out;
    for (const auto& e : m_log) if (topicOf(e, m_cat) == topic) out.push_back(e);
    return out;
}

void SwampCoreImpl::sendFrame(const std::string& topic, const json& frame) {
    if (!m_ready || topic.empty()) return;   // catch-up delivers anything we authored offline
    ensureJoined(topic);
    m_tx++;
    TopicHealth& h = m_topicHealth[topic];
    h.tx++;
    if (!h.firstTx) h.firstTx = nowMs();
    try { modules().loam_core.sendSealedAsync(topic, b64std(frame.dump()), [](std::string) {}); }
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
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!m_joined.count(topic)) return;
    m_topicHealth[topic].lastRx = nowMs();
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
    try { handleFrame(topic, f, live); }
    catch (const std::exception& e) { m_rxBad++; fprintf(stderr, "[swamp] bad frame dropped: %s\n", e.what()); }
}

void SwampCoreImpl::handleFrame(const std::string& topic, const json& f, bool live) {
    const std::string t = str(f, "t");
    bool subscribed = isSubscribedTopic(topic);
    if (t == "ev" || t == "evs") {
        json list = t == "ev" ? json::array({f.value("e", json())}) : f.value("es", json::array());
        if (!list.is_array()) { m_rxBad++; return; }
        for (const auto& j : list) {
            Event e;
            if (!eventFrom(j, e)) { m_rxBad++; continue; }
            // on a topic we only send to (a model we opened, outside our categories), keep what's
            // about models we already hold - not the whole category
            // - or a model someone sent us a link to
            const std::string mid = str(e.payload, "modelId");
            if (!subscribed && !m_linked.count(mid) && (e.type == "model.create" || !m_cat.models.count(mid))) continue;
            if (ingest(e)) m_rxEvents++;
        }
    } else if (wellFormedCatchup(f)) {
        if (!subscribed) return;   // we don't hold that topic's set: answering would be wrong
        // Store replays old catch-up requests on every (re)connect: answering them is pure cost.
        // Events always count, whatever their age.
        if (!live) { m_staleCatchup++; return; }
        // A peer's opening fp gets one answer per kAnswerEveryMs; its follow-ups (ids/need) always do.
        const std::string from = topic + "|" + str(f, "from");
        long long now = nowMs();
        if (str(f, "t") == "fp" && !f.contains("lo") && !f.contains("hi")) {
            auto a = m_answeredAt.find(from);
            if (a != m_answeredAt.end() && now - a->second < kAnswerEveryMs) { m_throttled++; return; }
            m_answeredAt[from] = now;
        }
        auto step = logos_sync::catchup::respond(eventsOn(topic), f, m_id.address);
        for (const auto& r : step.replies) sendFrame(topic, r);
        serveEvents(topic, step.serve);
    } else m_rxBad++;
}

// Serve what a peer asked for in batches, within a budget: every node answering every request at
// full speed would flood the topic (and spend the RLN allowance). Anything cut off is asked
// for again in the peer's next catch-up round.
void SwampCoreImpl::serveEvents(const std::string& topic, const std::vector<Event>& evs, bool budgeted) {
    long long now = nowMs();
    if (now - m_serveWindow > kServeWindowMs) { m_serveWindow = now; m_servedInWindow = 0; }
    json batch = json::array();
    size_t bytes = 0;
    auto flush = [&] { if (!batch.empty()) sendFrame(topic, json{{"t", "evs"}, {"es", batch}}); batch = json::array(); bytes = 0; };
    for (const auto& e : evs) {
        if (budgeted && m_servedInWindow >= kServePerWindow) { m_throttled += 1; continue; }
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

void SwampCoreImpl::catchupOn(const std::string& topic) {
    sendFrame(topic, logos_sync::catchup::buildInitial(eventsOn(topic), m_id.address));
}
void SwampCoreImpl::catchupRound() {
    m_lastCatchup = nowMs();
    m_rxEventsAtCatchup = m_rxEvents;
    for (const auto& t : subscribedTopics()) catchupOn(t);
}

// loam_core reports "Connected" by event; the event can arrive before we subscribed or get lost,
// so tick() also asks for the status until we're ready.
void SwampCoreImpl::onStatus(const std::string& s) {
    if (s.empty()) return;
    m_status = s;
    if (s == "Connected" && !m_ready) {
        m_ready = true;
        m_readyAt = nowMs();
        for (const auto& t : subscribedTopics()) ensureJoined(t);
        for (int ms : {3000, 10000, 25000}) QTimer::singleShot(ms, m_timer, [this] { std::lock_guard<std::recursive_mutex> l(m_mtx); catchupRound(); });
    } else if (s == "Connected") {
        catchupRound();
    }
    publishState();
}

void SwampCoreImpl::startTransport() {
    if (m_transportStarted) return;
    m_transportStarted = true;
    // useChannels:false = plain relay: delivery 0.3's reliable channels hold back every frame whose
    // causal history this node never saw ("SDS message has missing dependencies" -> "stash full"),
    // which silently dropped Swamp traffic between nodes with history (2026-10-09). Swamp's own
    // RBSR catch-up is the reliability layer; loam_core still unwraps channel frames from older peers.
    json cfg = json{{"mode", "Core"}, {"preset", "logos.test"}, {"useChannels", false}};
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
    std::set<std::string> mine = myBlobs();
    mine.insert(m_indexShas.begin(), m_indexShas.end());
    for (const auto& sha : mine) {
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
    // one event per topic: a blob.cids event is routed by the model of its first file
    std::map<std::string, json> byModel;
    for (const auto& [sha, cid] : m_toAnnounce) {
        auto bm = m_cat.blobModel.find(sha);
        std::string mid = bm == m_cat.blobModel.end() ? "" : bm->second;
        auto mi = m_cat.models.find(mid);
        std::string topic = mi == m_cat.models.end() ? "" : categoryTopic(mi->second.category);
        if (topic.empty()) continue;   // its model isn't folded yet: next tick
        json& m = byModel[topic];
        if (m.is_null()) m = json::object();
        if (m.size() >= kCidsPerEvent) continue;
        m[sha] = cid;
        m_announcedAt[sha] = now;
    }
    for (auto& [topic, m] : byModel) {
        for (auto it = m.begin(); it != m.end(); ++it) m_toAnnounce.erase(it.key());
        author("blob.cids", json{{"cids", m}});
    }
    publishState();
}

// 15 s, 30 s, 1 min, ... 30 min. Short at first: a fresh upload often isn't findable in the DHT
// for the first tens of seconds ("failed to get manifest"), then is.
// a decimal number from slicer output, independent of the user's locale (see swamp_fp.hpp)
static double numberAt(const std::string& s, size_t at) {
    double v = 0;
    while (at < s.size() && (s[at] == ' ' || s[at] == ':' || s[at] == '=')) at++;
    std::from_chars(s.data() + at, s.data() + s.size(), v);
    return v;
}

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
// Where a blob can be fetched from: CIDs the catalogue announced, else CIDs an index named (a
// search result's thumbnail, a shard). The bytes are verified against the hash either way.
std::vector<std::string> SwampCoreImpl::cidsFor(const std::string& sha) {
    auto it = m_cat.cids.find(sha);
    if (it != m_cat.cids.end() && !it->second.empty()) return it->second;
    auto x = m_extraCids.find(sha);
    return x == m_extraCids.end() ? std::vector<std::string>() : x->second;
}

void SwampCoreImpl::startFetch(const std::string& sha, long long size) {
    if (haveBlob(sha) || size <= 0 || size > MAX_FILE_BYTES) return;
    std::vector<std::string> cands = cidsFor(sha);
    const std::vector<std::string>* itv = &cands;
    if (cands.empty()) return;
    Fetch& F = m_fetch[sha];
    long long now = nowMs();
    if (F.inflight || F.gaveUp || now < F.nextTry) return;
    int inflight = 0;
    for (const auto& [s, f] : m_fetch) inflight += f.inflight;
    if (inflight >= kMaxFetches || !storageFree()) return;
    if (F.cidIdx >= itv->size()) F.cidIdx = 0;
    std::string cid = (*itv)[F.cidIdx];
    // index shards are fetched over Mix and not re-advertised: nobody learns what you searched for
    bool priv = m_privateFetch.count(sha) > 0;
    std::string part = m_dataDir + "/parts/" + sha;
    std::error_code ec; fs::remove(part, ec);
    F.inflight = true; F.since = now; F.size = size; F.cid = cid; F.session.clear(); F.seenSize = 0; F.grewAt = now;
    long long attempt = now;
    try {
        modules().storage_module.downloadToUrlAsyncResult(cid, part, false, 65536, priv, !priv,
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
    size_t n = cidsFor(sha).size();
    // a private (Mix) fetch that keeps failing falls back to a plain one, and says so
    if (m_privateFetch.count(sha) && F.rounds >= kPrivateRounds - 1 && F.cidIdx + 1 >= n) { m_privateFetch.erase(sha); m_privacyDowngrades++; }
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
            if (cidsFor(sha).empty()) refreshRecord(j.modelId);   // its CIDs may be in a newer index
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
                if (err.empty() && m_openAfter.erase(key)) { std::string why = launchSlicer(j); if (!why.empty()) j.error = why; }
                if (err.empty() && str(m_pjob, "stage") == "downloading" && str(m_pjob, "key") == key) beginSlice(j);
            } else j.status = "fetching";
        }
        if (j.status != prevStatus || j.error != prevError) changed = true;
    }
    if (changed) { saveJobs(); publishState(); }
}

// ---- hand-off to a desktop slicer --------------------------------------------------------------
// Swamp doesn't slice (yet): it hands the verified model files to the slicer you already use, and
// you print from there - with your printer set up however it is (Bambu cloud, LAN, PrusaLink...).
// Only model files are passed, never anything else from a download.
static bool isMeshFile(const std::string& name) {
    std::string e = lowerExt(name);
    return e == "stl" || e == "3mf" || e == "obj" || e == "step" || e == "stp" || e == "amf";
}

// ---- the slicer Swamp can set up by itself --------------------------------------------------------
// OrcaSlicer 2.4.2, the Ubuntu 24.04 AppImage (works on current distributions; the Ubuntu 22.04
// build needs WebKitGTK 4.0, which they no longer ship). Pinned by hash, unpacked once into
// <data>/slicers/ so it needs no FUSE, and preferred over anything else once it's there.
struct OrcaBuild { const char* arch; const char* url; const char* sha; long long size; };
static const OrcaBuild kOrcaBuilds[] = {
    {"x86_64", "https://github.com/OrcaSlicer/OrcaSlicer/releases/download/v2.4.2/OrcaSlicer_Linux_AppImage_Ubuntu2404_V2.4.2.AppImage",
     "d12fb8c8eac1aecd2dfb6377acd48f994f8fa439ed5292fa532dd82880f029fd", 137759224},
    {"arm64", "https://github.com/OrcaSlicer/OrcaSlicer/releases/download/v2.4.2/OrcaSlicer_Linux_AppImage_Ubuntu2404_aarch64_V2.4.2.AppImage",
     "e1a07275a25f176626c55a5df39e91bc4476d8c28ee4a3192ff758e29dd5c3ba", 135469576}};
static const OrcaBuild* orcaBuild() {
    std::string a = QSysInfo::currentCpuArchitecture().toStdString();
    for (const auto& b : kOrcaBuilds) if (a == b.arch) return &b;
    return nullptr;
}
json SwampCoreImpl::managedSlicer() const {
    std::string run = m_dataDir + "/slicers/orca-2.4.2/squashfs-root/AppRun";
    std::error_code ec;
    // only once it has started here: an unpacked copy that can't run must not shadow a working slicer
    if (!fs::exists(run, ec) || !fs::exists(m_dataDir + "/slicers/orca-2.4.2/works", ec)) return nullptr;
    return json{{"name", "OrcaSlicer 2.4.2 (set up by Swamp)"}, {"program", run}, {"args", json::array()}, {"managed", true}};
}

// The command that installs a system package on this distribution (from /etc/os-release).
struct OsRelease { std::string id, like, version; };
static OsRelease osRelease() {
    OsRelease o;
    std::ifstream f("/etc/os-release");
    std::string line;
    while (std::getline(f, line)) {
        auto val = [&](const char* k) { std::string v = line.substr(strlen(k)); v.erase(std::remove(v.begin(), v.end(), '"'), v.end()); return v; };
        if (line.rfind("ID=", 0) == 0) o.id = val("ID=");
        if (line.rfind("ID_LIKE=", 0) == 0) o.like = val("ID_LIKE=");
        if (line.rfind("VERSION_ID=", 0) == 0) o.version = val("VERSION_ID=");
    }
    return o;
}
// Debian 13+ and Ubuntu 24.04+ renamed some libraries for the 64-bit time_t transition (libfuse2 -> libfuse2t64)
static bool debianT64() {
    OsRelease o = osRelease();
    double v = atof(o.version.c_str());
    return (o.id == "debian" && v >= 13) || (o.id == "ubuntu" && v >= 24.04) || (o.id != "debian" && o.id != "ubuntu" && o.like.find("ubuntu") != std::string::npos);
}
static std::string pkgInstall(const std::string& fedora, const std::string& debian, const std::string& arch, const std::string& suse) {
    OsRelease o = osRelease();
    std::string all = " " + o.id + " " + o.like + " ";
    auto is = [&](const char* d) { return all.find(std::string(" ") + d + " ") != std::string::npos; };
    if (is("fedora") || is("rhel") || is("centos")) return "sudo dnf install -y " + fedora;
    if (is("debian") || is("ubuntu")) return "sudo apt install -y " + debian;
    if (is("arch")) return "sudo pacman -S --needed " + arch;
    if (is("suse") || is("opensuse")) return "sudo zypper install -y " + suse;
    return "";
}

// What to do about a slicer that didn't work, in plain steps. {title, text, steps:[{text, command}], action}
// action "installSlicer" = Swamp can fix it itself with one button.
json SwampCoreImpl::slicerFix(const std::string& log, const json& slicer) const {
    const OrcaBuild* b = orcaBuild();
    // only a library the loader couldn't find counts: Orca's normal output also names libwebkit2gtk-4.1
    // ("no version information available (required by .../libwebkit2gtk-4.1.so.0)")
    std::string missing;
    for (size_t at = log.find("error while loading shared libraries: "); at != std::string::npos; at = log.find("error while loading shared libraries: ", at + 1)) {
        size_t from = at + 38, to = log.find(':', from);
        missing += " " + log.substr(from, to == std::string::npos ? std::string::npos : to - from);
    }
    auto lacks = [&](const char* lib) { return missing.find(lib) != std::string::npos; };
    bool managed = slicer.is_object() && slicer.value("managed", false);
    json steps = json::array();
    auto manual = [&]() {   // the same thing by hand, for people who'd rather
        if (!b) return;
        steps.push_back({{"text", "Or do it by hand: download it into ~/Applications, then restart Basecamp."},
                         {"command", std::string("mkdir -p ~/Applications && curl -L -o ~/Applications/OrcaSlicer-2.4.2.AppImage '") + b->url + "' && chmod +x ~/Applications/OrcaSlicer-2.4.2.AppImage"}});
    };
    if (slicer.is_null()) {
        json fx{{"title", "No slicer found"}, {"action", b ? "installSlicer" : ""},
                {"text", std::string("To print a model, Swamp opens it in a slicer, and none is installed. ") +
                         (b ? "Swamp can download OrcaSlicer 2.4.2 for you (about 140 MB, from OrcaSlicer's GitHub releases, checked against a pinned hash)." : "Install OrcaSlicer 2.4 or newer, or Bambu Studio, then restart Basecamp.")}};
        manual(); fx["steps"] = steps; return fx;
    }
    if (log.find("version `GLIBC_") != std::string::npos || log.find("version `GLIBCXX_") != std::string::npos) {
        // the AppImage is built against a newer C library than this system has (e.g. Debian 12, Ubuntu 22.04)
        json fx{{"title", "This Linux is too old for that OrcaSlicer build"}, {"action", ""},
                {"text", str(slicer, "name") + " needs a newer C library (glibc 2.38 or later) than this system has. The Flatpak brings its own and works here."}};
        steps.push_back({{"text", "Install OrcaSlicer from Flathub (run in a terminal), then restart Basecamp:"}, {"command", "flatpak install -y flathub io.github.softfever.OrcaSlicer"}});
        steps.push_back({{"text", "No Flatpak yet? Set it up first:"}, {"command", pkgInstall("flatpak", "flatpak", "flatpak", "flatpak") + (pkgInstall("x", "x", "x", "x").empty() ? "" : " && flatpak remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo")}});
        fx["steps"] = steps; return fx;
    }
    if (lacks("libwebkit2gtk-4.0")) {
        json fx{{"title", "This OrcaSlicer is built for older Linux"}, {"action", b ? "installSlicer" : ""},
                {"text", "The " + str(slicer, "name") + " you have is the Ubuntu 22.04 build. It needs WebKitGTK 4.0, which current distributions no longer ship, so installing packages won't fix it. The Ubuntu 24.04 build of OrcaSlicer 2.4.2 works. Swamp can download it and use it from now on (about 140 MB, hash-checked); your other slicer stays as it is."}};
        manual(); fx["steps"] = steps; return fx;
    }
    if (lacks("libwebkit2gtk-4.1") || lacks("libjavascriptcoregtk-4.1")) {
        std::string cmd = pkgInstall("webkit2gtk4.1", "libwebkit2gtk-4.1-0", "webkit2gtk-4.1", "libwebkit2gtk-4_1-0");
        steps.push_back({{"text", cmd.empty() ? "Install WebKitGTK 4.1 (the package is usually called webkit2gtk4.1 or libwebkit2gtk-4.1-0) with your package manager, then try again." : "Run this in a terminal (it asks for your password), then try again:"},
                         {"command", cmd}});
        return json{{"title", "OrcaSlicer needs one system library"}, {"action", ""}, {"steps", steps},
                    {"text", str(slicer, "name") + " needs WebKitGTK 4.1 from your system, and it isn't installed."}};
    }
    if (lacks("libfuse") || log.find("dlopen(): error loading libfuse") != std::string::npos || log.find("AppImages require FUSE") != std::string::npos) {
        json fx{{"title", "This AppImage can't start without FUSE"}, {"action", b ? "installSlicer" : ""},
                {"text", "AppImages need FUSE to run. Swamp can download OrcaSlicer 2.4.2 and unpack it, which needs no FUSE."}};
        std::string cmd = pkgInstall("fuse-libs", debianT64() ? "libfuse2t64" : "libfuse2", "fuse2", "libfuse2");
        if (!cmd.empty()) steps.push_back({{"text", "Or install FUSE and keep your AppImage:"}, {"command", cmd}});
        fx["steps"] = steps; return fx;
    }
    if (log.find("G92 E0") != std::string::npos || log.find("layer_gcode") != std::string::npos) {
        json fx{{"title", "This slicer rejects the A1's own profile"}, {"action", b && !managed ? "installSlicer" : ""},
                {"text", "Seen with OrcaSlicer 2.3 betas. OrcaSlicer 2.4.2 works; Swamp can download it and use it from now on."}};
        manual(); fx["steps"] = steps; return fx;
    }
    if (!missing.empty()) {
        std::string name = missing.substr(1);
        // libraries Orca needs that aren't always installed: the package to install, per distribution
        struct Lib { const char* so; const char* fedora; const char* debian; const char* arch; const char* suse; };
        static const Lib libs[] = {{"libGLU.so", "mesa-libGLU", "libglu1-mesa", "glu", "libGLU1"},
                                   {"libmspack.so", "libmspack", "libmspack0", "libmspack", "libmspack0"},
                                   {"libOSMesa.so", "mesa-libOSMesa", "libosmesa6", "mesa", "Mesa-libOSMesa"}};
        for (const auto& l : libs) if (name.find(l.so) != std::string::npos) {
            std::string cmd = pkgInstall(l.fedora, l.debian, l.arch, l.suse);
            steps.push_back({{"text", cmd.empty() ? std::string("Install the package that provides ") + l.so + ", then try again." : "Run this in a terminal (it asks for your password), then try again:"}, {"command", cmd}});
            return json{{"title", "The slicer needs one system library"}, {"action", ""}, {"steps", steps},
                        {"text", str(slicer, "name") + " needs " + name + ", which isn't installed."}};
        }
        json fx{{"title", "The slicer is missing a system library"}, {"action", b && !managed ? "installSlicer" : ""},
                {"text", str(slicer, "name") + " needs " + name + ", which isn't on this system." + (b && !managed ? " Swamp can download the OrcaSlicer build it's tested with instead." : " Install the package that provides it, then try again.")}};
        manual(); fx["steps"] = steps; return fx;
    }
    return nullptr;
}

json SwampCoreImpl::slicerInstallState() const {
    json j = m_slicerInstall;
    if (str(j, "stage") == "downloading") j["bytes"] = m_slicerBytes.load();
    return j;
}

std::string SwampCoreImpl::installSlicer() {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    std::string st = str(m_slicerInstall, "stage");
    if (st == "downloading" || st == "unpacking" || st == "checking") return ok(json{{"install", m_slicerInstall}});
    const OrcaBuild* b = orcaBuild();
    if (!b) return fail("Swamp can't set up OrcaSlicer on this processor (" + QSysInfo::currentCpuArchitecture().toStdString() + ")");
    std::string dir = m_dataDir + "/slicers", part = dir + "/OrcaSlicer-2.4.2.AppImage.part", img = dir + "/OrcaSlicer-2.4.2.AppImage", out = dir + "/orca-2.4.2";
    m_slicerBytes = 0;
    m_slicerInstall = json{{"stage", "downloading"}, {"total", b->size}, {"message", "Downloading OrcaSlicer 2.4.2..."}};
    publishState();
    std::string url = b->url, sha = b->sha;
    std::atomic<long long>* bytes = &m_slicerBytes;
    auto life = m_life;
    auto setStage = [this, life](const char* stage, const char* msg) {
        if (*life) onLoop([this, life, stage, msg] { if (!*life) return; std::lock_guard<std::recursive_mutex> l(m_mtx); m_slicerInstall["stage"] = stage; m_slicerInstall["message"] = msg; publishState(); });
    };
    runAsync([dir, part, img, out, url, sha, bytes, setStage] {
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::string have;
        // already unpacked (an earlier Swamp set it up, before it checked that it starts): just check it again
        bool unpacked = fs::exists(out + "/squashfs-root/AppRun", ec);
        if (!unpacked && !(fs::exists(img, ec) && sha256File(img, have) && have == sha)) {
            QString tool = QStandardPaths::findExecutable("curl");
            // no stalling forever: give up on a connection that doesn't start or a transfer that stops
            QStringList args{"-fL", "--retry", "3", "--connect-timeout", "30", "--speed-limit", "1024", "--speed-time", "120", "--max-time", "3600",
                             "-o", QString::fromStdString(part), QString::fromStdString(url)};
            if (tool.isEmpty()) { tool = QStandardPaths::findExecutable("wget"); args = QStringList{"--timeout=60", "--tries=3", "-O", QString::fromStdString(part), QString::fromStdString(url)}; }
            if (tool.isEmpty()) return json{{"ok", false}, {"error", "Swamp needs curl or wget to download OrcaSlicer."}};
            fs::remove(part, ec);
            QProcess p; p.start(tool, args);
            while (!p.waitForFinished(500)) {
                if (p.state() == QProcess::NotRunning) break;
                *bytes = (long long)fs::file_size(part, ec);
                if (ec) ec.clear();
            }
            if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0)
                return json{{"ok", false}, {"error", "The download failed: " + QString(p.readAllStandardError()).trimmed().right(200).toStdString()}};
            std::string got;
            if (!sha256File(part, got) || got != sha) { fs::remove(part, ec); return json{{"ok", false}, {"error", "The download doesn't match the expected hash, so Swamp won't run it."}}; }
            fs::rename(part, img, ec);
            if (ec) return json{{"ok", false}, {"error", "Couldn't save the download: " + ec.message()}};
        }
        if (!unpacked) {
        setStage("unpacking", "Unpacking OrcaSlicer...");
        fs::permissions(img, fs::perms::owner_exec, fs::perm_options::add, ec);
        fs::remove_all(out, ec);
        fs::create_directories(out, ec);
        QProcess x; x.setWorkingDirectory(QString::fromStdString(out));
        x.start(QString::fromStdString(img), {"--appimage-extract"});
        x.waitForFinished(10 * 60 * 1000);
        if (!fs::exists(out + "/squashfs-root/AppRun", ec)) return json{{"ok", false}, {"error", "Couldn't unpack the AppImage: " + QString(x.readAllStandardError()).trimmed().right(200).toStdString()}};
        fs::remove(img, ec);   // the unpacked copy is what runs
        }
        setStage("checking", "Checking that it starts...");
        QProcess t; t.start(QString::fromStdString(out + "/squashfs-root/AppRun"), {"--help"});
        bool fin = t.waitForFinished(120000);
        std::string log = QString(t.readAllStandardOutput() + t.readAllStandardError()).toStdString();
        // it works if it printed its usage; a loader error or a crash leaves no usage text
        bool works = fin && t.exitStatus() == QProcess::NormalExit && log.find("Usage: orca-slicer") != std::string::npos;
        fs::remove(out + "/works", ec);
        if (works) { std::ofstream(out + "/works") << "ok\n"; }
        return json{{"ok", true}, {"works", works}, {"log", log.substr(log.size() > 4000 ? log.size() - 4000 : 0)}};
    }, [this](json r) {
        m_slicerAt = 0;   // rescan: the new one goes first
        if (!r.value("ok", false)) {
            m_slicerInstall = json{{"stage", "failed"}, {"message", r.value("error", "Setting up OrcaSlicer failed")}};
        } else {
            json fix = slicerFix(r.value("log", ""), json{{"name", "OrcaSlicer 2.4.2"}, {"managed", true}});
            if (r.value("works", false)) m_slicerInstall = json{{"stage", "done"}, {"message", "OrcaSlicer 2.4.2 is set up. Swamp uses it from now on."}};
            else m_slicerInstall = json{{"stage", "failed"}, {"message", "OrcaSlicer 2.4.2 is downloaded, but it doesn't start on this system."}, {"log", r.value("log", "")},
                                        {"fix", fix.is_object() ? fix : json{{"title", "OrcaSlicer doesn't start here"}, {"action", ""},
                                                {"text", "Copy its output below to see why, or install OrcaSlicer another way (the Flatpak works on most systems)."},
                                                {"steps", json::array({json{{"text", "Install OrcaSlicer from Flathub (run in a terminal), then restart Basecamp:"}, {"command", "flatpak install -y flathub io.github.softfever.OrcaSlicer"}}})}}}};
        }
        publishState();
    });
    return ok(json{{"install", m_slicerInstall}});
}

// Which slicer to open, best first: SWAMP_SLICER, then installed binaries, Flatpaks, AppImages in
// the usual folders. {name, program, leading args}.
json SwampCoreImpl::findSlicer() {
    long long now = nowMs();
    const char* forced = getenv("SWAMP_SLICER");
    if (!(forced && *forced) && m_slicerAt && now - m_slicerAt < 30000) return m_slicer;   // snapshot() is polled: don't rescan every time
    m_slicerAt = now;
    m_slicer = detectSlicer();
    m_bambuSlicers = detectSlicers(true);
    return m_slicer;
}
json SwampCoreImpl::detectSlicer() { auto all = detectSlicers(false); return all.empty() ? json() : all[0]; }

// Every slicer installed, best first; bambuOnly = the ones that can slice for a Bambu Lab printer
// (OrcaSlicer, Bambu Studio). Printing must not stop at "the first slicer found" - with PrusaSlicer
// as a Flatpak and Orca as an AppImage, that was PrusaSlicer (review 2026-10-07).
std::vector<json> SwampCoreImpl::detectSlicers(bool bambuOnly) {
    std::vector<json> out;
    auto add = [&](json j) { std::string n = str(j, "name"); if (!bambuOnly || n.find("Orca") != std::string::npos || n.find("Bambu") != std::string::npos) out.push_back(j); };
    if (const char* env = getenv("SWAMP_SLICER")) if (*env) add(json{{"name", fs::path(env).filename().string()}, {"program", env}, {"args", json::array()}});
    if (json m = managedSlicer(); m.is_object()) add(m);
    const std::vector<std::pair<std::string, std::vector<std::string>>> bins = {
        {"OrcaSlicer", {"orca-slicer", "OrcaSlicer", "orcaslicer"}},
        {"Bambu Studio", {"bambu-studio", "BambuStudio", "bambustudio"}},
        {"PrusaSlicer", {"prusa-slicer", "PrusaSlicer", "prusaslicer"}},
        {"SuperSlicer", {"superslicer", "SuperSlicer"}}};
    for (const auto& [name, cands] : bins)
        for (const auto& c : cands) {
            QString p = QStandardPaths::findExecutable(QString::fromStdString(c));
            if (!p.isEmpty()) { add(json{{"name", name}, {"program", p.toStdString()}, {"args", json::array()}}); break; }
        }
    QString flatpak = QStandardPaths::findExecutable("flatpak");
    if (!flatpak.isEmpty()) {
        const std::vector<std::pair<std::string, std::string>> apps = {
            {"OrcaSlicer", "io.github.softfever.OrcaSlicer"}, {"Bambu Studio", "com.bambulab.BambuStudio"}, {"PrusaSlicer", "com.prusa3d.PrusaSlicer"}};
        for (const auto& [name, id] : apps)
            for (const std::string& dir : {std::string("/var/lib/flatpak/app/") + id, homeDir() + "/.local/share/flatpak/app/" + id}) {
                std::error_code ec;
                if (fs::exists(dir, ec)) { add(json{{"name", name + " (Flatpak)"}, {"program", flatpak.toStdString()}, {"args", {"run", id}}}); break; }
            }
    }
    // AppImages: the Ubuntu 24.04 builds first - the 22.04 ones need WebKitGTK 4.0, gone from current distributions
    std::vector<json> imgs;
    const std::vector<std::pair<std::string, std::string>> images = {{"OrcaSlicer", "orca"}, {"Bambu Studio", "bambu"}, {"PrusaSlicer", "prusaslicer"}};
    for (const auto& [name, needle] : images)
        for (const char* sub : {"/Applications", "/Downloads", "/bin", "/opt", "/.local/bin"}) {
            std::error_code ec;
            std::string dir = homeDir() + sub;
            if (!fs::is_directory(dir, ec)) continue;
            for (const auto& de : fs::directory_iterator(dir, ec)) {
                std::string fn = de.path().filename().string(), low = fn;
                for (auto& c : low) c = (char)std::tolower((unsigned char)c);
                if (low.find(needle) != std::string::npos && low.size() > 9 && low.compare(low.size() - 9, 9, ".appimage") == 0)
                    imgs.push_back(json{{"name", name + " (AppImage)"}, {"program", de.path().string()}, {"args", json::array()},
                                        {"rank", low.find("2404") != std::string::npos ? 0 : low.find("2204") != std::string::npos ? 2 : 1}});
            }
        }
    std::stable_sort(imgs.begin(), imgs.end(), [](const json& a, const json& b) { return a["rank"].get<int>() < b["rank"].get<int>(); });
    for (auto& j : imgs) { j.erase("rank"); add(j); }
    return out;
}

std::string SwampCoreImpl::launchSlicer(const DownloadJob& j) {
    json sl = findSlicer();
    if (sl.is_null()) return "No slicer found - install OrcaSlicer, Bambu Studio or PrusaSlicer, or set SWAMP_SLICER";
    QStringList args;
    for (const auto& a : sl["args"]) args << QString::fromStdString(a.get<std::string>());
    int n = 0;
    for (const auto& [sha, name] : j.files) if (isMeshFile(name)) { args << QString::fromStdString(j.dir + "/" + name); n++; }
    if (n == 0) return "This version has no model files a slicer can open";
    qint64 pid = 0;
    bool started = QProcess::startDetached(QString::fromStdString(sl["program"].get<std::string>()), args, QString::fromStdString(j.dir), &pid);
    fprintf(stderr, "[swamp] slicer: %s %s (%d files) -> %s\n", str(sl, "name").c_str(), str(sl, "program").c_str(), n, started ? "started" : "FAILED");
    if (!started) return "Couldn't start " + str(sl, "name");
    m_slicerLaunches++;
    return "";
}

// One click: download if needed (verified), then open in the slicer.
std::string SwampCoreImpl::openInSlicer(std::string modelId, std::string version) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!m_loaded) return fail("Swamp is still starting - try again in a moment");
    modelId = unquote(modelId);
    if (findSlicer().is_null()) return fail("No slicer found - install OrcaSlicer, Bambu Studio or PrusaSlicer, or set SWAMP_SLICER");
    auto it = m_cat.models.find(modelId);
    if (it == m_cat.models.end() || it->second.versions.empty()) return fail("No such model");
    int v = atoi(unquote(version).c_str());
    if (v <= 0) v = (int)it->second.versions.size();
    std::string key = modelId + "@" + std::to_string(v);
    auto jt = m_jobs.find(key);
    if (jt != m_jobs.end() && jt->second.status == "done") {
        std::string why = launchSlicer(jt->second);
        if (!why.empty()) return fail(why);
        return ok(json{{"opened", true}});
    }
    m_openAfter.insert(key);
    std::string r = download(modelId, std::to_string(v));
    json rj = json::parse(r, nullptr, false);
    if (!rj.is_object() || !rj.value("ok", false)) { m_openAfter.erase(key); return r; }
    return ok(json{{"opened", false}, {"downloading", true}});
}

// Thumbnails and photos are small: fetch them eagerly so listings have pictures.
// The size a listed picture (a version image or a make photo) declares; 0 = not a picture we know.
long long SwampCoreImpl::imageSize(const std::string& sha) {
    for (const auto& [id, m] : m_cat.models) {
        for (const auto& v : m.versions) for (const auto& im : arr(v, "images")) if (str(im, "sha256") == sha) return num(im, "size");
        for (const auto& mk : m.makes) for (const auto& im : arr(mk, "images")) if (str(im, "sha256") == sha) return num(im, "size");
    }
    auto x = m_extraImg.find(sha);
    return x == m_extraImg.end() ? 0 : x->second;
}

// Pictures the view asked for in the last few minutes (cacheImage), small ones only.
void SwampCoreImpl::fetchPreviews() {
    int inflight = 0;
    for (const auto& [s, f] : m_fetch) inflight += f.inflight;
    long long now = nowMs();
    for (auto it = m_wantImg.begin(); it != m_wantImg.end();) {
        const std::string sha = it->first;
        if (now - it->second > kWantImgMs || haveBlob(sha)) { it = m_wantImg.erase(it); continue; }
        ++it;
        if (inflight >= kPreviewFetches) continue;
        long long size = imageSize(sha);
        if (size <= 0 || size > kPreviewMaxBytes || cidsFor(sha).empty()) continue;
        auto fi = m_fetch.find(sha);
        if (fi != m_fetch.end() && (fi->second.inflight || now < fi->second.nextTry)) continue;
        startFetch(sha, size);
        if (m_fetch.count(sha) && m_fetch[sha].inflight) inflight++;
    }
}

// A hub keeps a copy of every file it sees, so files outlive their creators' desktops.
// HUB: keep a copy of every file it sees, so files outlive their creators' desktops.
//
// Home nodes can't be dialled, but Storage's background fetch() still gets their blocks to a
// reachable hub while they're online (it goes through relays; proven in a NAT rig, 2026-10-09).
// fetch() returns at once and exists() turns true as soon as the MANIFEST is in - so the hub
// checks for the whole file by writing it from local data only (downloadToUrl, local=true) and
// verifying the sha256. Until that works it fetches again: often at first (publishers are usually
// online right after publishing), then backing off to every 30 min.
void SwampCoreImpl::hubSweep() {
    if (!m_hub) return;
    long long now = nowMs();
    int active = 0;
    for (const auto& [sha, p] : m_hubPulls) active += p.stage == HubPull::Write;
    for (const auto& [id, m] : m_cat.models) {
        if (m.retracted) continue;
        for (const auto& v : m.versions) {
            json blobs = json::array();
            for (const char* k : {"files", "images"}) if (v.contains(k)) for (const auto& f : v[k]) blobs.push_back(f);
            if (v.contains("fp") && v["fp"].is_object()) blobs.push_back(v["fp"]);
            for (const auto& f : blobs) {
                std::string sha = f.value("sha256", "");
                long long size = f.value("size", 0LL);
                if (sha.empty() || haveBlob(sha)) continue;
                std::vector<std::string> cids = cidsFor(sha);
                if (cids.empty()) continue;
                HubPull& p = m_hubPulls[sha];
                if (p.cid.empty()) { p.cid = cids[0]; p.size = size; }
                hubPullStep(sha, p, now, active);
            }
        }
    }
}

void SwampCoreImpl::hubPullStep(const std::string& sha, HubPull& p, long long now, int& active) {
    std::string part = m_dataDir + "/parts/" + sha + ".hub";
    std::error_code ec;
    auto retry = [&] {
        try { if (!p.session.empty()) modules().storage_module.downloadCancelAsyncResult(p.session, [](logos::AsyncResult<StdLogosResult>) {}, kStorageTimeoutMs); } catch (...) {}
        fs::remove(part, ec);
        p.session.clear(); p.tries++;
        p.stage = p.tries % 3 == 0 ? HubPull::Fetch : HubPull::Wait;   // re-issue the fetch every third try
        p.nextAt = now + std::min<long long>(m_hubPullRetryMs << std::min(p.tries / 3, 6), 30LL * 60 * 1000);
        if (p.tries % 3 == 0) {   // the next candidate CID, if the catalogue has several
            std::vector<std::string> cids = cidsFor(sha);
            if (!cids.empty()) p.cid = cids[(size_t)(p.tries / 3) % cids.size()];
        }
    };
    if (now < p.nextAt) return;
    switch (p.stage) {
    case HubPull::Fetch:
        if (!storageFree()) return;
        try {
            modules().storage_module.fetchAsyncResult(p.cid, false, true, [this, life = m_life](logos::AsyncResult<StdLogosResult>) {
                if (*life) onLoop([this] { std::lock_guard<std::recursive_mutex> lk(m_mtx); storageDone(); });
            }, kStorageTimeoutMs);
        } catch (...) { storageDone(); }
        p.stage = HubPull::Wait; p.nextAt = now + m_hubPullCheckMs;
        return;
    case HubPull::Wait: {
        if (active >= kHubConcurrency || !storageFree()) return;
        fs::remove(part, ec);
        p.stage = HubPull::Write; p.since = now; active++;
        long long since = now;
        try {
            modules().storage_module.downloadToUrlAsyncResult(p.cid, part, true, 65536, false, true,
                [this, life = m_life, sha, since](logos::AsyncResult<StdLogosResult> ar) {
                    if (*life) onLoop([this, ar, sha, since] {
                        std::lock_guard<std::recursive_mutex> lk(m_mtx);
                        storageDone();
                        auto it = m_hubPulls.find(sha);
                        if (it == m_hubPulls.end() || it->second.since != since) return;
                        if (ar.ok() && ar.value.success) it->second.session = resVal(ar.value);
                    });
                }, kStorageTimeoutMs);
        } catch (...) { storageDone(); retry(); }
        return;
    }
    case HubPull::Write: {
        long long sz = fs::exists(part, ec) ? (long long)fs::file_size(part, ec) : -1;
        std::string got;
        if (sz > 0 && (p.size <= 0 || sz == p.size) && sha256File(part, got) && got == sha) {
            fs::rename(part, blobPath(sha), ec);
            if (ec) { retry(); return; }
            m_fetched++; m_hubHeld++;
            fprintf(stderr, "[swamp] hub: holds %s (%lld bytes, after %d tries)\n", sha.substr(0, 12).c_str(), sz, p.tries + 1);
            m_hubPulls.erase(sha);
            publishState();
            return;
        }
        if (p.size > 0 && sz > p.size) { m_verifyFailed++; retry(); return; }
        if (now - p.since > m_hubPullWriteMs) retry();   // not all blocks are here yet
        return;
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
        indexTick();
        checkInclusion();
        // Peers serve a few hundred events a minute each, so a node that's behind (just installed, or
        // offline for a while) gets part of what it's missing per round. While rounds still bring
        // events in, ask again soon; once caught up, every two minutes. (Review 2026-10-09: a late
        // indexer missed a whole category for minutes.)
        // A node that just came up may get nothing from a round (every peer's budget spent): keep
        // asking at a moderate pace for its first minutes as well.
        long long every = m_rxEvents > m_rxEventsAtCatchup ? kCatchupBehindMs
                        : (m_readyAt && now - m_readyAt < kCatchupFreshForMs) ? kCatchupFreshMs : kCatchupEveryMs;
        if (m_ready && now - m_lastCatchup > every) catchupRound();
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
    findSlicer();   // refreshes m_bambuSlicers (cached ~30 s) before printSlicer reads it
    auto pit = m_cat.profiles.find(m_id.address);
    json profile = pit == m_cat.profiles.end() ? json{{"name", ""}, {"bio", ""}} : pit->second;
    size_t visible = 0;
    for (const auto& [id, m] : m_cat.models) if (!m.versions.empty() && !m.retracted) visible++;
    size_t inflight = 0;
    for (const auto& [s, f] : m_fetch) inflight += f.inflight;
    return json{{"ok", true}, {"version", SWAMP_VERSION}, {"status", m_status}, {"hub", m_hub}, {"experimentalPrint", m_experimentalPrint},
                {"me", {{"address", m_id.address}, {"profile", profile}}},
                {"categories", categoriesJson()},
                {"transport", transportHealth()},
                {"slicer", findSlicer()},
                {"printer", printerPublic()}, {"printJob", m_pjob}, {"slicerInstall", slicerInstallState()},
                {"printSlicer", m_bambuSlicers.empty() ? json() : m_bambuSlicers[0]}, {"printSlicerFix", m_bambuSlicers.empty() ? slicerFix("", nullptr) : json()}, {"discovering", m_discovering}, {"discovered", m_discovered},
                {"index", {{"indexer", m_indexer}, {"built", m_indexesBuilt}, {"known", m_cat.indexes.size()}, {"privacyDowngrades", m_privacyDowngrades},
                           {"omissions", omissionsJson()}, {"suspects", suspectsJson()}, {"excludedMine", excludedMineJson()}}},
                {"storage", {{"hostOwned", m_storageHostOwned}, {"started", m_storageStarted}, {"dataOk", m_storageOk}, {"downloads", m_downloadsDir}}},
                {"catalog", {{"events", m_log.size()}, {"models", visible}, {"rejected", m_cat.rejected}, {"cids", m_cat.cids.size()}}},
                {"counters", {{"hubHeld", m_hubHeld}, {"hubPulling", (long)m_hubPulls.size()}, {"rx", m_rx}, {"tx", m_tx}, {"rxEvents", m_rxEvents}, {"rxBad", m_rxBad}, {"uploaded", m_uploaded},
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
    std::string catFilter = str(q, "category");
    std::string text = str(q, "q"), tag = str(q, "tag"), sort = (str(q, "sort").empty() ? std::string("new") : str(q, "sort"));
    bool mineOnly = flag(q, "mine");
    size_t limit = (size_t)std::max(1, std::min(500, (int)std::max(-1000LL, std::min(1000LL, num(q, "limit", 100)))));
    std::vector<const Model*> hits;
    for (const auto& [id, m] : m_cat.models) {
        if (m.versions.empty()) continue;
        if (mineOnly && m.creator != m_id.address) continue;
        if (m.retracted && !mineOnly) continue;   // your own retracted models stay visible to you
        if (!mineOnly && m.creator != m_id.address && !m_subs.count(m.category)) continue;   // opened from search, not followed
        if (!catFilter.empty() && m.category != catFilter) continue;
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

// Share links: swamp://model/<modelId>?c=<category>. The first link in the pasted text counts; the
// Share button puts the link first, before the creator-chosen title. The id opens the model through the search
// index (its record shard), like a search result. The category lets this node listen on that topic
// for the session, so the model's new versions, comments and makes arrive live. (Catch-up on a
// topic needs the whole topic's set, which only followers hold.) Following a category stays the
// user's choice in Me.
std::string SwampCoreImpl::openLink(std::string link) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    link = unquote(link);
    while (!link.empty() && isspace(static_cast<unsigned char>(link.back()))) link.pop_back();
    size_t at = link.find("swamp://model/");
    if (at == std::string::npos) return fail("Not a Swamp link (they look like swamp://model/...)");
    std::string rest = link.substr(at + 14), id = rest.substr(0, rest.find_first_of("?#/ "));
    bool hex = id.size() == 32;
    for (char c : id) hex = hex && isxdigit(static_cast<unsigned char>(c));
    if (!hex) return fail("This Swamp link is damaged");
    for (auto& c : id) c = char(tolower(static_cast<unsigned char>(c)));
    std::string cat;
    size_t q = rest.find("c=");
    if (q != std::string::npos && (rest[q - 1] == '?' || rest[q - 1] == '&')) cat = rest.substr(q + 2, rest.find_first_of("&# ", q) - q - 2);
    // models from links are accepted on topics we don't follow - keep that set small
    if (!m_linked.count(id)) {
        m_linkedOrder.push_back(id);
        m_linked.insert(id);
        while (m_linkedOrder.size() > 256) { m_linked.erase(m_linkedOrder.front()); m_linkedOrder.pop_front(); }
    }
    if (knownCategory(cat) && !m_subs.count(cat)) ensureJoined(categoryTopic(cat));
    return json{{"ok", true}, {"modelId", id}, {"category", knownCategory(cat) ? cat : ""}}.dump();
}

std::string SwampCoreImpl::getModel(std::string modelId) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    modelId = unquote(modelId);
    auto it = m_cat.models.find(modelId);
    if (it == m_cat.models.end() || it->second.versions.empty()) {
        bool pending = false;
        if (!fetchRecord(modelId, pending)) {
            if (pending) return json{{"ok", false}, {"pending", true}, {"error", "Fetching this model from the index..."}}.dump();
            return fail("No such model (it may not have synced yet)");
        }
        it = m_cat.models.find(modelId);
        if (it == m_cat.models.end() || it->second.versions.empty()) return fail("No such model");
    }
    refreshRecord(modelId);
    it = m_cat.models.find(modelId);
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
        // parents with what the view needs to link them
        if (vv.contains("parents") && vv["parents"].is_array())
            for (auto& p : vv["parents"]) {
                auto pm = m_cat.models.find(str(p, "modelId"));
                if (pm != m_cat.models.end() && !pm->second.versions.empty()) {
                    p["title"] = pm->second.versions.back().value("title", "");
                    p["creatorName"] = nameOf(pm->second.creator);
                    int pv = (int)num(p, "v", 0);
                    if (pv >= 1 && pv <= (int)pm->second.versions.size()) {   // the version that was remixed, not the latest
                        p["title"] = pm->second.versions[pv - 1].value("title", "");
                        p["licence"] = pm->second.versions[pv - 1].value("licence", "");
                    }
                }
            }
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

// Basecamp 0.3 sandboxes a view: it may load only qrc: and files under its own plugin dir (no
// file:// elsewhere, no data: URLs). So the view passes its dir and we copy the picture into
// <viewDir>/cache/ - a cache only (an upgrade wipes it; we refill on demand). Only blobs the
// catalogue lists as pictures, only real images, only into a dir named like the plugin.
std::string SwampCoreImpl::cacheImage(std::string sha, std::string viewDir) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    sha = unquote(sha); viewDir = unquote(viewDir);
    if (!isHex(sha, 64)) return fail("Not a picture id");
    while (viewDir.size() > 1 && viewDir.back() == '/') viewDir.pop_back();
    std::error_code ec;
    if (viewDir.empty() || viewDir[0] != '/' || viewDir.find("..") != std::string::npos ||
        fs::path(viewDir).filename() != "swamp" || !fs::is_directory(viewDir, ec)) return fail("Not the Swamp view's directory");
    if (imageSize(sha) <= 0) return fail("Not a picture in the catalogue");
    if (!haveBlob(sha)) {
        // pictures are fetched when someone looks at them, not for the whole catalogue (ADR 0014)
        m_wantImg[sha] = nowMs();
        return json{{"ok", false}, {"pending", true}, {"error", "Fetching the picture"}}.dump();
    }
    if ((long long)fs::file_size(blobPath(sha), ec) > kImageMaxBytes) return fail("Picture too large to show");
    std::string head;
    { std::ifstream f(blobPath(sha), std::ios::binary); head.resize(16); f.read(head.data(), 16); head.resize((size_t)std::max<std::streamsize>(0, f.gcount())); }
    std::string mime = sniffImage(head);
    if (mime.empty()) return fail("Not an image");
    std::string ext = mime == "image/png" ? ".png" : mime == "image/jpeg" ? ".jpg" : ".webp";
    std::string dir = viewDir + "/cache", out = dir + "/" + sha + ext;
    if (!fs::exists(out, ec)) {
        fs::create_directories(dir, ec);
        // keep the cache bounded: drop the oldest files past kImageCacheFiles
        std::vector<std::pair<fs::file_time_type, fs::path>> files;
        for (const auto& de : fs::directory_iterator(dir, ec)) if (de.is_regular_file(ec)) files.push_back({de.last_write_time(ec), de.path()});
        if (files.size() >= kImageCacheFiles) {
            std::sort(files.begin(), files.end());
            for (size_t k = 0; k + kImageCacheFiles <= files.size(); k++) fs::remove(files[k].second, ec);
        }
        fs::copy_file(blobPath(sha), out + ".tmp", fs::copy_options::overwrite_existing, ec);
        if (!ec) fs::rename(out + ".tmp", out, ec);
        if (ec) return fail("Couldn't write the picture cache: " + ec.message());
    }
    return ok(json{{"path", out}});
}

// ---- global search (ADR 0015) ----------------------------------------------------------------
// INDEXER (a hub, or SWAMP_INDEXER=1): every epoch in which the catalogue changed, build the
// shards, store them like any blob (file name = sha256, so identical shards get identical CIDs on
// every indexer), upload, and once every shard has a CID publish a signed index.manifest.
void SwampCoreImpl::indexTick() {
    long long now = nowMs();
    if (!m_indexer || !m_ready) return;
    if (!m_indexPending.is_null()) {
        // A shard upload that never finishes must not stop this indexer for good: its last index would
        // stay the live one, however stale (found 2026-10-09: an indexer kept serving an index from
        // before it had caught up). Give up on it after a while and build a fresh one.
        if (now - num(m_indexPending, "startedAt") > m_indexUploadTimeoutMs) {
            fprintf(stderr, "[swamp] index: shard uploads didn't finish in %lld s; building a new index\n", m_indexUploadTimeoutMs / 1000);
            m_indexPending = nullptr; m_indexRoot.clear(); m_lastIndex = 0;
            return;
        }
        json shards = json::object();
        for (auto it = m_indexPending["shards"].begin(); it != m_indexPending["shards"].end(); ++it) {
            std::string sha = str(it.value(), "sha256");
            auto c = m_myCids.find(sha);
            if (c == m_myCids.end()) { uploadBlob(sha); return; }   // still uploading (uploadBlob retries one that was dropped)
            shards[it.key()] = {{"sha256", sha}, {"size", num(it.value(), "size")}, {"cid", c->second}};
        }
        json mf{{"v", index::VERSION}, {"epoch", num(m_indexPending, "epoch")}, {"root", str(m_indexPending, "root")},
                {"models", num(m_indexPending, "models")}, {"shards", shards}};
        if (!m_indexPending["excluded"].empty()) mf["excluded"] = m_indexPending["excluded"];
        author("index.manifest", mf);
        m_indexesBuilt++;
        m_indexPending = nullptr;
        return;
    }
    // the catalogue this index covers - manifests excluded, or publishing one would trigger the next
    std::vector<Event> content;
    for (const auto& e : m_log) if (e.type != "index.manifest") content.push_back(e);
    std::string root = logos_sync::catchup::fpOf(logos_sync::catchup::sortedIds(content));
    if (root == m_indexRoot || now - m_lastIndex < m_indexEveryMs) return;
    int visibleModels = 0;
    for (const auto& [id, m] : m_cat.models) visibleModels += !m.versions.empty() && !m.retracted;
    if (visibleModels == 0) return;
    m_lastIndex = now;
    m_indexRoot = root;
    // the hub's content policy: <data>/exclusions.json = {"<modelId>": "why"}; declared in the
    // manifest. SWAMP_TEST_OMIT silently drops a model - for testing omission proofs only.
    std::set<std::string> excluded, omit;
    json excl = json::array();
    { std::string ex; json ej = readFile(m_dataDir + "/exclusions.json", ex) ? json::parse(ex, nullptr, false) : json();
      if (ej.is_object()) for (auto it = ej.begin(); it != ej.end() && excl.size() < 200; ++it)
          if (isHex(it.key(), 32) && it.value().is_string()) { excluded.insert(it.key()); excl.push_back({{"m", it.key()}, {"why", it.value().get<std::string>().substr(0, 140)}}); } }
    if (!m_testOmit.empty()) omit.insert(m_testOmit);
    std::set<std::string> leaveOut = excluded;
    leaveOut.insert(omit.begin(), omit.end());
    auto shards = index::build(m_cat, content, leaveOut);
    json pend{{"epoch", now / 1000}, {"root", root}, {"models", 0}, {"shards", json::object()}, {"excluded", excl}, {"startedAt", now}};
    int models = 0;
    for (const auto& [id, m] : m_cat.models) models += !m.versions.empty() && !m.retracted;
    pend["models"] = models;
    m_indexShas.clear();
    for (const auto& [key, sh] : shards) {
        std::string bytes = sh.dump();
        std::string sha = storeBlob(bytes);
        pend["shards"][key] = {{"sha256", sha}, {"size", (long long)bytes.size()}};
        m_indexShas.insert(sha);
        uploadBlob(sha);
    }
    m_indexPending = pend;
    fprintf(stderr, "[swamp] index: %d models, %zu shards, epoch %lld\n", models, shards.size(), (long long)(now / 1000));
}

// CLIENT: the manifest to search - the newest one, from any indexer. How many indexers agree with
// it (same catalogue root, same shard hashes) is reported with every answer (ADR 0016 builds on it).
json SwampCoreImpl::bestManifest() {
    // The sender chooses its event's timestamp, so "newest" alone lets anyone win by dating a
    // manifest in the year 2100 (review 2026-10-07). Instead:
    //  - ignore manifests dated in the future (beyond clock skew) or too old to be useful;
    //  - prefer the content most independent indexers agree on (same root, same shards);
    //  - newest only breaks ties; indexers caught omitting come last.
    long long now = nowMs();
    struct Group { json newest; int indexers = 0, caught = 0; };
    std::map<std::string, Group> groups;
    for (const auto& [who, mf] : m_cat.indexes) {
        long long at = num(mf, "published");
        if (at > now + kMaxClockLeadMs || at < now - kManifestMaxAgeMs) continue;
        std::string key = str(mf, "root") + "|" + (mf.contains("shards") ? mf["shards"].dump() : "");
        Group& g = groups[key];
        g.indexers++;
        if (caughtOmitting(who)) g.caught++;
        if (g.newest.is_null() || at > num(g.newest, "published")) g.newest = mf;
    }
    const Group* best = nullptr;
    for (const auto& [k, g] : groups) {
        if (!best) { best = &g; continue; }
        int honestA = g.indexers - g.caught, honestB = best->indexers - best->caught;
        if (honestA != honestB ? honestA > honestB : num(g.newest, "published") > num(best->newest, "published")) best = &g;
    }
    if (!best) return json();
    json out = best->newest;
    out["agreeing"] = best->indexers;
    return out;
}

// CREATORS AUDIT THE INDEXERS (ADR 0016). For every indexer's newest manifest, check that each of
// my models published well before that index is in it - in the term shard of its first title
// word - or declared excluded. A missing one is evidence: the indexer signed a manifest naming a
// shard (by hash) that leaves my model out. Clients stop preferring that indexer.
// Caught leaving out at least one of my models, in its latest index. (Omissions are per indexer and
// model, and withdrawn when a newer index includes the model.)
bool SwampCoreImpl::caughtOmitting(const std::string& who) const {
    auto it = m_omissions.lower_bound(who + "|");
    return it != m_omissions.end() && it->first.rfind(who + "|", 0) == 0;
}
void SwampCoreImpl::checkInclusion() {
    long long now = nowMs();
    if (now - m_lastInclusion < m_inclusionEveryMs) return;
    m_lastInclusion = now;
    for (const auto& [who, mf] : m_cat.indexes) {
        if (who == m_id.address) continue;
        std::set<std::string> declared;
        for (const auto& x : arr(mf, "excluded")) declared.insert(str(x, "m"));
        for (const auto& [id, m] : m_cat.models) {
            if (m.creator != m_id.address || m.versions.empty() || m.retracted) continue;
            if (m.created > num(mf, "epoch") * 1000 - m_inclusionGraceMs) continue;   // too new to expect
            if (declared.count(id)) {
                std::string why;
                for (const auto& x : arr(mf, "excluded")) if (str(x, "m") == id) why = str(x, "why");
                m_excludedMine[who + "|" + id] = why;
                continue;
            }
            // every term the model is indexed under - title words and tags - must lead to it; an
            // indexer could otherwise drop it from all but the first word's file (review 2026-10-07)
            std::set<std::string> terms;
            for (const auto& w : index::words(m.versions.back().value("title", ""))) terms.insert(w);
            for (const auto& g : arr(m.versions.back(), "tags")) if (g.is_string()) for (const auto& w : index::words(g.get<std::string>())) terms.insert(w);
            if (terms.empty()) continue;
            bool pending = false, there = true;
            std::string key;
            for (const auto& t : terms) {
                std::string k = index::shardKeyFor(t);
                json sh = shard(mf, k);
                if (sh.is_null()) { pending = true; break; }
                bool listed = false;
                if (sh.contains("terms") && sh["terms"].is_object() && sh["terms"].contains(t))
                    for (const auto& x : sh["terms"][t]) if (x.is_string() && x.get<std::string>() == id) { listed = true; break; }
                if (!listed || !sh.contains("entries") || !sh["entries"].contains(id)) { there = false; key = k; break; }
            }
            if (pending) { m_lastInclusion = now - m_inclusionEveryMs + 5000; continue; }   // fetching: look again soon
            std::string skey = who + "|" + id;
            if (there) {
                m_suspects.erase(skey);
                // a newer index includes it after all (the indexer was catching up): withdraw the accusation
                if (m_omissions.erase(skey)) { fprintf(stderr, "[swamp] index %s now includes my model %s: omission withdrawn\n", who.substr(0, 10).c_str(), id.substr(0, 8).c_str()); publishState(); }
                continue;
            }
            if (m_omissions.count(skey)) continue;
            // Missing could just mean the indexer never received it (it was offline, or we were).
            // First time: a suspect - re-send the model's events so it can. Caught only if an
            // index built well after that re-send still leaves it out (ADR 0016).
            auto sp = m_suspects.find(skey);
            if (sp == m_suspects.end()) {
                m_suspects[skey] = json{{"indexer", who}, {"indexerName", nameOf(who)}, {"modelId", id}, {"title", m.versions.back().value("title", "")}, {"since", now}};
                std::vector<Event> mine;
                for (const auto& e : m_log) if (index::modelOfEvent(e, m_cat) == id) mine.push_back(e);
                // outside the serving budget: one model's events, once per suspect (a busy node would
                // otherwise drop exactly the re-send that clears the indexer)
                serveEvents(categoryTopic(m.category), mine, false);
                fprintf(stderr, "[swamp] index %s lacks my model %s: re-sent its %zu events\n", who.substr(0, 10).c_str(), id.substr(0, 8).c_str(), mine.size());
                publishState();
                continue;
            }
            if (num(mf, "epoch") * 1000 < num(sp->second, "since") + m_omissionConfirmMs) continue;   // no index built since then yet
            std::string sha = mf.contains("shards") && mf["shards"].contains(key) ? str(mf["shards"][key], "sha256") : "";
            m_omissions[skey] = json{{"indexer", who}, {"indexerName", nameOf(who)}, {"manifestEvent", str(mf, "eventId")},
                                    {"epoch", num(mf, "epoch")}, {"modelId", id}, {"title", m.versions.back().value("title", "")},
                                    {"shard", key}, {"shardSha256", sha}, {"shardMissing", sha.empty()}, {"modelCreated", m.created},
                                    {"resentAt", num(sp->second, "since")}, {"seen", now}};
            m_suspects.erase(skey);
            fprintf(stderr, "[swamp] OMISSION: indexer %s still leaves out my model %s after a re-send (shard %s)\n", who.substr(0, 10).c_str(), id.substr(0, 8).c_str(), key.c_str());
            publishState();
        }
    }
}
// A shard the manifest names: parsed if we hold it, else queued for a private fetch (null).
json SwampCoreImpl::shard(const json& mf, const std::string& key) {
    if (!mf.contains("shards") || !mf["shards"].contains(key)) return json::object();   // no such shard = no matches
    const json& ref = mf["shards"][key];
    std::string sha = str(ref, "sha256");
    auto c = m_shardCache.find(sha);
    if (c != m_shardCache.end()) return c->second;
    if (haveBlob(sha)) {
        std::string bytes;
        json j = readFile(blobPath(sha), bytes) ? json::parse(bytes, nullptr, false) : json();
        if (key[0] == 't') {   // term shards are cleaned before use; record shards are signed events, checked one by one
            json c = index::cleanTermShard(j);
            if (c.is_null()) { m_badShards++; c = json::object(); }
            j = c;
        } else if (!j.is_object()) { m_badShards++; j = json::object(); }
        if (m_shardCache.size() > 64) m_shardCache.clear();
        m_shardCache[sha] = j;
        return j;
    }
    m_extraCids[sha] = {str(ref, "cid")};
    // Private (Mix) shard fetches are opt-in: on logos.test (2026-10) every Mix lookup failed, and a
    // failed private request left the blocks wanted over a dead Mix route, so even the plain retry
    // stalled. The UI says searches aren't private while this is off.
    if (m_privateShards) m_privateFetch.insert(sha);
    startFetch(sha, num(ref, "size"));
    return nullptr;
}

std::string SwampCoreImpl::globalSearch(std::string queryJson) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    try { return globalSearchImpl(queryJson); }
    catch (const std::exception& e) { return fail(std::string("Search failed: ") + e.what()); }
}
std::string SwampCoreImpl::globalSearchImpl(const std::string& queryJson) {
    json q = parseArg(queryJson);
    if (!q.is_object()) q = json::object();
    json mf = bestManifest();
    if (mf.is_null()) return ok(json{{"results", json::array()}, {"pending", false}, {"index", nullptr}});
    std::map<std::string, json> terms;
    bool pending = false;
    for (const auto& w : index::words(str(q, "q"))) {
        std::string key = index::shardKeyFor(w);
        if (terms.count(key)) continue;
        json sh = shard(mf, key);
        if (sh.is_null()) pending = true; else terms[key] = sh;
    }
    json results = json::array();
    if (!pending) {
        size_t limit = (size_t)std::max<long long>(1, std::min<long long>(200, num(q, "limit", 50)));
        for (auto e : index::search(str(q, "q"), terms, str(q, "category"), limit)) {
            std::string mid = str(e, "m");
            bool held = m_cat.models.count(mid) && !m_cat.models[mid].versions.empty();
            // index entries are the indexer's word; for models we hold, show the signed facts
            if (held) {
                json c = card(m_cat.models[mid]);
                e["t"] = c["title"]; e["s"] = c["summary"]; e["a"] = c["creator"]; e["n"] = c["creatorName"];
                e["k"] = c["likes"]; e["mk"] = c["makes"]; e["l"] = c["licence"]; e["c"] = c["category"]; e["v"] = c["latest"];
            }
            // remote thumbnails go through the same lazy, verified picture path
            if (e.contains("th") && e["th"].is_object()) {
                std::string sha = str(e["th"], "sha");
                if (isHex(sha, 64)) {
                    m_extraImg[sha] = num(e["th"], "size");
                    std::vector<std::string> cids;
                    for (const auto& c : arr(e["th"], "cids")) if (c.is_string()) cids.push_back(c.get<std::string>());
                    if (!cids.empty() && !m_cat.cids.count(sha)) m_extraCids[sha] = cids;
                }
            }
            results.push_back(json{{"modelId", mid}, {"title", str(e, "t")}, {"summary", str(e, "s")}, {"creator", str(e, "a")},
                                   {"creatorName", str(e, "n").empty() ? str(e, "a").substr(0, 10) : str(e, "n")}, {"latest", num(e, "v")},
                                   {"licence", str(e, "l")}, {"category", str(e, "c")}, {"likes", num(e, "k")}, {"makes", num(e, "mk")},
                                   {"thumb", nullptr}, {"thumbSha", e.contains("th") && e["th"].is_object() ? str(e["th"], "sha") : ""},
                                   {"remix", flag(e, "remix")}, {"held", held}, {"verified", held}});
        }
    }
    int agree = (int)num(mf, "agreeing");
    return ok(json{{"results", results}, {"pending", pending},
                   {"index", {{"indexer", str(mf, "indexer")}, {"indexerName", nameOf(str(mf, "indexer"))}, {"models", num(mf, "models")},
                              {"ageMs", nowMs() - num(mf, "published")}, {"indexers", m_cat.indexes.size()}, {"agreeing", agree},
                              {"privacyDowngrades", m_privacyDowngrades}, {"private", m_privateShards}, {"badShards", m_badShards}}}});
}

// Opening a model you don't follow: take its events from the index's record shard - only that
// model's, plus its authors' profiles - and fold them in. Every event is signature-checked.
bool SwampCoreImpl::fetchRecord(const std::string& modelId, bool& pending) {
    pending = false;
    json mf = bestManifest();
    if (mf.is_null() || !isHex(modelId, 32)) return false;
    json sh = shard(mf, index::recordKeyFor(modelId));
    if (sh.is_null()) { pending = true; return false; }
    std::vector<Event> evs;
    std::set<std::string> authors;
    for (const auto& j : arr(sh, "events")) { Event e; if (eventFrom(j, e)) evs.push_back(e); }
    Catalog bucket = fold(evs);
    bool any = false;
    for (const auto& e : evs)
        if (e.type != "profile.put" && index::modelOfEvent(e, bucket) == modelId) { authors.insert(e.dev); any |= ingest(e); }
    for (const auto& e : evs) if (e.type == "profile.put" && authors.count(e.dev)) ingest(e);
    if (any) refold();
    m_recordAt[modelId] = nowMs();
    auto mi = m_cat.models.find(modelId);
    if (mi == m_cat.models.end()) return false;
    // listen on its category for events about it (CIDs announced later, new comments) - only
    // about models we hold, not the whole category (handleFrame)
    ensureJoined(categoryTopic(mi->second.category));
    return true;
}
// A model held only through the index goes stale: refresh its record from the newest manifest
// now and then (the shard cache is keyed by hash, so an unchanged index costs nothing).
void SwampCoreImpl::refreshRecord(const std::string& modelId) {
    auto mi = m_cat.models.find(modelId);
    if (mi == m_cat.models.end() || m_subs.count(mi->second.category) || mi->second.creator == m_id.address) return;
    auto at = m_recordAt.find(modelId);
    if (at != m_recordAt.end() && nowMs() - at->second < kRecordRefreshMs) return;
    bool pending = false;
    fetchRecord(modelId, pending);
    if (pending) m_recordAt[modelId] = nowMs() - kRecordRefreshMs + 5000;   // shard on its way: look again soon
}


// ---- print on a Bambu Lab printer over the LAN (no cloud) -------------------------------------
// The printer: LAN-only + Developer Mode (swamp_bambu.hpp). The flow, one job at a time:
//   preparePrint -> downloading (verified) -> slicing (OrcaSlicer CLI, the printer's default
//   profile, geometry only) -> ready {estimate} -> startPrint (the user confirmed) -> uploading
//   (FTPS) -> starting (MQTT project_file) -> sent
// Blocking network and slicing run on a worker thread; results come back on the module loop.
void SwampCoreImpl::runAsync(std::function<json()> work, std::function<void(json)> done) {
    auto life = m_life;
    std::thread([this, life, work, done] {
        json r;
        try { r = work(); } catch (const std::exception& e) { r = json{{"ok", false}, {"error", e.what()}}; }
        if (*life) onLoop([this, life, done, r] { if (!*life) return; std::lock_guard<std::recursive_mutex> lk(m_mtx); done(r); });
    }).detach();
}

// Which profile set the printer's model uses (Orca's bundled BBL profiles).
static json profilesFor(const std::string& model) {
    std::string m = model;
    for (auto& c : m) c = (char)std::tolower((unsigned char)c);
    if (m == "n1" || m == "a1 mini" || m == "a1mini" || m == "a1m")
        return json{{"label", "Bambu Lab A1 mini"}, {"machine", "Bambu Lab A1 mini 0.4 nozzle"}, {"process", "0.20mm Standard @BBL A1M"}, {"filament", "Bambu PLA Basic @BBL A1M"}};
    if (m == "n2s" || m == "a1")
        return json{{"label", "Bambu Lab A1"}, {"machine", "Bambu Lab A1 0.4 nozzle"}, {"process", "0.20mm Standard @BBL A1"}, {"filament", "Bambu PLA Basic @BBL A1"}};
    return nullptr;
}

bambu::Printer SwampCoreImpl::printerConf() {
    bambu::Printer p;
    p.ip = str(m_printer, "ip"); p.serial = str(m_printer, "serial"); p.accessCode = str(m_printer, "accessCode");
    p.model = str(m_printer, "model"); p.name = str(m_printer, "name");
    if (num(m_printer, "mqttPort") > 0) p.mqttPort = (int)num(m_printer, "mqttPort");
    if (num(m_printer, "ftpsPort") > 0) p.ftpsPort = (int)num(m_printer, "ftpsPort");
    p.certPinMqtt = str(m_printer, "certPinMqtt"); p.certPinFtps = str(m_printer, "certPinFtps");
    return p;
}
json SwampCoreImpl::printerPublic() {
    if (!m_printer.is_object() || str(m_printer, "ip").empty()) return nullptr;
    json pr = profilesFor(str(m_printer, "model"));
    return json{{"ip", str(m_printer, "ip")}, {"serial", str(m_printer, "serial")}, {"model", str(m_printer, "model")},
                {"name", str(m_printer, "name").empty() ? (pr.is_null() ? "Bambu Lab printer" : str(pr, "label")) : str(m_printer, "name")},
                {"supported", !pr.is_null()}, {"hasAccessCode", !str(m_printer, "accessCode").empty()}, {"state", m_printerState},
                {"stateError", m_printerStateErr}, {"stateAt", m_printerStateAt}};
}

std::string SwampCoreImpl::findPrinters() {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!m_experimentalPrint) return fail("Direct printing is switched off: it's an unfinished experiment (start Basecamp with SWAMP_EXPERIMENTAL_PRINT=1 to try it, at your own risk). Use Open in slicer to print.");
    // a fresh 6-second listen unless one is running or just finished (polling must not restart it)
    if (!m_discovering && nowMs() - m_discoveredAt > 15000) {
        m_discovering = true;
        int port = 2021;
        if (const char* dp = getenv("SWAMP_BAMBU_SSDP_PORT")) port = atoi(dp);
        runAsync([port] {
            json a = json::array();
            for (const auto& p : bambu::discover(6000, port)) a.push_back({{"ip", p.ip}, {"serial", p.serial}, {"model", p.model}, {"name", p.name}});
            return json{{"printers", a}};
        }, [this](json r) { m_discovering = false; m_discoveredAt = nowMs(); m_discovered = r.value("printers", json::array()); publishState(); });
    }
    return ok(json{{"running", m_discovering}, {"printers", m_discovered}});
}

std::string SwampCoreImpl::setPrinter(std::string printerJson) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!m_experimentalPrint) return fail("Direct printing is switched off: it's an unfinished experiment (start Basecamp with SWAMP_EXPERIMENTAL_PRINT=1 to try it, at your own risk). Use Open in slicer to print.");
    json p = parseArg(printerJson);
    if (!p.is_object()) return fail("The printer settings are not valid JSON");
    std::string ip = clip(p, "ip", 64), serial = clip(p, "serial", 40), code = clip(p, "accessCode", 32);
    if (ip.empty() || serial.empty()) return fail("The printer needs an IP address and a serial number (both shown on the printer, or use Find printers)");
    if (code.empty() && str(m_printer, "serial") == serial) code = str(m_printer, "accessCode");   // keep the saved one
    if (code.empty()) return fail("Enter the printer's access code (Settings > LAN only mode on the printer's screen)");
    json old = m_printer;
    m_printer = json{{"kind", "bambu"}, {"ip", ip}, {"serial", serial}, {"accessCode", code}, {"model", clip(p, "model", 32)}, {"name", clip(p, "name", 60)}};
    // setting the same printer up again keeps nothing pinned: that's how a reset/replaced printer is re-trusted
    for (const char* k : {"mqttPort", "ftpsPort"}) if (num(p, k) > 0) m_printer[k] = num(p, k);
    writeFile(m_dataDir + "/printer.json", m_printer.dump());
    std::error_code ec;   // the access code: owner-only
    fs::permissions(m_dataDir + "/printer.json", fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
    m_printerState = nullptr; m_printerStateAt = 0; m_printerStateErr.clear();
    refreshPrinterState();
    return ok(json{{"printer", printerPublic()}});
}

// Pin the printer's certificate the first time we see it (TOFU), per service.
void SwampCoreImpl::pinPrinterCert(const char* field, const std::string& pin) {
    if (pin.empty() || !str(m_printer, field).empty() || str(m_printer, "ip").empty()) return;
    m_printer[field] = pin;
    writeFile(m_dataDir + "/printer.json", m_printer.dump());
    std::error_code ec;
    fs::permissions(m_dataDir + "/printer.json", fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
}

void SwampCoreImpl::refreshPrinterState() {
    if (m_printerPolling || str(m_printer, "ip").empty()) return;
    m_printerPolling = true;
    bambu::Printer p = printerConf();
    runAsync([p] {
        json st; std::string err, pin;
        bool okk = bambu::status(p, st, err, &pin);
        return json{{"ok", okk}, {"state", st}, {"error", err}, {"pin", pin}};
    }, [this](json r) {
        m_printerPolling = false;
        m_printerStateAt = nowMs();
        if (r.value("ok", false)) pinPrinterCert("certPinMqtt", str(r, "pin"));
        if (r.value("ok", false)) {
            const json& st = r["state"];
            m_printerState = json{{"state", str(st, "gcode_state")}, {"percent", num(st, "mc_percent")}, {"remainingMin", num(st, "mc_remaining_time")},
                                  {"job", str(st, "subtask_name")}, {"nozzle", st.value("nozzle_temper", 0.0)}, {"bed", st.value("bed_temper", 0.0)}};
            m_printerStateErr.clear();
        } else m_printerStateErr = r.value("error", "no answer");
        publishState();
    });
}

std::string SwampCoreImpl::printerStatus() {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (nowMs() - m_printerStateAt > 10000) refreshPrinterState();
    return ok(json{{"printer", printerPublic()}, {"job", m_pjob}});
}

// OrcaSlicer (or Bambu Studio) on this machine, plus the folder holding its BBL profiles - for an
// AppImage, the three profile files are extracted once into <data>/orca/.
json SwampCoreImpl::orcaFor(const json& profiles, std::string& err) {
    findSlicer();   // refresh the lists (cached ~30 s)
    json sl;
    if (const char* o = getenv("SWAMP_ORCA")) sl = json{{"name", "OrcaSlicer"}, {"program", o}, {"args", json::array()}};
    else { auto c = m_bambuSlicers; if (!c.empty()) sl = c[0]; }
    if (sl.is_null()) {
        json any = findSlicer();
        err = "Printing to a Bambu Lab printer needs OrcaSlicer (2.4 or newer) or Bambu Studio installed - it slices with the printer's own profile." +
              (any.is_object() ? std::string(" Found only ") + str(any, "name") + ", which can open models but has no Bambu Lab profiles." : std::string(""));
        return nullptr;
    }
    std::string prog = str(sl, "program"), name = str(sl, "name");
    return sl;
}

// Where the slicer's Bambu Lab profiles are - for an AppImage, extracted once into <data>/orca/.
// Slow the first time (up to minutes): runs on the worker thread, never under the module's lock.
static json locateBblProfiles(const json& sl, const json& profiles, const std::string& dataDir, std::string& err) {
    std::string prog = str(sl, "program"), name = str(sl, "name");
    auto findIn = [&](const std::string& root) -> std::string {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) return "";
        auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
        for (int seen = 0; it != fs::recursive_directory_iterator() && seen < 200000; it.increment(ec), seen++) {
            if (ec) break;
            if (it->path().filename() == str(profiles, "machine") + ".json" && it->path().parent_path().filename() == "machine")
                return it->path().parent_path().parent_path().string();   // .../profiles/BBL
        }
        return "";
    };
    std::string bbl;
    if (const char* pd = getenv("SWAMP_ORCA_PROFILES")) bbl = pd;
    bool appimage = lowerExt(prog) == "appimage";
    if (!bbl.empty()) {
    } else if (appimage) {
        std::string dir = dataDir + "/orca";
        bbl = findIn(dir);
        if (bbl.empty()) {
            std::error_code ec; fs::create_directories(dir, ec);
            for (const char* kind : {"machine", "process", "filament"}) {
                std::string file = std::string("resources/profiles/BBL/") + kind + "/" + str(profiles, kind) + ".json";
                QProcess x; x.setWorkingDirectory(QString::fromStdString(dir));
                x.start(QString::fromStdString(prog), {"--appimage-extract", QString::fromStdString(file)});
                x.waitForFinished(120000);
            }
            bbl = findIn(dir);
        }
    } else {
        for (const std::string& root : {fs::path(prog).parent_path().parent_path().string(), std::string("/usr/share"), std::string("/opt"), std::string("/app/share"), homeDir() + "/.local/share",
                                        std::string("/var/lib/flatpak/app/io.github.softfever.OrcaSlicer"), std::string("/var/lib/flatpak/app/com.bambulab.BambuStudio")}) {
            bbl = findIn(root);
            if (!bbl.empty()) break;
        }
    }
    if (bbl.empty()) { err = "Couldn't find " + name + "'s Bambu Lab profiles"; return nullptr; }
    return json{{"program", prog}, {"args", sl["args"]}, {"name", name}, {"bbl", bbl}};
}

std::string SwampCoreImpl::preparePrint(std::string modelId, std::string version) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!m_experimentalPrint) return fail("Direct printing is switched off: it's an unfinished experiment (start Basecamp with SWAMP_EXPERIMENTAL_PRINT=1 to try it, at your own risk). Use Open in slicer to print.");
    if (!m_loaded) return fail("Swamp is still starting - try again in a moment");
    std::string stage = str(m_pjob, "stage");
    if (stage == "downloading" || stage == "slicing" || stage == "uploading" || stage == "starting") return fail("A print is already being prepared");
    if (str(m_printer, "ip").empty()) return fail("Set up your printer first (Me > Printer)");
    json prof = profilesFor(str(m_printer, "model"));
    if (prof.is_null()) return fail("Only the Bambu Lab A1 and A1 mini are supported so far");
    modelId = unquote(modelId);
    auto it = m_cat.models.find(modelId);
    if (it == m_cat.models.end() || it->second.versions.empty()) return fail("No such model");
    int v = atoi(unquote(version).c_str());
    if (v <= 0) v = (int)it->second.versions.size();
    std::string key = modelId + "@" + std::to_string(v);
    m_pjob = json{{"stage", "downloading"}, {"key", key}, {"modelId", modelId}, {"v", v}, {"title", it->second.versions[v - 1].value("title", "")},
                  {"printer", str(prof, "label")}, {"profile", str(prof, "process")}, {"filament", str(prof, "filament")}, {"message", "Downloading and verifying the files..."}, {"at", nowMs()}};
    auto jt = m_jobs.find(key);
    if (jt != m_jobs.end() && jt->second.status == "done") { beginSlice(jt->second); return ok(json{{"job", m_pjob}}); }
    std::string r = download(modelId, std::to_string(v));
    json rj = json::parse(r, nullptr, false);
    if (!rj.is_object() || !rj.value("ok", false)) { m_pjob = nullptr; return r; }
    return ok(json{{"job", m_pjob}});
}

// Slice what was downloaded - geometry only: STL/OBJ/STEP as they are; a 3MF is first reduced to
// an STL, so settings and custom G-code embedded by whoever published it never reach the printer.
void SwampCoreImpl::beginSlice(const DownloadJob& j) {
    json prof = profilesFor(str(m_printer, "model"));
    std::string err;
    json slicer = orcaFor(prof, err);
    if (slicer.is_null()) { m_pjob["stage"] = "failed"; m_pjob["message"] = err; m_pjob["fix"] = slicerFix("", nullptr); publishState(); return; }
    std::string dataDir = m_dataDir;
    std::vector<std::string> meshes, projects;
    for (const auto& [sha, name] : j.files) {
        std::string e = lowerExt(name), path = j.dir + "/" + name;
        if (e == "stl" || e == "obj" || e == "step" || e == "stp" || e == "amf") meshes.push_back(path);
        else if (e == "3mf") projects.push_back(path);
    }
    if (meshes.empty() && projects.empty()) { m_pjob["stage"] = "failed"; m_pjob["message"] = "This version has no model files to print"; publishState(); return; }
    m_pjob["stage"] = "slicing";
    m_pjob["slicer"] = str(slicer, "name"); m_pjob["slicerProgram"] = str(slicer, "program");
    m_pjob["message"] = "Slicing with " + str(slicer, "name") + " (" + str(prof, "process") + ", " + str(prof, "filament") + ")...";
    publishState();
    std::string out = m_dataDir + "/print/" + newId().substr(0, 8), cfg = m_dataDir + "/orca-config";
    std::string remote = "swamp-" + str(m_pjob, "modelId").substr(0, 8) + "-v" + std::to_string(num(m_pjob, "v")) + ".gcode.3mf";
    runAsync([slicer, prof, meshes, projects, out, cfg, remote, dataDir] {
        std::string perr;
        json orca = locateBblProfiles(slicer, prof, dataDir, perr);
        if (orca.is_null()) return json{{"ok", false}, {"error", perr}};
        std::error_code ec;
        fs::create_directories(out, ec); fs::create_directories(cfg, ec);
        auto run = [&](QStringList args, std::string& log) {
            QProcess p;
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert("XDG_CONFIG_HOME", QString::fromStdString(cfg));   // don't touch the user's own slicer settings
            p.setProcessEnvironment(env);
            QStringList all;
            for (const auto& a : orca["args"]) all << QString::fromStdString(a.get<std::string>());
            all << args;
            p.start(QString::fromStdString(str(orca, "program")), all);
            bool fin = p.waitForFinished(15 * 60 * 1000);
            log = p.readAllStandardOutput().toStdString() + p.readAllStandardError().toStdString();
            return fin && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
        };
        std::vector<std::string> inputs = meshes;
        for (size_t i = 0; i < projects.size(); i++) {   // 3MF -> geometry only (swamp_3mf.hpp, not the slicer: Orca 2.4.2 crashes on 3MF input)
            std::string sub = out + "/geom" + std::to_string(i), gerr;
            fs::create_directories(sub, ec);
            std::vector<std::string> stls;
            if (!swamp3mf::toStls(projects[i], sub, stls, gerr))
                return json{{"ok", false}, {"error", "Couldn't read the shapes out of " + fs::path(projects[i]).filename().string() + ": " + gerr}};
            inputs.insert(inputs.end(), stls.begin(), stls.end());
        }
        std::string bbl = str(orca, "bbl"), log;
        QStringList args{"--arrange", "1", "--slice", "1",
                         "--load-settings", QString::fromStdString(bbl + "/machine/" + str(prof, "machine") + ".json;" + bbl + "/process/" + str(prof, "process") + ".json"),
                         "--load-filaments", QString::fromStdString(bbl + "/filament/" + str(prof, "filament") + ".json"),
                         "--outputdir", QString::fromStdString(out), "--export-3mf", QString::fromStdString(remote)};
        for (const auto& in : inputs) args << QString::fromStdString(in);
        if (!run(args, log)) {
            // the view turns the log into plain steps (slicerFix); the whole tail is there to copy
            std::string tail = log.substr(log.size() > 2000 ? log.size() - 2000 : 0);
            return json{{"ok", false}, {"error", str(orca, "name") + " couldn't slice this model."}, {"log", tail}};
        }
        std::string file = out + "/" + remote, gcode;
        if (!fs::exists(file, ec)) return json{{"ok", false}, {"error", "The slicer produced no file"}};
        // the estimate, from the G-code header
        json est = json::object();
        std::ifstream g(out + "/plate_1.gcode");
        std::string line;
        // the header has time and layers; the filament totals come later in the file
        while (std::getline(g, line)) {
            if (line.empty() || line[0] != ';') continue;
            auto at = line.find("total estimated time: ");
            if (at != std::string::npos) est["time"] = line.substr(at + 22);
            if (line.rfind("; total layer number", 0) == 0) est["layers"] = (int)numberAt(line, 20);
            // OrcaSlicer: "; filament used [mm] = 14168.04", "; filament used [cm3] = 34.08"
            // Bambu Studio: "; total filament length [mm] : 13732.79" (its weight line is 0 when the
            // profile's density is 0, so grams come from length)
            if (line.rfind("; filament used [mm]", 0) == 0 || line.rfind("; total filament length [mm]", 0) == 0) {
                double mm = numberAt(line, line.find(']') + 1);
                if (mm > 0) est["filamentM"] = std::round(mm / 100.0) / 10.0;
            }
            if (line.rfind("; filament used [cm3]", 0) == 0) { double cm3 = numberAt(line, line.find(']') + 1); if (cm3 > 0) est["filamentG"] = std::round(cm3 * 1.24); }   // PLA
            if (line.rfind("; total filament weight [g]", 0) == 0) { double g = numberAt(line, line.find(']') + 1); if (g > 0) est["filamentG"] = std::round(g); }
            if (est.contains("time") && est.contains("layers") && (est.contains("filamentG") || est.contains("filamentM")) && est.contains("filamentG")) break;
        }
        if (!est.contains("filamentG") && est.contains("filamentM"))   // 1.75 mm PLA
            est["filamentG"] = std::round(est["filamentM"].get<double>() * 1000.0 * 3.14159265 * 0.875 * 0.875 * 1.24 / 1000.0);
        return json{{"ok", true}, {"file", file}, {"remote", remote}, {"estimate", est}};
    }, [this](json r) {
        if (str(m_pjob, "stage") != "slicing") return;   // cancelled meanwhile
        if (!r.value("ok", false)) {
            m_pjob["stage"] = "failed"; m_pjob["message"] = r.value("error", "Slicing failed");
            m_pjob["log"] = r.value("log", "");
            json used; for (const auto& c : m_bambuSlicers) if (str(c, "program") == str(m_pjob, "slicerProgram")) used = c;
            m_pjob["fix"] = slicerFix(r.value("log", ""), used.is_object() ? used : json{{"name", str(m_pjob, "slicer")}});
        }
        else {
            m_pjob["stage"] = "ready"; m_pjob["file"] = r["file"]; m_pjob["remote"] = r["remote"]; m_pjob["estimate"] = r["estimate"];
            m_pjob["message"] = "Sliced. Check the printer has PLA loaded and a clean plate, then start the print.";
        }
        publishState();
    });
}

std::string SwampCoreImpl::startPrint(std::string confirm) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!m_experimentalPrint) return fail("Direct printing is switched off: it's an unfinished experiment (start Basecamp with SWAMP_EXPERIMENTAL_PRINT=1 to try it, at your own risk). Use Open in slicer to print.");
    if (str(m_pjob, "stage") != "ready") return fail("Nothing is ready to print");
    if (unquote(confirm) != "yes") return fail("Confirm to start the print");
    bambu::Printer p = printerConf();
    std::string file = str(m_pjob, "file"), remote = str(m_pjob, "remote"), title = str(m_pjob, "title");
    m_pjob["stage"] = "uploading"; m_pjob["message"] = "Sending the file to the printer..."; m_pjob["progress"] = 0;
    publishState();
    auto life = m_life;
    runAsync([this, life, p, file, remote, title] {
        std::string err;
        // never start into a printer that's busy with something else (review 2026-10-07)
        json st;
        if (!bambu::status(p, st, err)) return json{{"ok", false}, {"error", err}};
        std::string state = st.contains("gcode_state") && st["gcode_state"].is_string() ? st["gcode_state"].get<std::string>() : "";
        if (!state.empty() && state != "IDLE" && state != "FINISH" && state != "FAILED")
            return json{{"ok", false}, {"error", "The printer is busy (" + state + (st.contains("mc_percent") ? ", " + st["mc_percent"].dump() + "%" : "") + "). Wait until it's done, then start again."}};
        std::string ftpsPin;
        long long lastPost = 0;
        bool up = bambu::upload(p, file, remote, err, [&](long long s, long long t) {
            long long now = bambu::msNow();
            if (now - lastPost < 500 || t <= 0) return;
            lastPost = now;
            int pct = (int)(s * 100 / t);
            if (*life) onLoop([this, life, pct] { if (!*life) return; std::lock_guard<std::recursive_mutex> lk(m_mtx); if (str(m_pjob, "stage") == "uploading") m_pjob["progress"] = pct; });
        }, &ftpsPin);
        if (!up) return json{{"ok", false}, {"error", err}};
        if (*life) onLoop([this, life, ftpsPin] { if (!*life) return; std::lock_guard<std::recursive_mutex> lk(m_mtx); pinPrinterCert("certPinFtps", ftpsPin); });
        if (*life) onLoop([this, life] { if (!*life) return; std::lock_guard<std::recursive_mutex> lk(m_mtx); m_pjob["stage"] = "starting"; m_pjob["message"] = "Starting the print..."; publishState(); });
        if (!bambu::startPrint(p, remote, title, false, err)) return json{{"ok", false}, {"error", err}};
        return json{{"ok", true}};
    }, [this](json r) {
        if (!r.value("ok", false)) { m_pjob["stage"] = "failed"; m_pjob["message"] = r.value("error", "Printing failed"); }
        else { m_pjob["stage"] = "sent"; m_pjob["message"] = "The printer accepted the job."; m_printsSent++; m_printerStateAt = 0; refreshPrinterState(); }
        publishState();
    });
    return ok(json{{"job", m_pjob}});
}

std::string SwampCoreImpl::cancelPrint() {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    std::string st = str(m_pjob, "stage");
    if (st == "uploading" || st == "starting") return fail("The file is already on its way to the printer - stop it on the printer if needed");
    m_pjob = nullptr;
    publishState();
    return ok();
}

// ---- creators -------------------------------------------------------------------------------
std::string SwampCoreImpl::publish(std::string draftJson) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!m_loaded) return fail("Swamp is still starting - try again in a moment");
    json d = parseArg(draftJson);
    if (!d.is_object()) return fail("The draft is not valid JSON");
    if (!d.contains("files") || !d["files"].is_array() || d["files"].empty()) return fail("Add at least one file");
    std::string modelId = str(d, "modelId");
    // a model lives in one category, fixed when it's created (its topic, ADR 0014)
    std::string category = str(d, "category");
    if (category.empty()) category = "other";
    if (!knownCategory(category)) return fail("Pick a category");
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
        std::string path = str(f, "path"), bytes, name;
        if (path.empty() && isHex(str(f, "sha256"), 64)) {
            // a file this node already holds (carried over from an earlier version)
            if (!haveBlob(str(f, "sha256")) || !readFile(blobPath(str(f, "sha256")), bytes)) return fail("A file from the previous version isn't on this device any more - add it again");
            name = str(f, "name");
        } else {
            if (!readFile(path, bytes) || bytes.empty()) return fail("Can't read " + path);
            name = fs::path(path).filename().string();
        }
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
    if (!nonce.empty()) author("model.create", json{{"modelId", modelId}, {"nonce", nonce}, {"title", str(d, "title")}, {"category", category}});
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

std::string SwampCoreImpl::setCategories(std::string listJson) {
    std::lock_guard<std::recursive_mutex> lk(m_mtx);
    if (!m_loaded) return fail("Swamp is still starting - try again in a moment");
    json a = parseArg(listJson);
    if (!a.is_array()) return fail("Expected a list of categories");
    std::set<std::string> want;
    for (const auto& c : a) {
        if (!c.is_string()) continue;
        if (c.get<std::string>() == "all") { for (const auto& [id, label] : categories()) want.insert(id); continue; }
        if (!knownCategory(c.get<std::string>())) return fail("Unknown category: " + c.get<std::string>());
        want.insert(c.get<std::string>());
    }
    // categories you publish in stay: comments on your models have to reach you
    for (const auto& [id, m] : m_cat.models) if (m.creator == m_id.address) want.insert(m.category);
    std::set<std::string> added;
    for (const auto& c : want) if (!m_subs.count(c)) added.insert(c);
    m_subs = want;
    saveSettings();
    for (const auto& c : added) { ensureJoined(categoryTopic(c)); if (m_ready) catchupOn(categoryTopic(c)); }
    publishState();
    return ok(json{{"categories", categoriesJson()}});
}

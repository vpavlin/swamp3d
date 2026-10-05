#pragma once
// SwampCoreImpl - Swamp (a decentralized library of 3D models) as a Logos CORE module
// (universal authoring, Basecamp 0.3 / builder 0.3.1). It owns everything: the signed public
// catalogue (loam-sync events over loam_core), publishing (hash, thumbnail, fingerprint, stage,
// upload to the host's Storage node, announce CIDs), verified downloads, and - with SWAMP_HUB=1 -
// caching every file it sees (a pinning hub). The `swamp` ui_qml view only calls these actions.
// docs/SPEC.md.
//
// Module rules (logos-basecamp-module + logos-basecamp-0.3-port): public methods return a JSON
// std::string ({"ok":true,...} or {"ok":false,"error":"<sentence>"}), at most 4 args, no default
// arguments, no trailing comments on declaration lines; startup calls to other modules are
// deferred out of onContextReady().
#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <functional>
#include <memory>
#include <atomic>
#include "logos_module_context.h"
#include "swamp_catalog.hpp"

class QTimer;

class SwampCoreImpl : public LogosModuleContext {
public:
    ~SwampCoreImpl() override;

    std::string snapshot();
    std::string resync();
    std::string listModels(std::string queryJson);
    std::string getModel(std::string modelId);
    // copy a picture this node holds into <viewDir>/cache/ and return its path (views are sandboxed)
    std::string cacheImage(std::string sha, std::string viewDir);

    // creators
    std::string publish(std::string draftJson);
    std::string retract(std::string modelId, std::string reason);

    // makers
    std::string download(std::string modelId, std::string version);
    std::string comment(std::string modelId, std::string text);
    std::string postMake(std::string modelId, std::string makeJson);
    std::string like(std::string modelId, std::string on);
    std::string setProfile(std::string profileJson);

protected:
    void onContextReady() override;

logos_events:
    void stateChanged(const std::string& summaryJson);

private:
    struct PendingUpload { std::string sha; long long since = 0; };
    // One entry per blob being fetched: which candidate CID we're on, retries, and the transfer.
    struct Fetch { size_t cidIdx = 0; int rounds = 0; long long nextTry = 0, since = 0, size = 0, seenSize = 0, grewAt = 0; bool inflight = false, gaveUp = false; std::string cid, session, error; };
    struct DownloadJob { std::string modelId; int v = 0; std::string dir; std::vector<std::pair<std::string, std::string>> files; std::string status, error; };

    // persistence + identity
    void setupDataDir();
    void loadAll();
    void saveLog();
    void saveJobs();
    std::string blobPath(const std::string& sha) const;
    bool haveBlob(const std::string& sha) const;
    std::string storeBlob(const std::string& bytes);

    // catalogue
    void refold();
    swamp::Event author(const std::string& type, const swamp::json& payload);
    bool ingest(const swamp::Event& e);
    swamp::json card(const swamp::Model& m);
    std::string nameOf(const std::string& address);
    std::set<std::string> myBlobs();

    // transport (loam_core)
    void startModules();
    void startTransport();
    void onStatus(const std::string& s);
    void sendFrame(const swamp::json& frame);
    void onFrame(const std::string& topic, const std::string& payloadB64, int64_t sentAt);
    void handleFrame(const swamp::json& f, bool live);
    void serveEvents(const std::vector<swamp::Event>& evs);
    void catchupRound();

    // storage (host-owned node)
    void ensureStorage();
    void uploadBlob(const std::string& sha);
    void completeUpload(const std::string& payload);
    void flushAnnouncements();
    void startFetch(const std::string& sha, long long size);
    void fetchFailed(const std::string& sha, const std::string& why);
    void completeDownload(const std::string& sessionId, bool ok, const std::string& error);
    void finishFetched(const std::string& sha);
    void pollStorage();
    void advanceJobs();
    void hubSweep();
    void fetchPreviews();
    void retryUploads();
    bool storageFree();
    bool jobWaiting();
    void storageDone();

    void tick();
    void onLoop(std::function<void()> fn);
    void publishState();
    long long nowMs() const;
    std::string fail(const std::string& why);
    std::string newId();

    std::recursive_mutex m_mtx;
    std::string m_dataDir, m_downloadsDir, m_status = "Starting...";
    bool m_storageOk = true, m_ready = false, m_transportStarted = false, m_storageStarted = false, m_storageHostOwned = false, m_hub = false;
    bool m_dirty = false, m_unsaved = false, m_loaded = false;
    // false once destroyed: module/storage callbacks hold a copy and check it before touching `this`
    std::shared_ptr<std::atomic<bool>> m_life = std::make_shared<std::atomic<bool>>(true);
    swamp::Identity m_id;
    std::vector<swamp::Event> m_log;
    std::set<std::string> m_logIds;
    swamp::Catalog m_cat;
    std::map<std::string, PendingUpload> m_upSessions;
    std::map<std::string, long long> m_upTried;
    std::map<std::string, Fetch> m_fetch;
    std::map<std::string, DownloadJob> m_jobs;
    std::map<std::string, std::string> m_myCids, m_toAnnounce;
    std::map<std::string, long long> m_announcedAt, m_answeredAt;
    QTimer* m_timer = nullptr;
    // one Storage request at a time: storage 3.x serves calls in turn and a download start can
    // wait ~30 s for a manifest, so queued calls would pile up past the IPC timeout
    bool m_storageBusy = false;
    long long m_storageBusySince = 0;
    long long m_lastCatchup = 0, m_lastSave = 0, m_serveWindow = 0, m_lastStatusPoll = 0, m_lastManifestPoll = 0, m_announceHeldSince = 0;
    int m_servedInWindow = 0;
    long m_rx = 0, m_tx = 0, m_rxEvents = 0, m_rxBad = 0, m_uploaded = 0, m_fetched = 0, m_verifyFailed = 0, m_tooBig = 0, m_servedEvents = 0, m_throttled = 0, m_staleCatchup = 0, m_stalled = 0;
};

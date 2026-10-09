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
#include <deque>
#include "logos_module_context.h"
#include "swamp_catalog.hpp"
#include "swamp_bambu.hpp"

class QTimer;

class SwampCoreImpl : public LogosModuleContext {
public:
    ~SwampCoreImpl() override;

    std::string snapshot();
    std::string resync();
    std::string listModels(std::string queryJson);
    std::string getModel(std::string modelId);
    // a share link (swamp://model/<id>?c=<category>) -> {modelId}; listens on its category so the model can arrive
    std::string openLink(std::string link);
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
    // categories this node mirrors (ADR 0014): a JSON array of category ids, or ["all"]
    std::string setCategories(std::string listJson);
    // search every category through the newest index snapshot (ADR 0015): {q, category, limit};
    // {pending:true} while the needed shards are being fetched - ask again
    std::string globalSearch(std::string queryJson);
    // download (if needed) and open the version's model files in the installed desktop slicer
    std::string openInSlicer(std::string modelId, std::string version);

    // printing on a Bambu Lab printer over the LAN (LAN-only + Developer Mode; no cloud)
    // listen for printers on the LAN (~6 s; ask again for the result)
    std::string findPrinters();
    // {ip, serial, accessCode, model, name}; the access code is stored owner-only and never returned
    std::string setPrinter(std::string printerJson);
    std::string printerStatus();
    // download (verified) + slice with the printer's default profile; then startPrint("yes")
    std::string preparePrint(std::string modelId, std::string version);
    std::string startPrint(std::string confirm);
    // download the OrcaSlicer build Swamp is tested with (pinned, hash-checked) and use it for printing
    std::string installSlicer();
    std::string cancelPrint();

protected:
    void onContextReady() override;

logos_events:
    void stateChanged(const std::string& summaryJson);

private:
    struct PendingUpload { std::string sha; long long since = 0; };
    struct TopicHealth { long tx = 0; long long firstTx = 0, lastRx = 0; };
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
    void sendFrame(const std::string& topic, const swamp::json& frame);
    void onFrame(const std::string& topic, const std::string& payloadB64, int64_t sentAt);
    void handleFrame(const std::string& topic, const swamp::json& f, bool live);
    void serveEvents(const std::string& topic, const std::vector<swamp::Event>& evs, bool budgeted = true);
    void catchupRound();
    void catchupOn(const std::string& topic);
    std::vector<std::string> subscribedTopics() const;
    bool isSubscribedTopic(const std::string& topic) const;
    void ensureJoined(const std::string& topic);
    void subscribe(const std::string& cat);
    std::vector<swamp::Event> eventsOn(const std::string& topic);
    void loadSettings();
    void indexTick();
    swamp::json bestManifest();
    std::string globalSearchImpl(const std::string& queryJson);
    swamp::json shard(const swamp::json& manifest, const std::string& key);
    bool fetchRecord(const std::string& modelId, bool& pending);
    void refreshRecord(const std::string& modelId);
    void checkInclusion();
    swamp::json findSlicer();
    void runAsync(std::function<swamp::json()> work, std::function<void(swamp::json)> done);
    swamp::bambu::Printer printerConf();
    swamp::json printerPublic();
    void refreshPrinterState();
    void pinPrinterCert(const char* field, const std::string& pin);
    swamp::json orcaFor(const swamp::json& profiles, std::string& err);
    void beginSlice(const DownloadJob& j);
    swamp::json m_printer, m_printerState, m_pjob, m_discovered = swamp::json::array();
    std::string m_printerStateErr;
    long long m_printerStateAt = 0, m_discoveredAt = 0;
    bool m_discovering = false, m_printerPolling = false;
    long m_printsSent = 0;
    swamp::json detectSlicer();
    std::vector<swamp::json> detectSlicers(bool bambuOnly);
    std::vector<swamp::json> m_bambuSlicers;
    swamp::json m_slicer;
    long long m_slicerAt = 0;
    // hand-off to hubs (see hubHandoff)
    struct Handoff { std::string sha; long long size = 0; int i = 0, n = 0; };
    struct Assembly { std::vector<std::string> parts; size_t got = 0; long long lastAt = 0; };
    Handoff m_handoff;
    std::map<std::string, long long> m_handoffSent;            // sha -> when last fully sent
    std::map<std::string, std::set<std::string>> m_cidsFrom;   // sha -> who announced CIDs for it
    std::map<std::string, Assembly> m_assembly;                // hub: files being put back together
    std::set<std::string> m_hubBlobs;                          // hub: files received by hand-off
    long m_handoffFrames = 0, m_hubReceived = 0, m_handoffRejected = 0;
    void hubHandoff();
    void onHubFrame(const swamp::json& f);
    void acceptHandoff(const std::string& sha, const std::string& all);
    std::map<std::string, std::pair<std::string, long long>> m_unlisted;   // hub: sha -> (bytes, when)
    void sendUnjoined(const std::string& topic, const swamp::json& frame);
    long long listedSize(const std::string& sha) const;
    bool heldByHub(const std::string& sha) const;
    swamp::json m_slicerInstall;                      // {stage: downloading|unpacking|checking|done|failed, bytes, total, message, fix}
    std::atomic<long long> m_slicerBytes{0};
    swamp::json managedSlicer() const;
    swamp::json slicerInstallState() const;
    swamp::json slicerFix(const std::string& log, const swamp::json& slicer) const;
    std::string launchSlicer(const DownloadJob& j);
    swamp::json omissionsJson();
    swamp::json transportHealth();
    std::map<std::string, TopicHealth> m_topicHealth;
    swamp::json suspectsJson();
    swamp::json excludedMineJson();
    std::vector<std::string> cidsFor(const std::string& sha);
    long long imageSize(const std::string& sha);
    swamp::json categoriesJson();
    void saveSettings();

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
    bool m_experimentalPrint = false;   // SWAMP_EXPERIMENTAL_PRINT=1
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
    std::set<std::string> m_subs, m_joined;
    std::set<std::string> m_linked;   // models opened from a share link this session (at most 256)
    std::deque<std::string> m_linkedOrder;
    std::map<std::string, long long> m_wantImg, m_extraImg, m_recordAt;
    std::map<std::string, std::vector<std::string>> m_extraCids;
    std::set<std::string> m_privateFetch, m_indexShas, m_openAfter;
    std::map<std::string, swamp::json> m_shardCache;
    swamp::json m_indexPending;
    std::string m_indexRoot, m_testOmit;
    bool m_indexer = false, m_privateShards = false;
    long long m_lastIndex = 0, m_indexEveryMs = 30LL * 60 * 1000, m_indexUploadTimeoutMs = 10LL * 60 * 1000;
    long long m_lastInclusion = 0, m_inclusionEveryMs = 5LL * 60 * 1000, m_inclusionGraceMs = 10LL * 60 * 1000;
    std::map<std::string, swamp::json> m_omissions, m_suspects;   // keyed "<indexer>|<modelId>"
    bool caughtOmitting(const std::string& who) const;
    long long m_omissionConfirmMs = 10LL * 60 * 1000;
    std::map<std::string, std::string> m_excludedMine;
    long m_indexesBuilt = 0, m_privacyDowngrades = 0, m_slicerLaunches = 0, m_badShards = 0;
    std::map<std::string, long long> m_announcedAt, m_answeredAt;
    QTimer* m_timer = nullptr;
    // one Storage request at a time: storage 3.x serves calls in turn and a download start can
    // wait ~30 s for a manifest, so queued calls would pile up past the IPC timeout
    bool m_storageBusy = false;
    long long m_storageBusySince = 0;
    long long m_lastCatchup = 0, m_lastSave = 0, m_serveWindow = 0, m_lastStatusPoll = 0, m_lastManifestPoll = 0, m_announceHeldSince = 0;
    int m_servedInWindow = 0;
    long m_rxEventsAtCatchup = 0;
    long long m_readyAt = 0;
    long m_rx = 0, m_tx = 0, m_rxEvents = 0, m_rxBad = 0, m_uploaded = 0, m_fetched = 0, m_verifyFailed = 0, m_tooBig = 0, m_servedEvents = 0, m_throttled = 0, m_staleCatchup = 0, m_stalled = 0;
};

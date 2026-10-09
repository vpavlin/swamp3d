#pragma once
// FAKE logos SDK (test-only): enough of the universal-module surface for swamp_core_impl.cpp to
// build and run outside nix/Basecamp. modules().loam_core is an in-process message bus
// (fake_bus.h); modules().storage_module is an in-process content store (fake_storage.h) that
// behaves like Logos Storage where it matters: CIDs depend on the file name, only ONLINE holders
// can serve a CID, and a node that downloads with advertise=true becomes a holder.
#include <functional>
#include <string>
#include <cstdint>
#include <nlohmann/json.hpp>

#define logos_events public

using LogosMap = nlohmann::json;
struct StdLogosResult { bool success = true; std::string error; LogosMap value; };
namespace logos {
template <class T> struct AsyncResult {
    T value; struct { std::string code; } error; bool good = true;
    bool ok() const { return good; }
};
}

struct FakeLoamNode;
struct FakeLoamCore {
    FakeLoamNode* node = nullptr;
    using RecvFn = std::function<void(const std::string&, const std::string&, const std::string&, int64_t)>;
    using StatusFn = std::function<void(const std::string&)>;
    using Cb = std::function<void(std::string)>;
    void onReceived(RecvFn fn);
    void onStatusChanged(StatusFn fn);
    void setSenderIdAsync(const std::string& id, Cb cb);
    void startAsync(const std::string& cfg, Cb cb);
    void joinAsync(const std::string& topic, Cb cb);
    void sendSealedAsync(const std::string& topic, const std::string& b64, Cb cb);
    void statusAsync(Cb cb);
};

struct FakeStoreNode;
struct FakeStorage {
    FakeStoreNode* node = nullptr;
    using EvFn = std::function<void(const std::string&)>;
    void onStorageUploadDone(EvFn fn);
    void onStorageDownloadDone(EvFn fn);
    bool init(const std::string& cfg);
    void start();
    StdLogosResult uploadUrl(const std::string& path, int chunk, bool advertise);
    StdLogosResult manifests();
    StdLogosResult downloadCancel(const std::string& sessionId);
    using ResCb = std::function<void(logos::AsyncResult<StdLogosResult>)>;
    void uploadUrlAsyncResult(const std::string& path, int chunk, bool advertise, ResCb cb, int timeoutMs);
    void manifestsAsyncResult(ResCb cb, int timeoutMs);
    void downloadCancelAsyncResult(const std::string& sessionId, ResCb cb, int timeoutMs);
    void downloadToUrlAsyncResult(const std::string& cid, const std::string& path, bool local, int chunk, bool isPrivate, bool advertise,
                                  std::function<void(logos::AsyncResult<StdLogosResult>)> cb, int timeoutMs);
    void fetchAsyncResult(const std::string& cid, bool isPrivate, bool advertise, ResCb cb, int timeoutMs);
};

struct FakeModules { FakeLoamCore loam_core; FakeStorage storage_module; };

class LogosModuleContext {
public:
    virtual ~LogosModuleContext() = default;
    FakeModules& modules() { return m_fakeModules; }
    void fakeStart() { onContextReady(); }
    FakeModules m_fakeModules;
protected:
    virtual void onContextReady() {}
};

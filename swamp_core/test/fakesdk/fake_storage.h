#pragma once
#include "logos_module_context.h"
#include <QCoreApplication>
#include <QTimer>
#include <map>
#include <set>
#include <fstream>
#include <sstream>
#include <openssl/sha.h>

struct FakeStoreNode {
    std::string name;
    bool online = true, dropEvents = false;
    FakeStorage::EvFn onUp, onDown;
    std::map<std::string, std::string> held;      // cid -> bytes
    std::vector<LogosMap> manifests;             // what this node uploaded
    int sess = 0;
};
struct FakeStoreNet {
    std::vector<FakeStoreNode*> nodes;
    static FakeStoreNet& get() { static FakeStoreNet n; return n; }
    static void later(int ms, std::function<void()> fn) { QTimer::singleShot(ms, QCoreApplication::instance(), fn); }
    static std::string cidOf(const std::string& filename, const std::string& bytes) {
        unsigned char h[32]; std::string in = filename + "\n" + bytes;
        SHA256((const unsigned char*)in.data(), in.size(), h);
        static const char* X = "0123456789abcdef"; std::string s = "zFake";
        for (int i = 0; i < 12; i++) { s += X[h[i] >> 4]; s += X[h[i] & 15]; }
        return s;
    }
    const std::string* find(const std::string& cid) {
        for (auto* n : nodes) if (n->online) { auto it = n->held.find(cid); if (it != n->held.end()) return &it->second; }
        return nullptr;
    }
};
inline void FakeStorage::onStorageUploadDone(EvFn fn) { node->onUp = fn; }
inline void FakeStorage::onStorageDownloadDone(EvFn fn) { node->onDown = fn; }
inline bool FakeStorage::init(const std::string&) { return false; }   // like Basecamp 0.3: the host owns it
inline void FakeStorage::start() {}
inline StdLogosResult FakeStorage::uploadUrl(const std::string& path, int, bool) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return StdLogosResult{false, "no such file", nullptr};
    std::stringstream ss; ss << f.rdbuf();
    std::string bytes = ss.str(), fname = path.substr(path.find_last_of('/') + 1);
    std::string cid = FakeStoreNet::cidOf(fname, bytes);
    std::string sess = node->name + "-up-" + std::to_string(++node->sess);
    FakeStoreNode* n = node;
    FakeStoreNet::later(20, [n, cid, bytes, fname, sess] {
        n->held[cid] = bytes;
        n->manifests.push_back(LogosMap{{"cid", cid}, {"filename", fname}, {"datasetSize", bytes.size()}});
        if (n->onUp && !n->dropEvents) n->onUp(LogosMap{{"sessionId", sess}, {"success", true}, {"cid", cid}}.dump());
    });
    return StdLogosResult{true, "", sess};
}
inline StdLogosResult FakeStorage::manifests() { return StdLogosResult{true, "", LogosMap(node->manifests)}; }
inline void FakeStorage::downloadToUrlAsyncResult(const std::string& cid, const std::string& path, bool, int, bool, bool advertise,
                                                  std::function<void(logos::AsyncResult<StdLogosResult>)> cb, int) {
    FakeStoreNode* n = node;
    std::string sess = n->name + "-down-" + std::to_string(++n->sess);
    FakeStoreNet::later(5, [cb, sess] { logos::AsyncResult<StdLogosResult> r; r.value = StdLogosResult{true, "", sess}; cb(r); });
    FakeStoreNet::later(40, [n, cid, path, advertise, sess] {
        const std::string* src = FakeStoreNet::get().find(cid);
        if (!src) { if (n->onDown && !n->dropEvents) n->onDown(LogosMap{{"sessionId", sess}, {"success", false}, {"error", "no provider"}}.dump()); return; }
        std::string bytes = *src;
        { std::ofstream o(path, std::ios::binary); o << bytes; }
        if (advertise) n->held[cid] = bytes;
        if (n->onDown && !n->dropEvents) n->onDown(LogosMap{{"sessionId", sess}, {"success", true}}.dump());
    });
}
inline StdLogosResult FakeStorage::downloadCancel(const std::string&) { return StdLogosResult{true, "", nullptr}; }
// async variants: the same calls, answered on the event loop like the real IPC
inline void FakeStorage::uploadUrlAsyncResult(const std::string& path, int chunk, bool advertise, ResCb cb, int) {
    logos::AsyncResult<StdLogosResult> r; r.value = uploadUrl(path, chunk, advertise);
    FakeStoreNet::later(2, [cb, r] { cb(r); });
}
inline void FakeStorage::manifestsAsyncResult(ResCb cb, int) {
    logos::AsyncResult<StdLogosResult> r; r.value = manifests();
    FakeStoreNet::later(2, [cb, r] { cb(r); });
}
inline void FakeStorage::downloadCancelAsyncResult(const std::string& s, ResCb cb, int) {
    logos::AsyncResult<StdLogosResult> r; r.value = downloadCancel(s);
    FakeStoreNet::later(2, [cb, r] { cb(r); });
}
inline void FakeStorage::fetchAsyncResult(const std::string& cid, bool, bool, ResCb cb, int) {
    FakeStoreNode* n = node;
    FakeStoreNet::later(5, [cb] { logos::AsyncResult<StdLogosResult> r; r.value = StdLogosResult{true, "", nullptr}; cb(r); });
    FakeStoreNet::later(60, [n, cid] {   // like the real one: copies it in if some online node holds it
        if (n->held.count(cid)) return;
        if (const std::string* b = FakeStoreNet::get().find(cid)) n->held[cid] = *b;
    });
}
inline void FakeStorage::existsAsyncResult(const std::string& cid, ResCb cb, int) {
    bool have = node->held.count(cid) > 0;
    FakeStoreNet::later(2, [cb, have] { logos::AsyncResult<StdLogosResult> r; r.value = StdLogosResult{true, "", have}; cb(r); });
}

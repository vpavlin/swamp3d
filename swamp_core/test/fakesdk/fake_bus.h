#pragma once
// In-process stand-in for loam_core + the Waku fleet (test-only). Frames sent on a topic
// reach every OTHER started node that joined it (SDS filters a sender's own frames),
// asynchronously on the Qt event loop, with one extra base64 layer like the real wire.
#include "logos_module_context.h"
#include <QCoreApplication>
#include <QTimer>
#include <set>
#include <vector>
#include <openssl/evp.h>

struct FakeLoamNode {
    std::string name, senderId;
    bool up = false, online = true, dropStatusEvent = false;
    std::set<std::string> topics;
    FakeLoamCore::RecvFn onRecv;
    FakeLoamCore::StatusFn onStatus;
    long rx = 0, tx = 0;
};

struct FakeLoamBus {
    std::vector<FakeLoamNode*> nodes;
    static FakeLoamBus& get() { static FakeLoamBus b; return b; }
    static std::string b64(const std::string& s) {
        std::string out(4 * ((s.size() + 2) / 3) + 1, '\0');
        int n = EVP_EncodeBlock((unsigned char*)out.data(), (const unsigned char*)s.data(), (int)s.size());
        out.resize(n); return out;
    }
    static void later(std::function<void()> fn) { QTimer::singleShot(0, QCoreApplication::instance(), fn); }
    bool alive(FakeLoamNode* n) { for (auto* x : nodes) if (x == n) return true; return false; }
    void send(FakeLoamNode* from, const std::string& topic, const std::string& b64payload) {
        if (!from->online) return;
        for (FakeLoamNode* n : nodes) {
            if (n == from || !n->up || !n->online || !n->topics.count(topic) || !n->onRecv) continue;
            std::string sid = from->senderId;
            later([this, n, topic, sid, b64payload] { if (alive(n) && n->online) { n->rx++; n->onRecv(topic, sid, b64(b64payload), 0); } });
        }
    }
};

inline void FakeLoamCore::onReceived(RecvFn fn) { node->onRecv = fn; }
inline void FakeLoamCore::onStatusChanged(StatusFn fn) { node->onStatus = fn; }
inline void FakeLoamCore::setSenderIdAsync(const std::string& id, Cb cb) { node->senderId = id; FakeLoamBus::later([cb] { cb(""); }); }
inline void FakeLoamCore::startAsync(const std::string&, Cb cb) {
    FakeLoamNode* n = node;
    FakeLoamBus::later([n, cb] { n->up = true; cb(""); if (n->onStatus && !n->dropStatusEvent) n->onStatus("Connected"); });
}
inline void FakeLoamCore::joinAsync(const std::string& topic, Cb cb) { node->topics.insert(topic); FakeLoamBus::later([cb] { cb(""); }); }
inline void FakeLoamCore::sendSealedAsync(const std::string& topic, const std::string& b64, Cb cb) {
    node->tx++; FakeLoamBus::get().send(node, topic, b64); FakeLoamBus::later([cb] { cb(""); });
}
inline void FakeLoamCore::statusAsync(Cb cb) {
    FakeLoamNode* n = node;
    FakeLoamBus::later([n, cb] { cb(n->up ? "Connected" : "Connecting..."); });
}

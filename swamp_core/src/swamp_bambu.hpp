#pragma once
// swamp_bambu.hpp - talk to a Bambu Lab printer on the LAN, nothing else: no cloud, no account.
// Works with the printer in LAN-only mode + Developer Mode (since the 2025-01 firmware, normal LAN
// mode needs Bambu's authorization for print commands; Developer Mode doesn't).
//
//   discover()  listen for the printer's SSDP NOTIFY on UDP 2021 -> ip, serial, model, name
//   upload()    FTPS (implicit TLS, port 990, user bblp / access code) -> the SD card root
//   command()   MQTT 3.1.1 over TLS (port 8883, bblp / access code): publish to
//               device/<serial>/request, read device/<serial>/report
//
// Everything here BLOCKS (with timeouts): call it from a worker thread, never the module's loop.
// The printer's TLS certificate is signed by Bambu's own CA, which we don't ship; on a LAN we
// connect without verifying it (the access code still authenticates us).
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <chrono>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <nlohmann/json.hpp>

namespace swamp {
namespace bambu {

using json = nlohmann::json;

// certPin*: SHA-256 of the printer's TLS certificate per service, pinned on first use (TOFU): the
// certificate can't be checked against a CA we ship, but a device that later presents a different
// one - someone posing as the printer to collect the access code - is refused.
struct Printer { std::string ip, serial, accessCode, model, name, certPinMqtt, certPinFtps; int mqttPort = 8883, ftpsPort = 990; };

inline long long msNow() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ---- TCP + TLS -------------------------------------------------------------------------------
inline int tcpConnect(const std::string& host, int port, int timeoutMs, std::string& err) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) { err = "can't resolve " + host; return -1; }
    int fd = -1;
    for (addrinfo* a = res; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) err = "can't connect to " + host + ":" + std::to_string(port);
    return fd;
}

class Tls {
public:
    ~Tls() { shut(); if (ctx_) SSL_CTX_free(ctx_); }
    bool open(const std::string& host, int port, int timeoutMs, std::string& err, SSL_SESSION* reuse = nullptr) {
        fd_ = tcpConnect(host, port, timeoutMs, err);
        if (fd_ < 0) return false;
        ctx_ = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, nullptr);
        ssl_ = SSL_new(ctx_);
        SSL_set_fd(ssl_, fd_);
        SSL_set_tlsext_host_name(ssl_, host.c_str());
        if (reuse) SSL_set_session(ssl_, reuse);   // FTPS data channel: the printer wants the control session reused
        if (SSL_connect(ssl_) != 1) { err = "TLS handshake with " + host + " failed"; return false; }
        if (X509* cert = SSL_get1_peer_certificate(ssl_)) {
            unsigned char md[32]; unsigned int n = 0;
            if (X509_digest(cert, EVP_sha256(), md, &n) == 1) { static const char* X = "0123456789abcdef"; for (unsigned i = 0; i < n; i++) { pin_ += X[md[i] >> 4]; pin_ += X[md[i] & 15]; } }
            X509_free(cert);
        }
        return true;
    }
    // TOFU check: empty `expected` = first contact (the caller stores pin())
    bool pinOk(const std::string& expected, std::string& err) const {
        if (expected.empty() || expected == pin_) return true;
        err = "The printer's certificate changed since you set it up - is this really your printer? (Set it up again if you replaced or reset it.)";
        return false;
    }
    const std::string& pin() const { return pin_; }
    bool write(const std::string& s) {
        size_t off = 0;
        while (off < s.size()) { int n = SSL_write(ssl_, s.data() + off, (int)(s.size() - off)); if (n <= 0) return false; off += n; }
        return true;
    }
    // read up to n bytes (blocks until at least 1 or timeout)
    int read(char* buf, int n) { return ssl_ ? SSL_read(ssl_, buf, n) : -1; }
    bool readExact(std::string& out, size_t n) {
        out.clear();
        char buf[4096];
        while (out.size() < n) { int r = read(buf, (int)std::min<size_t>(sizeof buf, n - out.size())); if (r <= 0) return false; out.append(buf, r); }
        return true;
    }
    bool readLine(std::string& line) {   // CRLF-terminated
        line.clear();
        char c;
        while (true) { int r = read(&c, 1); if (r <= 0) return false; if (c == '\n') return true; if (c != '\r') line += c; }
    }
    SSL_SESSION* session() { return ssl_ ? SSL_get1_session(ssl_) : nullptr; }
    // A polite close: send close_notify and wait (briefly) for the peer's, so the server sees a
    // clean end of the upload instead of a reset.
    void shut() {
        if (ssl_) {
            if (SSL_shutdown(ssl_) == 0) { char b[256]; for (int i = 0; i < 64 && SSL_read(ssl_, b, sizeof b) > 0; i++) {} SSL_shutdown(ssl_); }
            SSL_free(ssl_); ssl_ = nullptr;
        }
        if (fd_ >= 0) { close(fd_); fd_ = -1; }
    }
private:
    int fd_ = -1;
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    std::string pin_;
};

// ---- FTPS (implicit TLS) ---------------------------------------------------------------------
// An FTP reply: "227 Entering..." or a multi-line "220-..." ... "220 ...". Returns the code.
inline int ftpReply(Tls& t, std::string& text) {
    std::string line;
    text.clear();
    if (!t.readLine(line) || line.size() < 3) return -1;
    text = line;
    if (line.size() > 3 && line[3] == '-') {   // multi-line: read until "<code> "
        std::string code = line.substr(0, 3);
        while (t.readLine(line)) { text += "\n" + line; if (line.compare(0, 4, code + " ") == 0) break; }
    }
    return atoi(line.substr(0, 3).c_str());
}

inline bool upload(const Printer& p, const std::string& localPath, const std::string& remoteName, std::string& err,
                   std::function<void(long long, long long)> progress = nullptr, std::string* seenPin = nullptr) {
    std::ifstream f(localPath, std::ios::binary);
    if (!f) { err = "can't read " + localPath; return false; }
    f.seekg(0, std::ios::end); long long total = f.tellg(); f.seekg(0);
    Tls ctl;
    if (!ctl.open(p.ip, p.ftpsPort, 15000, err)) return false;
    if (!ctl.pinOk(p.certPinFtps, err)) return false;   // before sending the access code
    if (seenPin) *seenPin = ctl.pin();
    std::string txt;
    auto cmd = [&](const std::string& c, int want, const char* what) {
        if (!c.empty() && !ctl.write(c + "\r\n")) { err = std::string("FTPS: ") + what + ": connection lost"; return false; }
        int code = ftpReply(ctl, txt);
        if (code / 100 != want / 100) { err = std::string("FTPS: ") + what + ": " + (txt.empty() ? "no reply" : txt); return false; }
        return true;
    };
    if (!cmd("", 220, "greeting") || !cmd("USER bblp", 331, "login") || !cmd("PASS " + p.accessCode, 230, "access code rejected?")) return false;
    if (!cmd("TYPE I", 200, "binary mode") || !cmd("PBSZ 0", 200, "PBSZ") || !cmd("PROT P", 200, "PROT")) return false;
    if (!cmd("PASV", 227, "passive mode")) return false;
    // 227 Entering Passive Mode (h1,h2,h3,h4,p1,p2): use the printer's own IP, its port
    size_t l = txt.find('('), r = txt.find(')');
    int a[6] = {0};
    if (l == std::string::npos || r == std::string::npos || sscanf(txt.c_str() + l + 1, "%d,%d,%d,%d,%d,%d", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) != 6) { err = "FTPS: bad PASV reply: " + txt; return false; }
    int dport = a[4] * 256 + a[5];
    if (!ctl.write("STOR " + remoteName + "\r\n")) { err = "FTPS: STOR: connection lost"; return false; }
    SSL_SESSION* sess = ctl.session();
    Tls data;
    bool dataOk = data.open(p.ip, dport, 15000, err, sess);
    if (sess) SSL_SESSION_free(sess);
    if (!dataOk) return false;
    int code = ftpReply(ctl, txt);
    if (code != 150 && code != 125) { err = "FTPS: STOR refused: " + txt; return false; }
    std::vector<char> buf(64 * 1024);
    long long sent = 0;
    while (f) {
        f.read(buf.data(), buf.size());
        std::streamsize n = f.gcount();
        if (n <= 0) break;
        if (!data.write(std::string(buf.data(), (size_t)n))) { err = "FTPS: upload interrupted"; return false; }
        sent += n;
        if (progress) progress(sent, total);
    }
    data.shut();
    if (ftpReply(ctl, txt) / 100 != 2) { err = "FTPS: upload not confirmed: " + txt; return false; }
    ctl.write("QUIT\r\n");
    return true;
}

// ---- MQTT 3.1.1 ------------------------------------------------------------------------------
inline std::string mqttLen(size_t n) {
    std::string out;
    do { unsigned char b = n % 128; n /= 128; if (n) b |= 0x80; out += (char)b; } while (n);
    return out;
}
inline std::string mqttStr(const std::string& s) { return std::string{(char)(s.size() >> 8), (char)(s.size() & 0xff)} + s; }

class Mqtt {
public:
    bool connect(const Printer& p, std::string& err) {
        p_ = p;
        if (!t_.open(p.ip, p.mqttPort, 15000, err)) return false;
        if (!t_.pinOk(p.certPinMqtt, err)) return false;   // before sending the access code
        std::string vh = mqttStr("MQTT") + (char)4 + (char)0xC2 + (char)0 + (char)60;   // level 4; user+pass+clean; keepalive 60
        std::string pl = mqttStr("swamp-" + std::to_string(msNow() % 1000000)) + mqttStr("bblp") + mqttStr(p.accessCode);
        if (!t_.write(std::string(1, (char)0x10) + mqttLen(vh.size() + pl.size()) + vh + pl)) { err = "MQTT: connect failed"; return false; }
        int type; std::string body;
        if (!readPacket(type, body) || type != 2 || body.size() < 2) { err = "MQTT: no reply from the printer"; return false; }
        if (body[1] != 0) { err = body[1] == 4 || body[1] == 5 ? "MQTT: the printer rejected the access code" : "MQTT: refused (" + std::to_string((int)body[1]) + ")"; return false; }
        std::string topic = "device/" + p.serial + "/report";
        std::string sub = std::string{0, 1} + mqttStr(topic) + (char)0;   // packet id 1, qos 0
        if (!t_.write(std::string(1, (char)0x82) + mqttLen(sub.size()) + sub)) { err = "MQTT: subscribe failed"; return false; }
        if (!readPacket(type, body) || type != 9) { err = "MQTT: subscribe not acknowledged (wrong serial number?)"; return false; }
        return true;
    }
    bool publish(const json& msg, std::string& err) { return publishTo("device/" + p_.serial + "/request", msg, err); }
    bool publishTo(const std::string& topic, const json& msg, std::string& err) {
        std::string payload = msg.dump();
        std::string body = mqttStr(topic) + payload;   // qos 0: no packet id
        if (!t_.write(std::string(1, (char)0x30) + mqttLen(body.size()) + body)) { err = "MQTT: publish failed"; return false; }
        return true;
    }
    // Wait for a report; `want` decides whether it's the one we're waiting for.
    bool waitReport(int timeoutMs, std::function<bool(const json&)> want, json& out, std::string& err) {
        long long until = msNow() + timeoutMs;
        while (msNow() < until) {
            int type; std::string body;
            if (!readPacket(type, body)) { err = "MQTT: connection lost"; return false; }
            if (type != 3 || body.size() < 2) continue;
            size_t tl = ((unsigned char)body[0] << 8) | (unsigned char)body[1];
            if (body.size() < 2 + tl) continue;
            json j = json::parse(body.substr(2 + tl), nullptr, false);
            if (j.is_object() && want(j)) { out = j; return true; }
        }
        err = "MQTT: no matching report from the printer";
        return false;
    }
    void close() { std::string d{(char)0xE0, 0}; t_.write(d); t_.shut(); }
    const std::string& pin() const { return t_.pin(); }
private:
    bool readPacket(int& type, std::string& body) {
        char h;
        if (t_.read(&h, 1) != 1) return false;
        type = ((unsigned char)h) >> 4;
        size_t len = 0, mult = 1;
        for (int i = 0; i < 4; i++) {
            char b; if (t_.read(&b, 1) != 1) return false;
            len += ((unsigned char)b & 127) * mult; mult *= 128;
            if (!((unsigned char)b & 128)) break;
        }
        if (len > 4 * 1024 * 1024) return false;
        return t_.readExact(body, len);
    }
    Tls t_;
    Printer p_;
};

// The printer's state: connect, ask for everything (pushall), return the "print" object.
inline bool status(const Printer& p, json& out, std::string& err, std::string* seenPin = nullptr) {
    Mqtt m;
    if (!m.connect(p, err)) return false;
    if (seenPin) *seenPin = m.pin();
    m.publish(json{{"pushing", {{"sequence_id", "0"}, {"command", "pushall"}, {"version", 1}, {"push_target", 1}}}}, err);
    json rep;
    bool ok = m.waitReport(10000, [](const json& j) { return j.contains("print") && j["print"].contains("gcode_state"); }, rep, err);
    m.close();
    if (ok) out = rep["print"];
    return ok;
}

// Start printing a sliced project already on the SD card. Plate 1, no AMS by default (an A1
// without AMS lite prints from the external spool).
inline bool startPrint(const Printer& p, const std::string& remoteName, const std::string& title, bool useAms, std::string& err) {
    Mqtt m;
    if (!m.connect(p, err)) return false;
    // a sequence id of our own, so only the printer's answer to THIS command counts (review 2026-10-07:
    // a printer already busy reports RUNNING, which must not read as "accepted")
    std::string seq = std::to_string(100000 + msNow() % 900000000);
    json cmd{{"print", {{"sequence_id", seq}, {"command", "project_file"}, {"param", "Metadata/plate_1.gcode"},
                        {"project_id", "0"}, {"profile_id", "0"}, {"task_id", "0"}, {"subtask_id", "0"},
                        {"subtask_name", title}, {"file", remoteName}, {"url", "file:///sdcard/" + remoteName}, {"md5", ""},
                        {"timelapse", false}, {"bed_type", "auto"}, {"bed_levelling", true}, {"flow_cali", false},
                        {"vibration_cali", false}, {"layer_inspect", false}, {"use_ams", useAms},
                        {"ams_mapping", useAms ? json::array({0}) : json::array()}}}};
    if (!m.publish(cmd, err)) { m.close(); return false; }
    // the printer echoes the command with a result, or moves to PREPARE/RUNNING
    json rep;
    bool ok = m.waitReport(15000, [&](const json& j) {
        if (!j.contains("print") || !j["print"].is_object()) return false;
        const json& pr = j["print"];
        auto s = [&](const char* k) { return pr.contains(k) && pr[k].is_string() ? pr[k].get<std::string>() : std::string(); };
        if (s("command") == "project_file" && s("sequence_id") == seq && pr.contains("result")) return true;
        std::string st = s("gcode_state");
        return (st == "PREPARE" || st == "RUNNING" || st == "SLICING") && s("subtask_name") == title;   // our job, not someone else's
    }, rep, err);
    m.close();
    if (!ok) { err = "The printer didn't confirm the print (" + err + "). Is Developer Mode on?"; return false; }
    const json& pr = rep["print"];
    std::string res = pr.contains("result") && pr["result"].is_string() ? pr["result"].get<std::string>() : "";
    if (pr.contains("command") && pr["command"] == "project_file" && res != "success" && res != "SUCCESS") {
        err = "The printer refused the print: " + res + (pr.contains("reason") ? " (" + pr["reason"].dump() + ")" : "");
        return false;
    }
    return true;
}

// ---- discovery -------------------------------------------------------------------------------
// Bambu printers announce themselves with SSDP NOTIFYs on UDP 2021 every few seconds.
inline std::vector<Printer> discover(int listenMs, int port = 2021) {
    std::vector<Printer> found;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return found;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (sockaddr*)&a, sizeof a) != 0) { close(fd); return found; }
    long long until = msNow() + listenMs;
    char buf[2048];
    while (msNow() < until) {
        pollfd pf{fd, POLLIN, 0};
        if (poll(&pf, 1, (int)std::max<long long>(1, until - msNow())) <= 0) continue;
        sockaddr_in from{}; socklen_t fl = sizeof from;
        int n = recvfrom(fd, buf, sizeof buf - 1, 0, (sockaddr*)&from, &fl);
        if (n <= 0) continue;
        buf[n] = 0;
        std::map<std::string, std::string> h;
        std::string s(buf), line;
        for (size_t i = 0, j; i < s.size(); i = j + 1) {
            j = s.find('\n', i); if (j == std::string::npos) j = s.size();
            line = s.substr(i, j - i);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t c = line.find(':');
            if (c == std::string::npos) continue;
            std::string k = line.substr(0, c), v = line.substr(c + 1);
            for (auto& ch : k) ch = (char)std::tolower((unsigned char)ch);
            while (!v.empty() && v[0] == ' ') v.erase(0, 1);
            h[k] = v;
        }
        if (h["usn"].empty()) continue;
        Printer p;
        p.serial = h["usn"];
        p.ip = h["location"].empty() ? inet_ntoa(from.sin_addr) : h["location"];
        p.model = h["devmodel.bambu.com"];
        p.name = h["devname.bambu.com"];
        bool dup = false;
        for (const auto& q : found) dup |= q.serial == p.serial;
        if (!dup) found.push_back(p);
    }
    close(fd);
    return found;
}

} // namespace bambu
} // namespace swamp

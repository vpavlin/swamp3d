// Geometry out of a 3MF, nothing else: each printable build item becomes one binary STL, with the
// component and item transforms applied. Settings, plates, custom G-code and thumbnails that a
// publisher put in the project are ignored, so none of it can reach the printer (ADR 0017).
//
// Done here rather than with the slicer's CLI because OrcaSlicer 2.4.2 segfaults on any 3MF passed
// on its command line (--export-stl, --info; found in the first real-A1 test, 2026-10-08).
//
// A 3MF is a zip (stored or deflate entries; zlib) holding XML models (Qt's QXmlStreamReader).
// Bambu Studio / OrcaSlicer projects keep the meshes in 3D/Objects/*.model and reference them from
// 3D/3dmodel.model through <component p:path=...>.
#pragma once
#include <QByteArray>
#include <QString>
#include <QXmlStreamReader>
#include <zlib.h>
#include <array>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <functional>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace swamp3mf {

using Mat = std::array<double, 12>;   // 3MF order: m00 m01 m02 m10 m11 m12 m20 m21 m22 m30 m31 m32
inline Mat identity() { return {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}; }
// apply a then b (3MF row-vector convention: p' = p * A * B)
inline Mat compose(const Mat& a, const Mat& b) {
    Mat r{};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    for (int j = 0; j < 3; j++) r[9 + j] = a[9] * b[j] + a[10] * b[3 + j] + a[11] * b[6 + j] + b[9 + j];
    return r;
}
inline bool parseMat(const QString& s, Mat& m) {
    if (s.trimmed().isEmpty()) { m = identity(); return true; }
    const QStringList parts = s.simplified().split(' ');
    if (parts.size() != 12) return false;
    for (int i = 0; i < 12; i++) { bool ok = false; m[i] = parts[i].toDouble(&ok); if (!ok) return false; }   // QString::toDouble is locale-independent
    return true;
}

// ---- zip ------------------------------------------------------------------------------------------
static constexpr size_t kMaxEntry = 512u << 20;   // an uncompressed model part over 512 MB is refused

inline uint32_t rd32(const unsigned char* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
inline uint16_t rd16(const unsigned char* p) { return uint16_t(p[0] | p[1] << 8); }

// name -> bytes of every entry under 3D/ ending in .model
inline bool readModels(const std::string& path, std::map<std::string, std::string>& out, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "can't open the file"; return false; }
    std::string z((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const auto* d = reinterpret_cast<const unsigned char*>(z.data());
    size_t n = z.size(), eocd = std::string::npos;
    for (size_t i = n >= 22 ? n - 22 : 0; i + 22 <= n && n - i <= 22 + 65535; i--) {
        if (rd32(d + i) == 0x06054b50) { eocd = i; break; }
        if (i == 0) break;
    }
    if (eocd == std::string::npos) { err = "not a zip archive"; return false; }
    size_t count = rd16(d + eocd + 10), cd = rd32(d + eocd + 16);
    for (size_t k = 0, p = cd; k < count; k++) {
        if (p + 46 > n || rd32(d + p) != 0x02014b50) { err = "damaged zip directory"; return false; }
        uint16_t method = rd16(d + p + 10), nl = rd16(d + p + 28), xl = rd16(d + p + 30), cl = rd16(d + p + 32);
        size_t csize = rd32(d + p + 20), usize = rd32(d + p + 24), lho = rd32(d + p + 42);
        if (p + 46 + nl > n) { err = "damaged zip directory"; return false; }
        std::string name(z, p + 46, nl);
        p += 46 + nl + xl + cl;
        std::string lower = name;
        for (auto& c : lower) c = char(tolower(static_cast<unsigned char>(c)));
        if (lower.rfind("3d/", 0) != 0 || lower.size() < 6 || lower.compare(lower.size() - 6, 6, ".model") != 0) continue;
        if (usize > kMaxEntry || csize == 0xffffffff) { err = name + " is too large"; return false; }
        if (lho + 30 > n || rd32(d + lho) != 0x04034b50) { err = "damaged zip entry " + name; return false; }
        size_t data = lho + 30 + rd16(d + lho + 26) + rd16(d + lho + 28);
        if (data + csize > n) { err = "truncated zip entry " + name; return false; }
        std::string bytes;
        if (method == 0) bytes.assign(z, data, csize);
        else if (method == 8) {
            bytes.resize(usize);
            z_stream s{};
            if (inflateInit2(&s, -MAX_WBITS) != Z_OK) { err = "zlib"; return false; }
            s.next_in = const_cast<Bytef*>(d + data); s.avail_in = uInt(csize);
            s.next_out = reinterpret_cast<Bytef*>(bytes.data()); s.avail_out = uInt(usize);
            int rc = inflate(&s, Z_FINISH);
            size_t got = s.total_out;
            inflateEnd(&s);
            if (rc != Z_STREAM_END || got != usize) { err = "can't unpack " + name; return false; }
        } else { err = name + " uses an unsupported zip compression"; return false; }
        out["/" + name] = std::move(bytes);
    }
    if (out.empty()) { err = "no 3D model inside"; return false; }
    return true;
}

// ---- models ---------------------------------------------------------------------------------------
struct Component { std::string path; int id = 0; Mat m = identity(); };
struct Object { std::vector<std::array<double, 3>> v; std::vector<std::array<uint32_t, 3>> t; std::vector<Component> parts; };
struct Item { int id = 0; Mat m = identity(); bool printable = true; };
using Objects = std::map<std::pair<std::string, int>, Object>;   // (model file, object id)

inline bool parseModel(const std::string& file, const std::string& xml, Objects& objs, std::vector<Item>* build, std::string& err) {
    QXmlStreamReader r(QByteArray::fromRawData(xml.data(), int(xml.size())));
    Object* cur = nullptr;
    double mm = 1;   // the file's unit in millimetres (<model unit="...">; the default is millimetre)
    while (!r.atEnd()) {
        if (r.readNext() != QXmlStreamReader::StartElement) continue;
        const auto n = r.name();
        const auto a = r.attributes();
        if (n == u"model") {
            const QString u = a.value("unit").toString();
            if (u.isEmpty() || u == "millimeter") mm = 1;
            else if (u == "micron") mm = 0.001;
            else if (u == "centimeter") mm = 10;
            else if (u == "inch") mm = 25.4;
            else if (u == "foot") mm = 304.8;
            else if (u == "meter") mm = 1000;
            else { err = "an unknown unit \"" + u.toStdString() + "\" in " + file; return false; }
        }
        else if (n == u"object") { cur = &objs[{file, a.value("id").toInt()}]; }
        else if (n == u"vertex" && cur) {
            bool ox, oy, oz;
            cur->v.push_back({a.value("x").toDouble(&ox) * mm, a.value("y").toDouble(&oy) * mm, a.value("z").toDouble(&oz) * mm});
            if (!ox || !oy || !oz) { err = "a bad vertex in " + file; return false; }
        }
        else if (n == u"triangle" && cur) {
            bool o1, o2, o3;
            cur->t.push_back({a.value("v1").toUInt(&o1), a.value("v2").toUInt(&o2), a.value("v3").toUInt(&o3)});
            if (!o1 || !o2 || !o3) { err = "a bad triangle in " + file; return false; }
        }
        else if (n == u"component" && cur) {
            Component c;
            c.id = a.value("objectid").toInt();
            QString p;
            for (const auto& at : a) if (at.name() == u"path") p = at.value().toString();   // p:path, whatever the prefix
            c.path = p.isEmpty() ? file : p.toStdString();
            if (!parseMat(a.value("transform").toString(), c.m)) { err = "a bad component transform in " + file; return false; }
            for (int k = 9; k < 12; k++) c.m[k] *= mm;   // translations are in the file's unit too
            cur->parts.push_back(c);
        }
        else if (n == u"item" && build) {
            Item it;
            it.id = a.value("objectid").toInt();
            it.printable = a.value("printable") != u"0";
            if (!parseMat(a.value("transform").toString(), it.m)) { err = "a bad build transform in " + file; return false; }
            for (int k = 9; k < 12; k++) it.m[k] *= mm;
            build->push_back(it);
        }
    }
    if (r.hasError()) { err = file + ": " + r.errorString().toStdString(); return false; }
    return true;
}

// Every printable build item as one binary STL in outDir (item-1.stl, ...). Returns the paths.
inline bool toStls(const std::string& path, const std::string& outDir, std::vector<std::string>& stls, std::string& err) {
    std::map<std::string, std::string> files;
    if (!readModels(path, files, err)) return false;
    std::string root = "/3D/3dmodel.model";
    if (!files.count(root)) {   // case-insensitive fallback
        for (const auto& [k, v] : files) { std::string l = k; for (auto& c : l) c = char(tolower(static_cast<unsigned char>(c))); if (l == "/3d/3dmodel.model") root = k; }
        if (!files.count(root)) { err = "no 3D/3dmodel.model inside"; return false; }
    }
    Objects objs;
    std::vector<Item> build;
    for (const auto& [name, xml] : files) if (!parseModel(name, xml, objs, name == root ? &build : nullptr, err)) return false;
    if (build.empty()) { err = "the project has nothing on its build plate"; return false; }
    int index = 0;
    for (const auto& item : build) {
        if (!item.printable) continue;
        std::vector<float> tris;   // 9 floats per triangle
        size_t budget = 20'000'000;   // triangles; also stops a component cycle
        std::function<bool(const std::string&, int, const Mat&, int)> addShape = [&](const std::string& file, int id, const Mat& m, int depth) {
            auto it = objs.find({file, id});
            if (it == objs.end() || depth > 16) { err = "a missing or looping object in the project"; return false; }
            const Object& o = it->second;
            for (const auto& tr : o.t) {
                if (!budget--) { err = "the model is too large"; return false; }
                for (uint32_t vi : tr) {
                    if (vi >= o.v.size()) { err = "a triangle points past the vertex list"; return false; }
                    const auto& p = o.v[vi];
                    for (int j = 0; j < 3; j++) tris.push_back(float(p[0] * m[j] + p[1] * m[3 + j] + p[2] * m[6 + j] + m[9 + j]));
                }
            }
            for (const auto& c : o.parts) if (!addShape(c.path, c.id, compose(c.m, m), depth + 1)) return false;
            return true;
        };
        if (!addShape(root, item.id, item.m, 0)) return false;
        if (tris.empty()) continue;
        std::string out = outDir + "/item-" + std::to_string(++index) + ".stl";
        std::ofstream f(out, std::ios::binary);
        char header[80] = "swamp: geometry from a 3MF";
        f.write(header, 80);
        uint32_t count = uint32_t(tris.size() / 9);
        f.write(reinterpret_cast<const char*>(&count), 4);
        for (size_t i = 0; i < tris.size(); i += 9) {
            const float* t = &tris[i];
            float ux = t[3] - t[0], uy = t[4] - t[1], uz = t[5] - t[2], vx = t[6] - t[0], vy = t[7] - t[1], vz = t[8] - t[2];
            float nrm[3] = {uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx};
            float len = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
            if (len > 0) for (float& c : nrm) c /= len;
            f.write(reinterpret_cast<const char*>(nrm), 12);
            f.write(reinterpret_cast<const char*>(t), 36);
            uint16_t attr = 0;
            f.write(reinterpret_cast<const char*>(&attr), 2);
        }
        if (!f) { err = "can't write " + out; return false; }
        stls.push_back(out);
    }
    if (stls.empty()) { err = "the project has no printable shapes"; return false; }
    return true;
}

}  // namespace swamp3mf

#pragma once
// swamp_thumb.hpp - render a mesh to a small PNG thumbnail at publish time (docs/adr/0008:
// thumbnails before a 3D viewer). Flat-shaded, z-buffered, orthographic three-quarter view,
// transparent background. Dependency-free: a tiny rasterizer + a stored-deflate PNG writer.
#include <vector>
#include <string>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace swamp {
namespace thumb {

inline uint32_t crc32(const uint8_t* d, size_t n, uint32_t c = 0xffffffffu) {
    static uint32_t T[256];
    static bool init = false;
    if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t k = i; for (int j = 0; j < 8; j++) k = k & 1 ? 0xedb88320u ^ (k >> 1) : k >> 1; T[i] = k; } init = true; }
    for (size_t i = 0; i < n; i++) c = T[(c ^ d[i]) & 255] ^ (c >> 8);
    return c;
}

inline std::string png(const std::vector<uint8_t>& rgba, int w, int h) {
    auto be32 = [](std::string& s, uint32_t v) { s += (char)(v >> 24); s += (char)(v >> 16); s += (char)(v >> 8); s += (char)v; };
    auto chunk = [&](std::string& out, const char* type, const std::string& data) {
        be32(out, (uint32_t)data.size());
        std::string td = std::string(type, 4) + data;
        out += td;
        be32(out, crc32((const uint8_t*)td.data(), td.size()) ^ 0xffffffffu);
    };
    std::string raw;
    for (int y = 0; y < h; y++) { raw += '\0'; raw.append((const char*)&rgba[(size_t)y * w * 4], (size_t)w * 4); }
    // zlib stream with stored (uncompressed) deflate blocks
    std::string z = "\x78\x01";
    uint32_t a = 1, b = 0;
    for (unsigned char c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw.size() || off == 0; off += 65535) {
        size_t len = std::min<size_t>(65535, raw.size() - off);
        bool last = off + len >= raw.size();
        z += (char)(last ? 1 : 0);
        z += (char)(len & 255); z += (char)(len >> 8);
        z += (char)(~len & 255); z += (char)((~len >> 8) & 255);
        z.append(raw, off, len);
        if (last) break;
    }
    be32(z, (b << 16) | a);
    std::string out = "\x89PNG\r\n\x1a\n";
    std::string ihdr;
    be32(ihdr, w); be32(ihdr, h);
    ihdr += (char)8; ihdr += (char)6; ihdr += '\0'; ihdr += '\0'; ihdr += '\0';
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", "");
    return out;
}

/** Render a triangle soup (9 doubles per triangle) to an RGBA PNG of size x size. */
inline std::string render(const std::vector<double>& rawTris, int size = 256) {
    // drop non-finite / absurd coordinates (a crafted STL must not crash the renderer)
    std::vector<double> tris;
    for (size_t i = 0; i + 9 <= rawTris.size(); i += 9) {
        bool ok = true;
        for (int k = 0; k < 9; k++) if (!std::isfinite(rawTris[i + k]) || std::fabs(rawTris[i + k]) > 1e7) { ok = false; break; }
        if (ok) tris.insert(tris.end(), rawTris.begin() + i, rawTris.begin() + i + 9);
    }
    const size_t n = tris.size() / 9;
    // view: rotate 35 deg about Z, then tilt 30 deg about X (three-quarter view, Z up)
    const double az = 35 * M_PI / 180, el = 60 * M_PI / 180;
    auto view = [&](const double* p, double out[3]) {
        double x = p[0] * std::cos(az) - p[1] * std::sin(az), y = p[0] * std::sin(az) + p[1] * std::cos(az), z = p[2];
        out[0] = x;
        out[1] = z * std::sin(el) - y * std::cos(el);   // screen up: +Z, tilted toward the viewer
        out[2] = y * std::sin(el) + z * std::cos(el);   // depth: larger = nearer the viewer
    };
    std::vector<double> v(n * 9);
    double mn[2] = {1e300, 1e300}, mx[2] = {-1e300, -1e300};
    for (size_t i = 0; i < n * 3; i++) {
        view(&tris[i * 3], &v[i * 3]);
        for (int k = 0; k < 2; k++) { mn[k] = std::min(mn[k], v[i * 3 + k]); mx[k] = std::max(mx[k], v[i * 3 + k]); }
    }
    double span = std::max(mx[0] - mn[0], mx[1] - mn[1]);
    if (!(span > 0)) span = 1;
    const double pad = size * 0.06, scale = (size - 2 * pad) / span;
    const double ox = pad + ((size - 2 * pad) - (mx[0] - mn[0]) * scale) / 2, oy = pad + ((size - 2 * pad) - (mx[1] - mn[1]) * scale) / 2;
    std::vector<double> zb((size_t)size * size, -1e300);
    std::vector<uint8_t> img((size_t)size * size * 4, 0);
    const double L[3] = {-0.35, 0.55, 0.76};   // light from upper left, toward the viewer
    for (size_t t = 0; t < n; t++) {
        double* a = &v[t * 9]; double* b = a + 3; double* c = a + 6;
        double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], wx = c[0] - a[0], wy = c[1] - a[1], wz = c[2] - a[2];
        double nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
        double nl = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (nl == 0) continue;
        nx /= nl; ny /= nl; nz /= nl;
        double shade = std::fabs(nx * L[0] + ny * L[1] + nz * L[2]);   // two-sided (winding may be inconsistent)
        uint8_t r = (uint8_t)std::min(255.0, 70 + 170 * shade), g = (uint8_t)std::min(255.0, 95 + 140 * shade), bl = (uint8_t)std::min(255.0, 60 + 110 * shade);
        double P[3][3];
        for (int k = 0; k < 3; k++) { double* s = a + k * 3; P[k][0] = ox + (s[0] - mn[0]) * scale; P[k][1] = size - (oy + (s[1] - mn[1]) * scale); P[k][2] = s[2]; }
        auto cl = [size](double x) { return x < 0 ? 0 : x > size - 1 ? size - 1 : (int)x; };
        int x0 = cl(std::floor(std::min({P[0][0], P[1][0], P[2][0]}))), x1 = cl(std::ceil(std::max({P[0][0], P[1][0], P[2][0]})));
        int y0 = cl(std::floor(std::min({P[0][1], P[1][1], P[2][1]}))), y1 = cl(std::ceil(std::max({P[0][1], P[1][1], P[2][1]})));
        double den = (P[1][1] - P[2][1]) * (P[0][0] - P[2][0]) + (P[2][0] - P[1][0]) * (P[0][1] - P[2][1]);
        if (std::fabs(den) < 1e-12) continue;
        for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
            double px = x + 0.5, py = y + 0.5;
            double l0 = ((P[1][1] - P[2][1]) * (px - P[2][0]) + (P[2][0] - P[1][0]) * (py - P[2][1])) / den;
            double l1 = ((P[2][1] - P[0][1]) * (px - P[2][0]) + (P[0][0] - P[2][0]) * (py - P[2][1])) / den;
            double l2 = 1 - l0 - l1;
            if (l0 < 0 || l1 < 0 || l2 < 0) continue;
            double z = l0 * P[0][2] + l1 * P[1][2] + l2 * P[2][2];
            size_t i = (size_t)y * size + x;
            if (z <= zb[i]) continue;
            zb[i] = z;
            img[i * 4] = r; img[i * 4 + 1] = g; img[i * 4 + 2] = bl; img[i * 4 + 3] = 255;
        }
    }
    return png(img, size, size);
}

} // namespace thumb
} // namespace swamp

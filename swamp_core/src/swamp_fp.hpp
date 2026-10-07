#pragma once
// swamp_fp.hpp - shape fingerprint fp/v1 in C++: a step-for-step port of packages/fp/src/fp.mjs
// (F1 = D2 + A3 histograms, F3 = local multi-rank point-pair histogram). Same integer PRNG
// (mulberry32), same canonical triangle order (stable sort), same bins. Parity with the JS
// reference is checked by test/fp_parity_test.cpp (histograms equal within float noise).
// docs/adr/0003, docs/BENCHMARK.md.
#include <vector>
#include <charconv>
#include <string>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <array>

namespace swamp {
namespace fp {

constexpr int VERSION = 1;
constexpr int SAMPLES = 4096;
constexpr int D2_PAIRS = 20000, D2_BINS = 64;
constexpr double D2_MAX = 3.0;
constexpr int A3_TRIPLES = 20000, A3_BINS = 32;
constexpr int RANKS[4] = {2, 6, 16, 40};
constexpr int RATIO_BINS = 4, ANG_BINS = 6;
constexpr int F3_SIZE = 4 * RATIO_BINS * ANG_BINS * ANG_BINS * ANG_BINS;

struct Rng {
    uint32_t a;
    explicit Rng(uint32_t seed) : a(seed) {}
    uint32_t u32() {
        a += 0x6d2b79f5u;
        uint32_t t = a;
        t = (t ^ (t >> 15)) * (t | 1u);
        t ^= t + (t ^ (t >> 7)) * (t | 61u);
        return t ^ (t >> 14);
    }
    double f64() { return u32() / 4294967296.0; }
};

/** Parse binary or ASCII STL into a triangle soup (9 doubles per triangle). */
inline std::vector<double> parseStl(const std::string& buf) {
    std::vector<double> out;
    if (buf.size() >= 84) {
        uint32_t n; std::memcpy(&n, buf.data() + 80, 4);
        if (84ull + 50ull * n == buf.size()) {
            out.resize((size_t)n * 9);
            for (uint32_t i = 0; i < n; i++)
                for (int k = 0; k < 9; k++) { float f; std::memcpy(&f, buf.data() + 84 + (size_t)i * 50 + 12 + k * 4, 4); out[(size_t)i * 9 + k] = f; }
            return out;
        }
    }
    size_t p = 0;
    while ((p = buf.find("vertex", p)) != std::string::npos) {
        p += 6;
        for (int k = 0; k < 3; k++) {
            while (p < buf.size() && std::isspace((unsigned char)buf[p])) p++;
            // locale-independent: strtod follows the user's locale in a Qt program, so on a
            // decimal-comma system "6.614115e+00" read as 6 (review 2026-10-07)
            const char* b = buf.data() + p;
            const char* e = buf.data() + buf.size();
            if (b < e && *b == '+') b++;
            double v = 0;
            auto r = std::from_chars(b, e, v);
            if (r.ec != std::errc() || r.ptr == b) break;
            out.push_back(v);
            p = r.ptr - buf.data();
        }
    }
    out.resize(out.size() - out.size() % 9);
    return out;
}

struct Samples { std::vector<double> pts, nrm; int count = 0; double area = 0; };

inline double dist3(const double* a, const double* b) {
    double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

/** Drop triangles with non-finite or absurd coordinates (a crafted STL must not crash us). */
inline std::vector<double> sanitize(const std::vector<double>& in) {
    std::vector<double> out;
    out.reserve(in.size());
    for (size_t i = 0; i + 9 <= in.size(); i += 9) {
        bool ok = true;
        for (int k = 0; k < 9; k++) if (!std::isfinite(in[i + k]) || std::fabs(in[i + k]) > 1e7) { ok = false; break; }
        if (ok) out.insert(out.end(), in.begin() + i, in.begin() + i + 9);
    }
    return out;
}
inline int clampBin(double x, int bins) {
    if (!std::isfinite(x)) return -1;
    if (x < 0) return 0;
    if (x >= bins) return bins - 1;
    return (int)x;
}

inline Samples samplePoints(const std::vector<double>& tris, int count = SAMPLES, uint32_t seed = 0x5a3b) {
    const size_t n = tris.size() / 9;
    if (!n) throw std::runtime_error("mesh has no triangles");
    std::vector<double> area(n);
    double total = 0;
    for (size_t i = 0; i < n; i++) {
        const double* o = &tris[i * 9];
        double ux = o[3] - o[0], uy = o[4] - o[1], uz = o[5] - o[2], vx = o[6] - o[0], vy = o[7] - o[1], vz = o[8] - o[2];
        double cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
        area[i] = 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
        total += area[i];
    }
    if (!(total > 0)) throw std::runtime_error("mesh has no area");
    const double s = std::sqrt(total);
    std::vector<double> canon(n * 9);
    struct Key { long long r0, r1, r2; size_t i; };
    std::vector<Key> keys(n);
    for (size_t i = 0; i < n; i++) {
        const double* P[3] = {&tris[i * 9], &tris[i * 9 + 3], &tris[i * 9 + 6]};
        double L[3] = {dist3(P[1], P[2]), dist3(P[0], P[2]), dist3(P[0], P[1])};
        int idx[3] = {0, 1, 2};
        std::sort(idx, idx + 3, [&](int a, int b) { return L[a] < L[b] || (L[a] == L[b] && a < b); });
        for (int k = 0; k < 3; k++) for (int c = 0; c < 3; c++) canon[i * 9 + k * 3 + c] = P[idx[k]][c];
        auto rnd = [&](int j) { return (long long)std::floor((L[j] / s) * 1e6 + 0.5); };   // JS Math.round
        keys[i] = {rnd(idx[0]), rnd(idx[1]), rnd(idx[2]), i};
    }
    std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
        if (a.r0 != b.r0) return a.r0 < b.r0;
        if (a.r1 != b.r1) return a.r1 < b.r1;
        return a.r2 < b.r2;
    });
    std::vector<double> cum(n);
    double acc = 0;
    for (size_t j = 0; j < n; j++) { acc += area[keys[j].i]; cum[j] = acc; }
    Rng R(seed);
    Samples S;
    S.count = count; S.area = total;
    S.pts.resize((size_t)count * 3); S.nrm.resize((size_t)count * 3);
    for (int q = 0; q < count; q++) {
        double t = R.f64() * total;
        size_t lo = 0, hi = n - 1;
        while (lo < hi) { size_t mid = (lo + hi) >> 1; if (cum[mid] < t) lo = mid + 1; else hi = mid; }
        const double* o = &canon[keys[lo].i * 9];
        double r1 = R.f64(), r2 = R.f64();
        double sq = std::sqrt(r1), a = 1 - sq, b = sq * (1 - r2), c = sq * r2;
        for (int k = 0; k < 3; k++) S.pts[q * 3 + k] = a * o[k] + b * o[3 + k] + c * o[6 + k];
        double ux = o[3] - o[0], uy = o[4] - o[1], uz = o[5] - o[2], vx = o[6] - o[0], vy = o[7] - o[1], vz = o[8] - o[2];
        double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        double nl = std::sqrt(nx * nx + ny * ny + nz * nz); if (nl == 0) nl = 1;
        S.nrm[q * 3] = nx / nl; S.nrm[q * 3 + 1] = ny / nl; S.nrm[q * 3 + 2] = nz / nl;
    }
    return S;
}

inline double hyp(double x, double y, double z) { return std::sqrt(x * x + y * y + z * z); }

inline double angleAt(const std::vector<double>& p, int m, int a, int c) {
    double ux = p[a * 3] - p[m * 3], uy = p[a * 3 + 1] - p[m * 3 + 1], uz = p[a * 3 + 2] - p[m * 3 + 2];
    double vx = p[c * 3] - p[m * 3], vy = p[c * 3 + 1] - p[m * 3 + 1], vz = p[c * 3 + 2] - p[m * 3 + 2];
    double lu = hyp(ux, uy, uz), lv = hyp(vx, vy, vz);
    if (lu == 0 || lv == 0) return 0;
    double c0 = (ux * vx + uy * vy + uz * vz) / (lu * lv);
    return std::acos(std::max(-1.0, std::min(1.0, c0)));
}

struct Fingerprint {
    std::array<double, D2_BINS> d2{};
    std::array<double, A3_BINS> a3{};
    std::vector<int> f3;   // uint16-scaled bins
};

inline void f1(const Samples& S, Fingerprint& F) {
    Rng R(0xd2d2);
    const int n = S.count;
    const auto& p = S.pts;
    std::vector<double> d(D2_PAIRS);
    double mean = 0;
    for (int i = 0; i < D2_PAIRS; i++) {
        int a = R.u32() % n, b = R.u32() % n;
        d[i] = hyp(p[a * 3] - p[b * 3], p[a * 3 + 1] - p[b * 3 + 1], p[a * 3 + 2] - p[b * 3 + 2]);
        mean += d[i];
    }
    mean /= D2_PAIRS;
    for (int i = 0; i < D2_PAIRS; i++) {
        int bin = clampBin(std::floor((d[i] / (mean ? mean : 1) / D2_MAX) * D2_BINS), D2_BINS);
        if (bin >= 0) F.d2[bin] += 1.0 / D2_PAIRS;
    }
    for (int i = 0; i < A3_TRIPLES; i++) {
        int a = R.u32() % n, b = R.u32() % n, c = R.u32() % n;
        double ang = angleAt(p, b, a, c);
        int bin = clampBin(std::floor((ang / M_PI) * A3_BINS), A3_BINS);
        if (bin >= 0) F.a3[bin] += 1.0 / A3_TRIPLES;
    }
}

inline void f3(const Samples& S, Fingerprint& F) {
    const int n = S.count, K = 40;
    const auto& p = S.pts; const auto& q = S.nrm;
    std::vector<int> nb((size_t)n * K);
    std::vector<double> nd((size_t)n * K), d(n);
    for (int i = 0; i < n; i++) {
        const double x = p[i * 3], y = p[i * 3 + 1], z = p[i * 3 + 2];
        for (int j = 0; j < n; j++) { double a = p[j * 3] - x, b = p[j * 3 + 1] - y, c = p[j * 3 + 2] - z; d[j] = a * a + b * b + c * c; }
        d[i] = INFINITY;
        std::vector<int> best; best.reserve(K);
        auto less = [&](int u, int v) { return d[u] < d[v] || (d[u] == d[v] && u < v); };
        for (int j = 0; j < n; j++) {
            if ((int)best.size() < K) { best.push_back(j); if ((int)best.size() == K) std::sort(best.begin(), best.end(), less); continue; }
            int last = best[K - 1];
            if (d[j] < d[last] || (d[j] == d[last] && j < last)) {
                int k = K - 1;
                while (k > 0 && (d[best[k - 1]] > d[j] || (d[best[k - 1]] == d[j] && best[k - 1] > j))) { best[k] = best[k - 1]; k--; }
                best[k] = j;
            }
        }
        if ((int)best.size() < K) std::sort(best.begin(), best.end(), less);
        for (int k = 0; k < K; k++) {
            int j = k < (int)best.size() ? best[k] : i;
            nb[(size_t)i * K + k] = j;
            nd[(size_t)i * K + k] = k < (int)best.size() ? std::sqrt(d[j]) : 0;
        }
    }
    std::vector<double> hist(F3_SIZE, 0.0);
    for (int i = 0; i < n; i++) {
        double r = nd[(size_t)i * K + 3]; if (r == 0) r = 1e-12;
        for (int ri = 0; ri < 4; ri++) {
            int k = RANKS[ri] - 1;
            int j = nb[(size_t)i * K + k]; double dij = nd[(size_t)i * K + k];
            if (j == i || dij == 0) continue;
            double dx = (p[j * 3] - p[i * 3]) / dij, dy = (p[j * 3 + 1] - p[i * 3 + 1]) / dij, dz = (p[j * 3 + 2] - p[i * 3 + 2]) / dij;
            double a1 = std::fabs(q[i * 3] * dx + q[i * 3 + 1] * dy + q[i * 3 + 2] * dz);
            double a2 = std::fabs(q[j * 3] * dx + q[j * 3 + 1] * dy + q[j * 3 + 2] * dz);
            double a3 = std::fabs(q[i * 3] * q[j * 3] + q[i * 3 + 1] * q[j * 3 + 1] + q[i * 3 + 2] * q[j * 3 + 2]);
            double lr = std::log2(dij / r / std::sqrt(RANKS[ri] / 4.0));
            int rb = clampBin(std::floor((lr + 1) * RATIO_BINS / 2), RATIO_BINS);
            auto qa = [](double x) { return clampBin(std::floor(x * ANG_BINS), ANG_BINS); };
            if (rb < 0 || qa(a1) < 0 || qa(a2) < 0 || qa(a3) < 0) continue;
            int tok = (((ri * RATIO_BINS + rb) * ANG_BINS + qa(a1)) * ANG_BINS + qa(a2)) * ANG_BINS + qa(a3);
            hist[tok] += 1;
        }
    }
    double tot = 0;
    for (double v : hist) tot += v;
    F.f3.resize(F3_SIZE);
    for (int t = 0; t < F3_SIZE; t++) F.f3[t] = (int)std::floor((tot ? hist[t] / tot : 0) * 65535 + 0.5);
}

inline Fingerprint fingerprint(const std::vector<double>& rawTris) {
    std::vector<double> tris = sanitize(rawTris);
    Samples S = samplePoints(tris);
    Fingerprint F;
    f1(S, F);
    f3(S, F);
    return F;
}

/** 1 - (L1 distance of D2 + A3)/4, in [0,1]: 1 = identical global shape histograms. */
inline double f1Similarity(const Fingerprint& a, const Fingerprint& b) {
    double s = 0;
    for (int i = 0; i < D2_BINS; i++) s += std::fabs(a.d2[i] - b.d2[i]);
    for (int i = 0; i < A3_BINS; i++) s += std::fabs(a.a3[i] - b.a3[i]);
    return 1 - s / 4;
}
inline double f3Intersection(const Fingerprint& a, const Fingerprint& b) {
    long long s = 0;
    for (int t = 0; t < F3_SIZE; t++) s += std::min(a.f3[t], b.f3[t]);
    return s / 65535.0;
}

} // namespace fp
} // namespace swamp

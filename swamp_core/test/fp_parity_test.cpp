// C++ fp/v1 vs the JS reference (packages/fp) on the committed test meshes - once in the C locale
// and once in a decimal-comma locale (cs_CZ), where a locale-dependent parser read "6.61e+00" as 6
// (review 2026-10-07). The comma pass is skipped if no such locale is installed.
#include "swamp_fp.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <iostream>
#include <clocale>
using json = nlohmann::json;
using namespace swamp;
int main(int argc, char** argv) {
    std::string root = argc > 1 ? argv[1] : ".";
    json v = json::parse(std::ifstream(root + "/swamp_core/test/fp-vectors.json"));
    int fails = 0;
    for (const char* loc : {"C", "cs_CZ.UTF-8"}) {
    if (!std::setlocale(LC_ALL, loc)) { std::cout << "(locale " << loc << " not installed - skipped)\n"; continue; }
    std::cout << "locale " << loc << ":\n";
    for (const auto& g : v) {
        std::ifstream f(root + "/swamp_core/test/meshes/" + g["id"].get<std::string>() + ".stl", std::ios::binary);
        std::stringstream ss; ss << f.rdbuf();
        fp::Fingerprint F = fp::fingerprint(fp::parseStl(ss.str()));
        double d1 = 0; long long d3 = 0, binsDiff = 0;
        for (int i = 0; i < fp::D2_BINS; i++) d1 += std::fabs(F.d2[i] - g["d2"][i].get<double>());
        for (int i = 0; i < fp::A3_BINS; i++) d1 += std::fabs(F.a3[i] - g["a3"][i].get<double>());
        for (int t = 0; t < fp::F3_SIZE; t++) { long long x = std::llabs((long long)F.f3[t] - g["f3"][t].get<long long>()); d3 += x; if (x) binsDiff++; }
        bool ok = d1 < 1e-9 && d3 == 0;
        std::cout << g["id"] << (ok ? " identical" : " DIFFERS") << "  F1 L1 diff " << d1 << ", F3 bins differing " << binsDiff << " (sum " << d3 << ")\n";
        if (d1 > 1e-3 || d3 > 200) fails++;   // tolerance: float noise may flip a rare bin
    }
    }
    std::cout << (fails ? "FAIL" : "PASS") << "\n";
    return fails ? 1 : 0;
}

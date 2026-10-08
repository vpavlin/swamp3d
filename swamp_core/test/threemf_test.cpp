// swamp_3mf.hpp: geometry out of Bambu-style 3MF projects (components, transforms, deflate).
//   threemf_test <meshes dir> [extra.3mf ...]   (extra files: only checked to convert at all)
#include "swamp_3mf.hpp"
#include <filesystem>
#include <iostream>
namespace fs = std::filesystem;
static int passes = 0, fails = 0;
#define CHECK(c, w) do { if (c) passes++; else { fails++; std::cerr << "FAIL: " << w << "\n"; } } while (0)

struct Stl { uint32_t n = 0; float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}; };
static Stl readStl(const std::string& p) {
    Stl s;
    std::ifstream f(p, std::ios::binary);
    f.seekg(80); f.read(reinterpret_cast<char*>(&s.n), 4);
    for (uint32_t i = 0; i < s.n; i++) {
        float t[12]; uint16_t a;
        f.read(reinterpret_cast<char*>(t), 48); f.read(reinterpret_cast<char*>(&a), 2);
        for (int v = 1; v < 4; v++) for (int j = 0; j < 3; j++) { s.lo[j] = std::min(s.lo[j], t[v * 3 + j]); s.hi[j] = std::max(s.hi[j], t[v * 3 + j]); }
    }
    return s;
}
static bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

int main(int argc, char** argv) {
    std::string meshes = argc > 1 ? argv[1] : "meshes";
    fs::path tmp = fs::temp_directory_path() / "swamp-3mf-test";
    fs::remove_all(tmp); fs::create_directories(tmp);
    std::vector<std::string> stls; std::string err;
    bool ok = swamp3mf::toStls(meshes + "/bambu-style.3mf", tmp.string(), stls, err);
    CHECK(ok, "bambu-style.3mf converts: " << err);
    CHECK(stls.size() == 2, "one STL per printable item, the non-printable one skipped (got " << stls.size() << ")");
    if (stls.size() == 2) {
        Stl cube = readStl(stls[0]), tets = readStl(stls[1]);
        CHECK(cube.n == 12, "cube: 12 triangles");
        CHECK(near(cube.lo[0], 100) && near(cube.hi[0], 110) && near(cube.lo[2], 0) && near(cube.hi[2], 10), "cube: component scale x10 then item move to (100,100)");
        CHECK(tets.n == 8, "two tetrahedron components: 8 triangles");
        CHECK(near(tets.lo[0], 50) && near(tets.hi[0], 80) && near(tets.hi[2], 10), "tetrahedra: component offsets + item move");
    }
    {   // units: a one-inch cube moved 2 inches along x, in a model whose unit is inches
        fs::path sub = tmp / "inch"; fs::create_directories(sub);
        std::vector<std::string> out; std::string e;
        bool k = swamp3mf::toStls(meshes + "/inch.3mf", sub.string(), out, e);
        Stl c = k && out.size() == 1 ? readStl(out[0]) : Stl();
        CHECK(k && near(c.lo[0], 50.8f) && near(c.hi[0], 76.2f) && near(c.hi[2], 25.4f), "a model in inches comes out in millimetres (" << c.lo[0] << ".." << c.hi[0] << ") " << e);
    }
    for (int i = 2; i < argc; i++) {   // real projects (e.g. one OrcaSlicer saved)
        fs::path sub = tmp / ("x" + std::to_string(i)); fs::create_directories(sub);
        std::vector<std::string> out; std::string e;
        bool k = swamp3mf::toStls(argv[i], sub.string(), out, e);
        uint32_t tri = 0; for (const auto& s : out) tri += readStl(s).n;
        CHECK(k && tri > 0, argv[i] << " converts (" << out.size() << " items, " << tri << " triangles) " << e);
    }
    // refusals
    std::ofstream(tmp / "junk.3mf") << "not a zip";
    stls.clear();
    CHECK(!swamp3mf::toStls((tmp / "junk.3mf").string(), tmp.string(), stls, err) && err == "not a zip archive", "junk refused: " << err);
    CHECK(!swamp3mf::toStls(meshes + "/torus.stl", tmp.string(), stls, err), "an STL is not a 3MF");
    std::cout << passes << " passed, " << fails << " failed\n";
    return fails ? 1 : 0;
}

#!/usr/bin/env bash
# Build + run the C++ tests without nix (system g++, OpenSSL, nlohmann-json, Qt6Core).
#   swamp_core/test/run-tests.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; REPO="$(cd "$HERE/../.." && pwd)"
OUT="${SWAMP_TEST_OUT:-${TMPDIR:-/tmp}/swamp-ctest}"; mkdir -p "$OUT"
CXX="g++ -std=c++20 -O1 -g -Wall -Wno-deprecated-declarations -Wno-unused-but-set-variable -I$HERE/../src"
$CXX "$HERE/catalog_test.cpp" -lcrypto -o "$OUT/catalog_test" && "$OUT/catalog_test"
$CXX "$HERE/index_test.cpp" -lcrypto -o "$OUT/index_test" && "$OUT/index_test"
$CXX -O2 "$HERE/fp_parity_test.cpp" -o "$OUT/fp_parity" && "$OUT/fp_parity" "$REPO"
python3 "$HERE/gen-test-3mf.py" --hostile "$OUT/hostile-3mf" > /dev/null
$CXX -fPIC $(pkg-config --cflags Qt6Core) "$HERE/threemf_test.cpp" $(pkg-config --libs Qt6Core) -lz -o "$OUT/threemf_test" && SWAMP_TEST_HOSTILE_3MF="$OUT/hostile-3mf" "$OUT/threemf_test" "$HERE/meshes" ${SWAMP_TEST_3MF:-}
$CXX -fPIC -I"$HERE/fakesdk" $(pkg-config --cflags Qt6Core) "$HERE/e2e_test.cpp" "$HERE/../src/swamp_core_impl.cpp" $(pkg-config --libs Qt6Core) -lssl -lcrypto -lz -o "$OUT/e2e_test"
# a fake Bambu Lab printer for the LAN printing tests (FTPS, MQTT over TLS, SSDP)
MOCK="$OUT/bambu-mock"; rm -rf "$MOCK"; mkdir -p "$MOCK"
python3 "$HERE/bambu_mock.py" "$MOCK" 29990 28883 22021 03919A3B0000001 12345678 > "$MOCK/py.log" 2>&1 &
MOCKPID=$!; trap 'kill $MOCKPID 2>/dev/null' EXIT
for i in $(seq 1 50); do [ -f "$MOCK/ready" ] && break; sleep 0.2; done
$CXX "$HERE/bambu_test.cpp" -lssl -lcrypto -o "$OUT/bambu_test" && "$OUT/bambu_test" "$MOCK" 29990 28883 22021 "$HERE/meshes/torus.stl"
rm -f "$HERE/fake-orca-args.txt"
SWAMP_MOCK_PRINTER="$MOCK" SWAMP_MOCK_MQTT=28883 SWAMP_MOCK_FTPS=29990 SWAMP_BAMBU_SSDP_PORT=22021 SWAMP_TEST_DIR="$HERE" "$OUT/e2e_test" "$REPO"

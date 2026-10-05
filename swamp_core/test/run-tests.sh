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
$CXX -fPIC -I"$HERE/fakesdk" $(pkg-config --cflags Qt6Core) "$HERE/e2e_test.cpp" "$HERE/../src/swamp_core_impl.cpp" $(pkg-config --libs Qt6Core) -lcrypto -o "$OUT/e2e_test"
"$OUT/e2e_test" "$REPO"

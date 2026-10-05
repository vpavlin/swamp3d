#!/usr/bin/env bash
# Render the Swamp view offscreen against the real core (system Qt6 Quick + the Basecamp-bundled
# design system). Screenshots -> ${SW_OUT:-/tmp/swamp-harness}
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; HDIR="$ROOT/scripts/qml-harness"
OUT="${SW_OUT:-/tmp/swamp-harness}"; mkdir -p "$OUT"
DS="${SW_DS:-$HOME/basecamp-test/squashfs-root/usr/lib}"
"$(pkg-config --variable=libexecdir Qt6Core)/moc" "$HDIR/harness.cpp" -o "$HDIR/harness.moc"
g++ -std=c++20 -O1 -fPIC -Wno-deprecated-declarations -I"$HDIR" -I"$ROOT/swamp_core/test/fakesdk" -I"$ROOT/swamp_core/src" \
    $(pkg-config --cflags Qt6Quick Qt6Qml Qt6Gui Qt6Core) "$HDIR/harness.cpp" "$ROOT/swamp_core/src/swamp_core_impl.cpp" \
    $(pkg-config --libs Qt6Quick Qt6Qml Qt6Gui Qt6Core) -lcrypto -o "$OUT/harness"
QML_IMPORT_PATH="$DS" "$OUT/harness" "$ROOT/module/Main.qml" "$OUT" "$ROOT"

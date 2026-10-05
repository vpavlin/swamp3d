#!/usr/bin/env bash
# Global search over the real Logos test fleet (ADRs 0014 + 0015), two headless logosctl nodes:
# A indexes and follows everything; B follows only "tools". A publishes in "toys": B must NOT get
# it by sync, but must FIND it through A's index (shards fetched privately over Mix), open it and
# download the file, verified.
#
#   LOGOSCTL=/path/to/logosctl LGX_DIR=/dir/with/*.lgx hub/two-node.sh [stl]
#
# LGX_DIR must hold the portable .lgx of: delivery_module (0.3.x), ble_mesh, keycard, loam_core
# (port/0.3) and swamp_core. storage_module is the host's (bundled in logosctl 0.3.1).
set -euo pipefail
L="${LOGOSCTL:?set LOGOSCTL}"
LGX="${LGX_DIR:?set LGX_DIR}"
STL="${1:-$(cd "$(dirname "$0")/.." && pwd)/bench/data/raw/74890.stl}"
BASE=/tmp/swamp-search

envfor() { case $1 in a) echo "SWAMP_INDEXER=1 SWAMP_INDEX_EVERY_MS=15000";; b) echo "SWAMP_CATEGORIES=tools";; esac; }
ctl() { local s=$1; shift; env $(envfor $s) HOME="$BASE/$s/home" SWAMP_CORE_DATA="$BASE/$s/home/.swamp-core" SWAMP_DOWNLOADS="$BASE/$s/home/Swamp" LOGOSCTL_CONFIG_DIR="$BASE/$s/cfg" QT_QPA_PLATFORM=offscreen timeout 90 "$L" "$@"; }
res() { python3 -c 'import json,sys; raw=sys.stdin.read().strip().splitlines(); d=json.loads(raw[-1]); r=d.get("result",d) if isinstance(d,dict) else d
for _ in range(3):
  if isinstance(r,str):
    try: r=json.loads(r)
    except Exception: break
print(json.dumps(r))'; }

for s in a b; do
  [ -d "$BASE/$s/cfg" ] && ctl $s daemon stop >/dev/null 2>&1 || true
done
for s in a b; do
  rm -rf "/tmp/swamp-search/$s"   # literal path: never rm through a redefined HOME
  mkdir -p "$BASE/$s/home"
done
# Each host Storage node gets its own TCP port (libstorage 0.5 has no separate discovery port)
# and announces loopback, so the two can find and dial each other on this machine
# (logos-storage: no extip = nothing announced). Let the host write its default config first,
# then merge just those keys - a key libstorage doesn't know makes the whole config fail to load.
port=8301
for s in a b; do
  ctl $s daemon start --detach >/dev/null; sleep 4; ctl $s daemon stop >/dev/null
  python3 -c "import json,sys; p=sys.argv[1]; d=json.load(open(p)); d.update({'listen-port': int(sys.argv[2]), 'nat': 'extip:127.0.0.1'}); json.dump(d, open(p,'w'))" \
    "$BASE/$s/home/.logos_storage/config.json" $port
  port=$((port+1))
done

# Storage finds content through the DHT, and two fresh nodes on one box don't find each other on
# their own: B bootstraps from A's signed peer record (as a NAT-ed client would from a hub).
ctl a daemon start --detach >/dev/null
SPR=""
for i in $(seq 1 12); do
  SPR=$(ctl a call storage_module spr -j 2>/dev/null | res | python3 -c 'import json,sys; r=json.load(sys.stdin); r=r.get("value",r) if isinstance(r,dict) else r; print(r if isinstance(r,str) and r.startswith("spr:") else "")' || true)
  [ -n "$SPR" ] && break; sleep 2
done
[ -n "$SPR" ] || { echo "FAIL: no SPR from A's Storage node"; exit 1; }
python3 -c "import json,sys; p=sys.argv[1]; d=json.load(open(p)); d['bootstrap-node']=[sys.argv[2]]; json.dump(d, open(p,'w'))" \
  "$BASE/b/home/.logos_storage/config.json" "$SPR"
echo "B bootstraps Storage from A: ${SPR:0:40}..."

for s in a b; do
  [ $s = a ] || ctl $s daemon start --detach >/dev/null
  for f in "$LGX"/*.lgx; do ctl $s install "$f" -y >/dev/null; done
  ctl $s module load swamp_core >/dev/null
  echo "node $s: $(ctl $s status -j | python3 -c 'import json,sys; d=json.load(sys.stdin); print(", ".join(m["name"]+":"+m["status"] for m in d["modules"]))')"
done

wait_for() { local what=$1 cmd=$2 secs=${3:-180}; for i in $(seq 1 $((secs/5))); do if eval "$cmd"; then echo "  ok: $what ($((i*5))s)"; return 0; fi; sleep 5; done; echo "  TIMEOUT: $what"; return 1; }
st() { ctl $1 call swamp_core snapshot -j | res | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('status'))"; }
wait_for "A connected" '[ "$(st a)" = Connected ]'
wait_for "B connected" '[ "$(st b)" = Connected ]'

TITLE="Swampy duck $RANDOM$RANDOM"
WORD=$(echo "$TITLE" | awk '{print $3}')
ctl a call swamp_core setProfile "str:{\"name\":\"Indexer A\"}" -j | res
PUB=$(ctl a call swamp_core publish "str:{\"title\":\"$TITLE\",\"licence\":\"CC-BY-4.0\",\"category\":\"toys\",\"tags\":[\"duck\"],\"files\":[{\"path\":\"$STL\"}]}" -j | res)
echo "publish: $PUB"
MID=$(echo "$PUB" | python3 -c "import json,sys; print(json.load(sys.stdin)['modelId'])")
j() { python3 -c "import json,sys
try: d=json.load(sys.stdin); print(eval(sys.argv[1]))
except Exception as e: print('')" "$1"; }
built() { ctl a call swamp_core snapshot -j | res | j "d['index']['built']"; }
wait_for "A built and published an index" '[ "$(built)" -ge 1 ] 2>/dev/null' 300
known() { ctl b call swamp_core snapshot -j | res | j "d['index']['known']"; }
wait_for "B received A's index manifest" '[ "$(known)" -ge 1 ] 2>/dev/null' 240
local_total() { ctl b call swamp_core listModels "str:{\"q\":\"$WORD\"}" -j | res | j "d['total']"; }
[ "$(local_total)" = 0 ] && echo "  ok: B (tools only) did NOT get the toys model by sync" || { echo "FAIL: B mirrored a category it doesn't follow"; exit 1; }
gsearch() { ctl b call swamp_core globalSearch "str:{\"q\":\"$WORD\"}" -j | res; }
wait_for "B finds it through the index" '[ "$(gsearch | j "sum(1 for r in d[\"results\"] if r[\"modelId\"]==\"$MID\")")" = 1 ]' 600
echo "  index info: $(gsearch | j "d['index']")"
opened() { ctl b call swamp_core getModel "str:$MID" -j | res | j "d.get('ok')"; }
wait_for "B opens it (record shard from the index)" '[ "$(opened)" = True ]' 600
ctl b call swamp_core download "str:$MID" str:1 -j | res
dl() { ctl b call swamp_core getModel "str:$MID" -j | res | j "d['model']['versions'][0].get('download',{}).get('status','')"; }
wait_for "B downloaded the file" '[ "$(dl)" = done ]' 600
DIR=$(ctl b call swamp_core getModel "str:$MID" -j | res | j "d['model']['versions'][0]['download']['dir']")
if cmp -s "$STL" "$DIR/$(basename "$STL")"; then echo "PASS: found via the index, opened from a record shard, downloaded byte-identical"; else echo "FAIL: B's copy differs or is missing"; exit 1; fi
echo "B snapshot: $(ctl b call swamp_core snapshot -j | res | j "{k: d[k] for k in ('index','counters')}")"
for s in a b; do ctl $s daemon stop >/dev/null 2>&1 || true; done

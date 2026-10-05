#!/usr/bin/env bash
# Two headless Swamp nodes on one machine (logosctl 0.3.1), over the real Logos test fleet:
# A publishes a real STL, B sees it in its catalogue and downloads it, verified.
#
#   LOGOSCTL=/path/to/logosctl LGX_DIR=/dir/with/*.lgx hub/two-node.sh [stl]
#
# LGX_DIR must hold the portable .lgx of: delivery_module (0.3.x), ble_mesh, keycard, loam_core
# (port/0.3) and swamp_core. storage_module is the host's (bundled in logosctl 0.3.1).
set -euo pipefail
L="${LOGOSCTL:?set LOGOSCTL}"
LGX="${LGX_DIR:?set LGX_DIR}"
STL="${1:-$(cd "$(dirname "$0")/.." && pwd)/bench/data/raw/74890.stl}"
BASE=/tmp/swamp-2n

ctl() { local s=$1; shift; HOME="$BASE/$s/home" SWAMP_CORE_DATA="$BASE/$s/home/.swamp-core" SWAMP_DOWNLOADS="$BASE/$s/home/Swamp" LOGOSCTL_CONFIG_DIR="$BASE/$s/cfg" QT_QPA_PLATFORM=offscreen timeout 90 "$L" "$@"; }
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
  rm -rf "/tmp/swamp-2n/$s"   # literal path: never rm through a redefined HOME
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

ctl a call swamp_core setProfile "str:{\"name\":\"Node A\"}" -j | res
PUB=$(ctl a call swamp_core publish "str:{\"title\":\"Two-node test\",\"licence\":\"CC-BY-4.0\",\"tags\":[\"test\"],\"files\":[{\"path\":\"$STL\"}]}" -j | res)
echo "publish: $PUB"
MID=$(echo "$PUB" | python3 -c "import json,sys; print(json.load(sys.stdin)['modelId'])")

models() { ctl b call swamp_core listModels str:{} -j | res | python3 -c "import json,sys; print(json.load(sys.stdin).get('total',0))"; }
wait_for "B sees the model" '[ "$(models)" -ge 1 ]' 240
cids() { ctl b call swamp_core getModel "str:$MID" -j | res | python3 -c "
import json,sys
try: v=json.load(sys.stdin)['model']['versions'][0]; print(min(f['cids'] for f in v['files']))
except Exception: print(0)"; }
wait_for "A's upload finished and its CID reached B" '[ "$(cids)" -ge 1 ]' 300
ctl b call swamp_core download "str:$MID" str:1 -j | res
dl() { ctl b call swamp_core getModel "str:$MID" -j | res | python3 -c "import json,sys; v=json.load(sys.stdin)['model']['versions'][0]; print(v.get('download',{}).get('status',''))"; }
wait_for "B downloaded the file" '[ "$(dl)" = done ]' 300
DIR=$(ctl b call swamp_core getModel "str:$MID" -j | res | python3 -c "import json,sys; print(json.load(sys.stdin)['model']['versions'][0]['download']['dir'])")
addr() { ctl $1 call swamp_core snapshot -j | res | python3 -c "import json,sys; print(json.load(sys.stdin)['me']['address'])"; }
fetched() { ctl b call swamp_core snapshot -j | res | python3 -c "import json,sys; print(json.load(sys.stdin)['counters']['fetched'])"; }
case "$DIR" in "$BASE/b/"*) ;; *) echo "FAIL: B wrote outside its session: $DIR"; exit 1;; esac
[ "$(addr a)" != "$(addr b)" ] || { echo "FAIL: A and B share an identity (same data dir?)"; exit 1; }
[ "$(fetched)" -ge 1 ] || { echo "FAIL: B never fetched anything over Storage"; exit 1; }
if cmp -s "$STL" "$DIR/$(basename "$STL")"; then echo "PASS: B fetched the file over Logos Storage and its copy is byte-identical"; else echo "FAIL: B's copy differs or is missing"; exit 1; fi
ctl b call swamp_core snapshot -j | res
for s in a b; do ctl $s daemon stop >/dev/null 2>&1 || true; done

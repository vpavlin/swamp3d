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

ctl() { local s=$1; shift; HOME="$BASE/$s/home" LOGOSCTL_CONFIG_DIR="$BASE/$s/cfg" QT_QPA_PLATFORM=offscreen timeout 90 "$L" "$@"; }
res() { python3 -c 'import json,sys; raw=sys.stdin.read().strip().splitlines(); d=json.loads(raw[-1]); r=d.get("result",d) if isinstance(d,dict) else d
for _ in range(3):
  if isinstance(r,str):
    try: r=json.loads(r)
    except Exception: break
print(json.dumps(r))'; }

for s in a b; do
  rm -rf "/tmp/swamp-2n/$s"   # literal path: never rm through a redefined HOME
  mkdir -p "$BASE/$s/home/.logos_storage"
done
# Each host Storage node gets its own ports and announces loopback, so the two can find and
# dial each other on this machine (logos-storage: no extip = nothing announced).
port=8301
for s in a b; do
  cat > "$BASE/$s/home/.logos_storage/config.json" <<EOF
{"config-version":3,"data-dir":"$BASE/$s/home/.logos_storage/data","listen-port":$port,"disc-port":$((port+100)),"nat":"extip:127.0.0.1","log-level":"INFO"}
EOF
  port=$((port+1))
done

for s in a b; do
  ctl $s daemon start --detach >/dev/null
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
cids() { ctl b call swamp_core getModel "str:$MID" -j | res | python3 -c "import json,sys; v=json.load(sys.stdin)['model']['versions'][0]; print(min(f['cids'] for f in v['files']))"; }
wait_for "A's upload finished and its CID reached B" '[ "$(cids)" -ge 1 ]' 300
ctl b call swamp_core download "str:$MID" str:1 -j | res
dl() { ctl b call swamp_core getModel "str:$MID" -j | res | python3 -c "import json,sys; v=json.load(sys.stdin)['model']['versions'][0]; print(v.get('download',{}).get('status',''))"; }
wait_for "B downloaded the file" '[ "$(dl)" = done ]' 300
DIR=$(ctl b call swamp_core getModel "str:$MID" -j | res | python3 -c "import json,sys; print(json.load(sys.stdin)['model']['versions'][0]['download']['dir'])")
if cmp -s "$STL" "$DIR/$(basename "$STL")"; then echo "PASS: B's copy is byte-identical"; else echo "FAIL: B's copy differs or is missing"; fi
ctl b call swamp_core snapshot -j | res

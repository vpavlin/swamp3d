#!/usr/bin/env bash
# An always-on Swamp hub on a server with a public IP: a headless swamp_core (SWAMP_HUB=1) under
# logosctl 0.3.1, as a systemd service. A hub follows every category, keeps a copy of every file it
# sees, builds the search index, and is a Storage node others can always reach (public extip,
# AutoNAT + relay server for NAT-ed clients). Anyone can run one.
#
#   sudo PUBLIC_IP=203.0.113.7 LOGOSCTL_APPIMAGE=/path/logosctl-x86_64.AppImage LGX_DIR=/path/lgx hub/vps-hub.sh
#
# LGX_DIR holds the portable .lgx of delivery_module, ble_mesh, keycard, loam_core and swamp_core.
# Optional: HUB_DIR (/var/lib/swamp-hub), STORAGE_PORT (8299), HUB_NAME ("Swamp hub").
# Re-running upgrades the packages and restarts the service; the hub's identity and data stay.
set -euo pipefail
: "${PUBLIC_IP:?set PUBLIC_IP}" "${LOGOSCTL_APPIMAGE:?set LOGOSCTL_APPIMAGE}" "${LGX_DIR:?set LGX_DIR}"
HUB_DIR="${HUB_DIR:-/var/lib/swamp-hub}"
STORAGE_PORT="${STORAGE_PORT:-8299}"
HUB_NAME="${HUB_NAME:-Swamp hub}"
BIN="$HUB_DIR/bin/logosctl"
mkdir -p "$HUB_DIR/bin" "$HUB_DIR/home" "$HUB_DIR/lgx"
install -m 0755 "$LOGOSCTL_APPIMAGE" "$BIN"
cp "$LGX_DIR"/*.lgx "$HUB_DIR/lgx/"

# the environment of every logosctl call for this hub (session dir + its own HOME; no FUSE on servers)
cat > "$HUB_DIR/env" <<EOF
HOME=$HUB_DIR/home
LOGOSCTL_CONFIG_DIR=$HUB_DIR/cfg
SWAMP_CORE_DATA=$HUB_DIR/home/.swamp-core
SWAMP_DOWNLOADS=$HUB_DIR/home/Swamp
SWAMP_HUB=1
QT_QPA_PLATFORM=offscreen
APPIMAGE_EXTRACT_AND_RUN=1
EOF
ctl() { env $(cat "$HUB_DIR/env") timeout 120 "$BIN" "$@"; }

systemctl stop swamp-hub.service 2>/dev/null || true

# Storage: the host writes its default config on first start; then merge in what makes a hub
# reachable. Only keys libstorage knows: an unknown key makes the whole config fail to load.
CFG="$HUB_DIR/home/.logos_storage/config.json"
if [ ! -f "$CFG" ]; then ctl daemon start --detach >/dev/null; sleep 5; ctl daemon stop >/dev/null || true; fi
python3 - "$CFG" "$PUBLIC_IP" "$STORAGE_PORT" <<'EOF'
import json, sys
p, ip, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
d = json.load(open(p))
d.update({"listen-port": port, "nat": "extip:" + ip, "autonat-server": True, "relay-server": True})
json.dump(d, open(p, "w"), indent=2)
EOF

# install / upgrade the packages, then let systemd run it
ctl daemon start --detach >/dev/null
for f in "$HUB_DIR"/lgx/*.lgx; do ctl install "$f" -y >/dev/null; done
ctl daemon stop >/dev/null || true

# after the daemon starts: load the core (it stays loaded while the daemon runs) and name the hub
cat > "$HUB_DIR/bin/after-start" <<AFTER
#!/bin/sh
for i in \$(seq 1 60); do "$BIN" status >/dev/null 2>&1 && break; sleep 2; done
"$BIN" module load swamp_core
sleep 5
"$BIN" call swamp_core setProfile 'str:{"name":"$HUB_NAME","bio":"An always-on Swamp hub: keeps copies of models and builds the search index"}' >/dev/null
AFTER
chmod 0755 "$HUB_DIR/bin/after-start"

cat > /etc/systemd/system/swamp-hub.service <<EOF
[Unit]
Description=Swamp hub (headless swamp_core under logosctl)
After=network-online.target
Wants=network-online.target

[Service]
EnvironmentFile=$HUB_DIR/env
ExecStart=$BIN daemon start
ExecStartPost=$HUB_DIR/bin/after-start
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable --now swamp-hub.service

# Storage must be reachable from the internet on its TCP port
if command -v ufw >/dev/null && ufw status | grep -q active; then ufw allow "$STORAGE_PORT/tcp" >/dev/null; fi
if command -v firewall-cmd >/dev/null && firewall-cmd --state >/dev/null 2>&1; then firewall-cmd -q --permanent --add-port="$STORAGE_PORT/tcp" && firewall-cmd -q --reload; fi

echo "swamp-hub: started. Watch it with:  env \$(cat $HUB_DIR/env) $BIN call swamp_core snapshot"

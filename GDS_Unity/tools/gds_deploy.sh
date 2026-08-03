#!/usr/bin/env bash
# gds_deploy.sh - one-command deploy + test + log-pull for Game Dev Story on an
# R36S over SSH.  Run from your PC.  This is the "make testing easy" script.
#
# Usage:
#   ./gds_deploy.sh [user@host] [port_dir]
#
#   user@host  SSH target for the R36S (default: read from GDS_R36S_HOST env,
#              or ssh/ArkOS default "ark@<ip>").  Example: ark@192.168.1.50
#   port_dir   path to the port zip (default: GDS_Unity/gamedevstory.zip)
#
# Env overrides:
#   GDS_R36S_HOST   SSH host (default: ark@10.1.1.2)
#   GDS_PORTS_DIR   ports dir on device (default: /roms/ports)
#
# It:
#   1. builds loader2 (if sources newer) and repacks gamedevstory.zip
#   2. scp's the zip to the device, unzips into <ports>/gamedevstory (replacing)
#   3. sets exec bits, runs the launcher via SSH
#   4. pulls back port_launch.log + loader.log into ./gds_logs/
#
set -euo pipefail

HOST="${GDS_R36S_HOST:-${1:-ark@10.1.1.2}}"
PORTS_DIR="${GDS_PORTS_DIR:-/roms/ports}"
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"           # repo root = GDS_Unity
ZIP="${GDS_ZIP:-$REPO/gamedevstory.zip}"

echo "==> Host: $HOST   Ports dir: $PORTS_DIR"

# --- 1. build + repack if loader sources changed ---
if [ "$REPO/loader/loader.c" -nt "$REPO/loader/loader2" ] \
   || [ "$REPO/loader/freestdlib.c" -nt "$REPO/loader/loader2" ] \
   || [ "$REPO/loader/host_syms.c" -nt "$REPO/loader/loader2" ] \
   || [ "$REPO/loader/jni_shim.c" -nt "$REPO/loader/loader2" ]; then
  echo "==> Rebuilding loader2..."
  ( cd "$REPO" && bash loader/build.sh )
fi

# --- 2. ensure port zip is current ---
if [ ! -f "$ZIP" ] || [ "$REPO/ports/gamedevstory/Game Dev Story.sh" -nt "$ZIP" ]; then
  echo "==> Repacking gamedevstory.zip..."
  ( cd "$REPO/ports/gamedevstory" && \
    zip -q -r -y "$ZIP" "Game Dev Story.sh" gamedevstory port.json \
        README.md gameinfo.xml screenshot.png cover.png )
fi

echo "==> Uploading $ZIP to $HOST:$PORTS_DIR ..."
scp "$ZIP" "$HOST:$PORTS_DIR/gamedevstory.zip"

echo "==> Installing (unzip + perms + run) ..."
ssh "$HOST" "
  set -e
  rm -rf '$PORTS_DIR/gamedevstory'
  cd '$PORTS_DIR'
  unzip -o gamedevstory.zip
  rm -f gamedevstory.zip
  chmod +x '$PORTS_DIR/Game Dev Story.sh' '$PORTS_DIR/gamedevstory/loader2'
  echo '=== running launcher ==='
  bash '$PORTS_DIR/Game Dev Story.sh'
  echo '=== launcher exited with code '\$?' ==='
"

echo "==> Pulling logs ..."
mkdir -p "$HERE/gds_logs"
scp "$HOST:$PORTS_DIR/port_launch.log" "$HERE/gds_logs/" 2>/dev/null || true
scp "$HOST:$PORTS_DIR/gamedevstory/loader.log" "$HERE/gds_logs/" 2>/dev/null || true
scp "$HOST:/tmp/gamedevstory_loader.log" "$HERE/gds_logs/" 2>/dev/null || true

echo ""
echo "=== Logs saved to $HERE/gds_logs/ ==="
for f in "$HERE"/gds_logs/*; do
  [ -f "$f" ] && echo "--- $(basename "$f") ---" && cat "$f" && echo
done

#!/usr/bin/env bash
# gds_deploy.sh - one-command deploy + test + log-pull for Game Dev Story on an
# R36S over SSH.  Run from your PC.  NO repo checkout needed - it downloads the
# pre-built gamedevstory.zip straight from GitHub, installs it, runs it, and
# pulls the log back.
#
# Usage:
#   ./gds_deploy.sh [user@host]
#
#   user@host  SSH target for the R36S (default: ark@10.1.1.2)
#
# Env overrides:
#   GDS_R36S_HOST   SSH host (default: ark@10.1.1.2)
#   GDS_PORTS_DIR   ports dir on device (default: /roms/ports)
#   GDS_BRANCH      git branch (default: arena/019fc860-asdf)
#   GDS_ZIP_URL     full URL to a gamedevstory.zip (default: auto from GDS_BRANCH)
#
# It:
#   1. downloads the latest pre-built gamedevstory.zip from GitHub (or uses a
#      local one if GDS_ZIP_URL is a path / the file is next to this script)
#   2. scp's it to the device, unzips into <ports>/gamedevstory (replacing)
#   3. sets exec bits, runs the launcher via SSH
#   4. pulls back port_launch.log + loader.log into ./gds_logs/ and prints them
#
set -euo pipefail

HOST="${GDS_R36S_HOST:-${1:-ark@10.1.1.2}}"
PORTS_DIR="${GDS_PORTS_DIR:-/roms/ports}"
BRANCH="${GDS_BRANCH:-arena/019fc860-asdf}"
HERE="$(cd "$(dirname "$0")" && pwd)"
LOGDIR="$HERE/gds_logs"

# --- locate / download the port zip ---
ZIP=""
if [ -n "${GDS_ZIP_URL:-}" ]; then
  if [ -f "$GDS_ZIP_URL" ]; then
    ZIP="$GDS_ZIP_URL"
  else
    ZIP="$HERE/gamedevstory.zip"
    echo "==> Downloading $GDS_ZIP_URL"
    curl -sL -o "$ZIP" "$GDS_ZIP_URL"
  fi
else
  # default: download the pre-built zip committed to the repo
  URL="https://github.com/jackomix/asdf/raw/$BRANCH/GDS_Unity/gamedevstory.zip"
  ZIP="$HERE/gamedevstory.zip"
  echo "==> Downloading $URL"
  curl -sL -o "$ZIP" "$URL"
fi

if [ ! -f "$ZIP" ] || [ ! -s "$ZIP" ]; then
  echo "ERROR: could not get gamedevstory.zip" >&2
  exit 1
fi
# sanity: must be a zip
if ! unzip -t "$ZIP" >/dev/null 2>&1; then
  echo "ERROR: $ZIP is not a valid zip (download may have failed)" >&2
  exit 1
fi
echo "==> Using $ZIP"

echo "==> Uploading to $HOST:$PORTS_DIR ..."
scp "$ZIP" "$HOST:$PORTS_DIR/gamedevstory.zip"

echo "==> Installing + running ..."
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
mkdir -p "$LOGDIR"
rm -f "$LOGDIR"/port_launch.log "$LOGDIR"/loader.log "$LOGDIR"/gamedevstory_loader.log
scp "$HOST:$PORTS_DIR/port_launch.log" "$LOGDIR/" 2>/dev/null || true
scp "$HOST:$PORTS_DIR/gamedevstory/loader.log" "$LOGDIR/" 2>/dev/null || true
scp "$HOST:/tmp/gamedevstory_loader.log" "$LOGDIR/" 2>/dev/null || true

echo ""
echo "=== Logs saved to $LOGDIR/ ==="
for f in "$LOGDIR"/*.log; do
  [ -f "$f" ] && echo "----- $(basename "$f") -----" && cat "$f" && echo
done

#!/usr/bin/env bash
# gds_deploy.sh - one-command deploy + run + log-pull for Game Dev Story on an
# R36S over SSH.  macOS + Linux friendly (no `timeout` binary needed).
#
#   # with SSH key (recommended, no prompts):
#   GDS_SSH_KEY="$HOME/.ssh/id_ed25519_antigravity" ./gds_deploy.sh ark@192.168.18.20
#
#   # with password (will prompt):
#   ./gds_deploy.sh ark@192.168.18.20
#
# It downloads the pre-built gamedevstory.zip from GitHub, uploads it (with
# retries, since the R36S Wi-Fi/SSH is flaky), installs it, runs it, and pulls
# back the log.
set -uo pipefail

HOST="${GDS_R36S_HOST:-${1:-ark@10.1.1.2}}"
PORTS_DIR="${GDS_PORTS_DIR:-/roms/ports}"
BRANCH="${GDS_BRANCH:-arena/019fc860-asdf}"
SSH_KEY="${GDS_SSH_KEY:-}"
HERE="$(cd "$(dirname "$0")" && pwd)"
LOGDIR="$HERE/gds_logs"
ZIP="$HERE/gamedevstory.zip"

# --- SSH options (no `timeout` binary; rely on ssh's own ConnectTimeout) ---
# StrictHostKeyChecking=no + UserKnownHostsFile=/dev/null tolerate ArkOS
# host-key changes (which otherwise block ssh/scp instantly).
if [ -n "$SSH_KEY" ]; then
  BASE=(ssh -i "$SSH_KEY" -o BatchMode=yes -o ConnectTimeout=10 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)
  SCPS=(scp -i "$SSH_KEY" -o BatchMode=yes -o ConnectTimeout=10 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)
else
  BASE=(ssh -o ConnectTimeout=10 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)
  SCPS=(scp -o ConnectTimeout=10 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)
fi

echo "=== Game Dev Story deploy ==="
echo "Host: $HOST"

# --- 1. health + login check (retry; flaky R36S) ---
up=0
for i in 1 2 3 4 5; do
  if "${BASE[@]}" "$HOST" 'echo ok' >/dev/null 2>&1; then
    up=1; break
  fi
  echo "  waiting for $HOST (attempt $i/5)..."
  sleep 3
done
if [ "$up" != "1" ]; then
  echo "!! Cannot SSH to $HOST after retries."
  echo "   Make sure the R36S is ON, on the same network, and SSH is enabled."
  if [ -z "$SSH_KEY" ]; then
    echo "   Tip: set GDS_SSH_KEY=~/.ssh/<key> to avoid password issues."
  fi
  exit 1
fi
echo "✓ Device reachable + login works"

# --- 2. download the port zip ---
echo "Downloading gamedevstory.zip..."
curl -sL --progress-bar -o "$ZIP" "https://github.com/jackomix/asdf/raw/$BRANCH/GDS_Unity/gamedevstory.zip"
echo
if [ ! -s "$ZIP" ] || ! unzip -t "$ZIP" >/dev/null 2>&1; then
  echo "!! Downloaded zip is corrupt/missing. Try again."
  exit 1
fi
echo "✓ zip ready ($(du -h "$ZIP" | cut -f1))"

# --- 3. upload with retries (this is the flaky part) ---
echo "Uploading to $HOST ..."
up=0
for i in 1 2 3 4 5 6 7 8 9 10; do
  echo "  upload attempt $i..."
  if "${SCPS[@]}" "$ZIP" "$HOST:$PORTS_DIR/gamedevstory.zip"; then
    up=1; break
  fi
  echo "  upload failed - retrying in 5s (R36S Wi-Fi is flaky)..."
  sleep 5
done
if [ "$up" != "1" ]; then
  echo "!! Upload failed after 10 tries. Restart the R36S and re-run."
  exit 1
fi
echo "✓ uploaded"

# --- 4. install + run (with retries) ---
echo "Installing and running..."
for i in 1 2 3; do
  if "${BASE[@]}" "$HOST" "
    set -e
    rm -rf '$PORTS_DIR/gamedevstory'
    cd '$PORTS_DIR'
    unzip -o gamedevstory.zip >/dev/null 2>&1
    rm -f gamedevstory.zip
    chmod +x '$PORTS_DIR/Game Dev Story.sh' '$PORTS_DIR/gamedevstory/loader2'
    echo '=== running launcher ==='
    bash '$PORTS_DIR/Game Dev Story.sh'
    echo '=== launcher exited with code '\$?' ==='
  "; then
    break
  fi
  echo "  run failed - retrying in 5s..."
  sleep 5
done

# --- 5. pull logs ---
echo "Pulling logs..."
mkdir -p "$LOGDIR"
"${SCPS[@]}" "$HOST:$PORTS_DIR/port_launch.log" "$LOGDIR/" 2>/dev/null || true
"${SCPS[@]}" "$HOST:$PORTS_DIR/gamedevstory/loader.log" "$LOGDIR/" 2>/dev/null || true
"${SCPS[@]}" "$HOST:/tmp/gamedevstory_loader.log" "$LOGDIR/" 2>/dev/null || true

echo
echo "=== Logs saved to $LOGDIR/ ==="
for f in "$LOGDIR"/*.log; do
  [ -f "$f" ] && echo "----- $(basename "$f") -----" && cat "$f" && echo
done
echo "Done. Send the loader.log output to the developer."

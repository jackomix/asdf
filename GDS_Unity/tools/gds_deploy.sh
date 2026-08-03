#!/usr/bin/env bash
# gds_deploy.sh - ONE script that does EVERYTHING for Game Dev Story on the R36S.
# Handles SSH auth itself using the device password, no keys, no prompts.
#
#   ./gds_deploy.sh [user@host]
#
# Env overrides:
#   GDS_R36S_HOST   SSH host (default: ark@10.1.1.2)
#   GDS_SSH_PASS    device password (default: ark)
#   GDS_PORTS_DIR   ports dir on device (default: /roms/ports)
#   GDS_BRANCH      git branch (default: arena/019fc860-asdf)
set -uo pipefail

HOST="${GDS_R36S_HOST:-${1:-ark@10.1.1.2}}"
PASS="${GDS_SSH_PASS:-ark}"
PORTS_DIR="${GDS_PORTS_DIR:-/roms/ports}"
BRANCH="${GDS_BRANCH:-arena/019fc860-asdf}"
HERE="$(cd "$(dirname "$0")" && pwd)"
LOGDIR="$HERE/gds_logs"
ZIP="$HERE/gamedevstory.zip"

# ---- password plumbing: make ssh/scp use $PASS automatically ----
# Write a tiny askpass script that prints the password, and point ssh at it.
ASKPASS="$HERE/.gds_askpass.sh"
printf '#!/usr/bin/env bash\nprintf "%%s\\n" "%s"\n' "$PASS" > "$ASKPASS"
chmod 700 "$ASKPASS"
export SSH_ASKPASS="$ASKPASS"
export SSH_ASKPASS_REQUIRE=force      # force askpass even with a tty (OpenSSH 8.4+)
unset SSH_AUTH_SOCK                    # ignore any agent; we use password

SSHBASE=(ssh -o ConnectTimeout=12 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o NumberOfPasswordPrompts=1)
SCPBASE=(scp -o ConnectTimeout=12 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)

echo "=== Game Dev Story deploy to $HOST ==="

# ---- 1. health + login check (retries; flaky R36S) ----
up=0
for i in 1 2 3 4 5; do
  if "${SSHBASE[@]}" "$HOST" 'echo ok' >/dev/null 2>&1; then up=1; break; fi
  echo "  waiting for $HOST (attempt $i/5)..."; sleep 3
done
if [ "$up" != "1" ]; then
  echo "!! Cannot SSH to $HOST. Is the R36S on, on the same network, SSH enabled?"
  echo "   If the password isn't 'ark', run with: GDS_SSH_PASS=yourpass $0 $HOST"
  exit 1
fi
echo "✓ Device reachable + login works"

# ---- 2. download the port zip ----
echo "Downloading gamedevstory.zip..."
curl -sL --progress-bar -o "$ZIP" "https://github.com/jackomix/asdf/raw/$BRANCH/GDS_Unity/gamedevstory.zip"
echo
if [ ! -s "$ZIP" ] || ! unzip -t "$ZIP" >/dev/null 2>&1; then
  echo "!! Downloaded zip corrupt/missing. Try again."; exit 1
fi
echo "✓ zip ready ($(du -h "$ZIP" | cut -f1))"

# ---- 3. upload with retries ----
echo "Uploading to $HOST ..."
up=0
for i in 1 2 3 4 5 6 7 8 9 10; do
  echo "  upload attempt $i..."
  if "${SCPBASE[@]}" "$ZIP" "$HOST:$PORTS_DIR/gamedevstory.zip"; then up=1; break; fi
  echo "  upload failed - retrying in 5s..."; sleep 5
done
if [ "$up" != "1" ]; then
  echo "!! Upload failed after 10 tries. Restart the R36S and re-run."; exit 1
fi
echo "✓ uploaded"

# ---- 4. install + run ----
echo "Installing and running..."
"${SSHBASE[@]}" "$HOST" "
  set -e
  rm -rf '$PORTS_DIR/gamedevstory'
  cd '$PORTS_DIR'
  unzip -o gamedevstory.zip >/dev/null 2>&1
  rm -f gamedevstory.zip
  chmod +x '$PORTS_DIR/Game Dev Story.sh' '$PORTS_DIR/gamedevstory/loader2'
  echo '=== running launcher ==='
  bash '$PORTS_DIR/Game Dev Story.sh'
  echo '=== launcher exited with code '\$?' ==='
"

# ---- 5. pull logs ----
echo "Pulling logs..."
mkdir -p "$LOGDIR"
"${SCPBASE[@]}" "$HOST:$PORTS_DIR/port_launch.log" "$LOGDIR/" 2>/dev/null || true
"${SCPBASE[@]}" "$HOST:$PORTS_DIR/gamedevstory/loader.log" "$LOGDIR/" 2>/dev/null || true
"${SCPBASE[@]}" "$HOST:/tmp/gamedevstory_loader.log" "$LOGDIR/" 2>/dev/null || true

echo
echo "=== Logs saved to $LOGDIR/ ==="
for f in "$LOGDIR"/*.log; do
  [ -f "$f" ] && echo "----- $(basename "$f") -----" && cat "$f" && echo
done
rm -f "$ASKPASS"
echo "Done. Send the loader.log output to the developer."

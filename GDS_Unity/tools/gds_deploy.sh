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
#   GDS_BRANCH      git branch (default: arena/019fd2ed-asdf)
#   GDS_RUN_SECONDS how long to let the game boot before the deploy returns
#                   (default: 12).  The deploy never blocks on a running game.
set -uo pipefail

HOST="${GDS_R36S_HOST:-${1:-ark@10.1.1.2}}"
PASS="${GDS_SSH_PASS:-ark}"
PORTS_DIR="${GDS_PORTS_DIR:-/roms/ports}"
BRANCH="${GDS_BRANCH:-arena/019fd2ed-asdf}"
# The loader build version the deployed zip MUST contain.  This baked value is
# ONLY a last-resort fallback: at deploy time the script re-reads the real
# expected version from GDS_Unity/loader_ref/VERSION at the exact commit it
# downloads from (SHA URLs are immutable, so GitHub's raw CDN can never serve
# a stale one).  That closes the old failure mode where a cached copy of THIS
# script validated a stale zip against a stale baked version and everything
# looked "fine".  Set GDS_EXPECT_VER env to force a value manually.
GDS_EXPECT_VER_BAKED="0.82.0-padland"
GDS_EXPECT_VER_ENV="${GDS_EXPECT_VER:-}"
HERE="$(cd "$(dirname "$0")" && pwd)"
LOGDIR="$HERE/gds_logs"
ZIP="$HERE/gamedevstory.zip"
RUN_SECONDS="${GDS_RUN_SECONDS:-12}"

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
# ---- show how fresh the remote build is (from GitHub API) ----
echo "Checking latest build on branch '$BRANCH'..."
GH_TS=$(curl -sL --max-time 15 "https://api.github.com/repos/jackomix/asdf/commits/$BRANCH" 2>/dev/null | python3 -c "import sys,json;d=json.load(sys.stdin);print(d.get('commit',{}).get('committer',{}).get('date',''))" 2>/dev/null || true)
GH_SHA=$(curl -sL --max-time 15 "https://api.github.com/repos/jackomix/asdf/commits/$BRANCH" 2>/dev/null | python3 -c "import sys,json;d=json.load(sys.stdin);print(d.get('sha',''))" 2>/dev/null || true)
if [ -n "$GH_TS" ]; then
  # age in minutes (macOS date -j, Linux date -d)
  NOW=$(date +%s)
  # parse the ISO commit time (Linux 'date -d', macOS 'date -j')
  if date -d "$GH_TS" +%s >/dev/null 2>&1; then TS=$(date -d "$GH_TS" +%s)
  else TS=$(date -j -f "%Y-%m-%dT%H:%M:%SZ" "$GH_TS" +%s 2>/dev/null || echo 0); fi
  if [ "$TS" -gt 0 ] 2>/dev/null; then
    AGE=$(( (NOW - TS) / 60 ))
    if [ "$AGE" -lt 0 ]; then AGE=0; fi
    # Also print the raw UTC commit timestamp so the age isn't ambiguous.
    echo "  latest commit: ${GH_SHA:0:8}  (committed $GH_TS UTC, ~${AGE} minutes ago)"
  else
    echo "  latest commit: ${GH_SHA:0:8}  (commit time unknown: $GH_TS)"
  fi
else
  echo "  (could not check commit time; continuing)"
fi

echo "Downloading gamedevstory.zip..."
# Download by commit SHA (unique URL per commit) so GitHub's raw CDN can't serve
# a cached stale zip for a newer build.  The branch-tip URL (?ts= cache-buster) is
# only a fallback.  This is what kept the device on an old loader2 before.
DOWNLOAD_SHA="${GDS_SHA:-$GH_SHA}"

# ---- learn the real expected loader version from the repo (not from this script) ----
# GDS_Unity/loader_ref/VERSION is stamped by build.sh from the binary itself.
# Fetched via the SAME commit SHA we download the zip from, so script/zip can
# never disagree because of CDN caching of this script.
GDS_EXPECT_VER="$GDS_EXPECT_VER_BAKED"
if [ -n "$GDS_EXPECT_VER_ENV" ]; then
  GDS_EXPECT_VER="$GDS_EXPECT_VER_ENV"
  echo "  expected loader version: $GDS_EXPECT_VER (forced via env)"
elif [ -n "$DOWNLOAD_SHA" ]; then
  FETCHED_VER=$(curl -sL --max-time 15 "https://github.com/jackomix/asdf/raw/$DOWNLOAD_SHA/GDS_Unity/loader_ref/VERSION" 2>/dev/null | head -1 | tr -d '[:space:]' || true)
  if [ -n "$FETCHED_VER" ] && printf '%s' "$FETCHED_VER" | grep -qE "^0\.[0-9]+\.[0-9]+(-[a-z0-9]+)?$"; then
    GDS_EXPECT_VER="$FETCHED_VER"
    echo "  expected loader version: $GDS_EXPECT_VER (from repo @ ${DOWNLOAD_SHA:0:8})"
  else
    echo "  expected loader version: $GDS_EXPECT_VER (baked fallback; VERSION file not fetched)"
  fi
else
  echo "  expected loader version: $GDS_EXPECT_VER (baked fallback; commit SHA unknown)"
fi

ZIP_URL=""
if [ -n "$DOWNLOAD_SHA" ]; then
  ZIP_URL="https://github.com/jackomix/asdf/raw/$DOWNLOAD_SHA/GDS_Unity/gamedevstory.zip"
  echo "  downloading via commit $DOWNLOAD_SHA"
else
  ZIP_URL="https://github.com/jackomix/asdf/raw/$BRANCH/GDS_Unity/gamedevstory.zip?ts=$(date +%s)"
fi
curl -sL --progress-bar -o "$ZIP" "$ZIP_URL"
echo
if [ ! -s "$ZIP" ] || ! unzip -t "$ZIP" >/dev/null 2>&1; then
  echo "!! Downloaded zip corrupt/missing. Try again."; exit 1
fi
# Show the loader version baked into this zip so we can spot a stale/cached zip
GZVER=$(unzip -p "$ZIP" gamedevstory/loader2 2>/dev/null | grep -a -oE "0\.[0-9]+\.[0-9]+(-[a-z0-9]+)?" | head -1 || true)
echo "  zip loader2: build ${GZVER:-version unknown}"
# Hard check: the zip MUST contain the current loader build or the deploy is
# pointless (the device would run stale code again).  Retry with the branch URL.
if [ -z "$GZVER" ] || [ "$GZVER" != "$GDS_EXPECT_VER" ]; then
  echo "!! Downloaded zip has loader build '${GZVER:-none}' but expected '$GDS_EXPECT_VER'."
  echo "   (stale zip) - retrying via branch URL..."
  sleep 2
  curl -sL --progress-bar -o "$ZIP" "https://github.com/jackomix/asdf/raw/$BRANCH/GDS_Unity/gamedevstory.zip?ts=$(date +%s%N)"
  echo
  GZVER=$(unzip -p "$ZIP" gamedevstory/loader2 2>/dev/null | grep -a -oE "0\.[0-9]+\.[0-9]+(-[a-z0-9]+)?" | head -1 || true)
  echo "  retry: zip loader2 build ${GZVER:-unknown}"
  if [ -z "$GZVER" ] || [ "$GZVER" != "$GDS_EXPECT_VER" ]; then
    echo "!! Still got '${GZVER:-none}' (expected $GDS_EXPECT_VER). Aborting."
    echo "   The expected version was read from the repo itself (loader_ref/VERSION),"
    echo "   so this means the zip at that commit is genuinely wrong or not pushed yet."
    echo "   Check that build.sh ran (it stamps VERSION) and the zip was committed together."
    exit 1
  fi
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
  # kill any stale loader2 FIRST: a leftover loader holds the DRM master and
  # makes SDL_CreateWindow fail for the fresh run (seen on-device: stale
  # NullGL loader still printing frames during the next test)
  pkill -9 -x loader2 2>/dev/null || true
  sleep 1
  rm -rf '$PORTS_DIR/gamedevstory'
  cd '$PORTS_DIR'
  unzip -o gamedevstory.zip >/dev/null 2>&1
  rm -f gamedevstory.zip
  chmod +x '$PORTS_DIR/Game Dev Story.sh' '$PORTS_DIR/gamedevstory/loader2'
  echo '=== running launcher (background, '$RUN_SECONDS's) ==='
  nohup bash '$PORTS_DIR/Game Dev Story.sh' >/dev/null 2>&1 &
  for i in 1 2 3; do sleep $RUN_SECONDS; done
  if kill -0 %1 2>/dev/null; then
    echo 'launcher still running (game alive) after '$RUN_SECONDS's'
  else
    echo 'launcher already exited'
  fi
"

# ---- 5. pull logs ----
echo "Pulling logs..."
mkdir -p "$LOGDIR"
"${SCPBASE[@]}" "$HOST:$PORTS_DIR/port_launch.log" "$LOGDIR/" 2>/dev/null || true
"${SCPBASE[@]}" "$HOST:$PORTS_DIR/gamedevstory/loader.log" "$LOGDIR/" 2>/dev/null || true
"${SCPBASE[@]}" "$HOST:/tmp/gamedevstory_loader.log" "$LOGDIR/" 2>/dev/null || true

echo
echo "=== Logs saved to $LOGDIR/ ==="
{
for f in "$LOGDIR"/*.log; do
  [ -f "$f" ] && echo "----- $(basename "$f") -----" && cat "$f" && echo
done
} > /tmp/gds_logs_dump.txt
cat /tmp/gds_logs_dump.txt
# Copy the log text to the clipboard automatically (macOS pbcopy, Linux xclip)
if command -v pbcopy >/dev/null 2>&1; then
  cat /tmp/gds_logs_dump.txt | pbcopy
  echo "   → Logs copied to clipboard. Just paste them to send them!"
elif command -v xclip >/dev/null 2>&1; then
  cat /tmp/gds_logs_dump.txt | xclip -selection clipboard
  echo "   → Logs copied to clipboard. Just paste them to send them!"
elif command -v wl-copy >/dev/null 2>&1; then
  cat /tmp/gds_logs_dump.txt | wl-copy
  echo "   → Logs copied to clipboard. Just paste them to send them!"
else
  echo "   (no clipboard tool found; copy the text above manually)"
fi
rm -f "$ASKPASS" /tmp/gds_logs_dump.txt
echo "Done."

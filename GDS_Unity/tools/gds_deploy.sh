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
GDS_EXPECT_VER_BAKED="0.95.15-osk8"
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
# v2: atomic install (2026-08-09).  v1 wiped the live folder BEFORE unzip;
# a truncated upload or full SD card left the device with an empty game dir.
echo "  deploy script v2 (atomic install, saves-preserving)"

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

# ---- 4. install (ATOMIC v2: stage + verify + swap; never wipes live files) ----
# 0.95.13 device incident: the user re-deployed and the launcher found
# /roms/ports/gamedevstory containing ONLY data/.  Root cause: v1 did
# 'rm -rf gamedevstory' BEFORE 'unzip' -- when the freshly uploaded zip was
# unusable on-device (truncated upload / full card), unzip died after the
# first entries (data/ is first in the zip) and the live folder was already
# gone.  v2: integrity-test the uploaded zip ON THE DEVICE, extract into a
# scratch dir, verify the staged build, THEN swap.  A failure at any step
# aborts with the live folder untouched and prints df so the cause is clear.
echo "Installing (staged + verified; old folder kept until the swap)..."
"${SSHBASE[@]}" "$HOST" "
  set -e
  bail() {
    echo \"!! \$1\"
    echo \"   free space on '$PORTS_DIR':\"
    df -h '$PORTS_DIR' | tail -1
    rm -rf '$PORTS_DIR/.gds_install'
    exit 1
  }
  # kill any stale loader2 FIRST: a leftover loader holds the DRM master and
  # makes SDL_CreateWindow fail for the fresh run (seen on-device: stale
  # NullGL loader still printing frames during the next test)
  pkill -9 -x loader2 2>/dev/null || true
  sleep 1
  cd '$PORTS_DIR'

  # 1) the uploaded zip must be self-consistent BEFORE we touch anything
  unzip -t gamedevstory.zip >/dev/null 2>&1 ||
      bail 'uploaded zip failed integrity test on device -- live folder NOT touched.  Free space on the card and re-run the deploy.'

  # 2) enough room for zip + staged copy + live copy at the same time
  FREE_KB=\$(df -k '$PORTS_DIR' | awk 'NR==2 {print \$4}')
  NEED_KB=200000
  if [ \"\${FREE_KB:-0}\" -lt \$NEED_KB ]; then
      bail \"only \${FREE_KB:-0}KB free (need \${NEED_KB}KB) -- make room on the card (roms) and re-run the deploy\"
  fi

  # 3) stage into a scratch dir next to the target (same fs = instant swap)
  rm -rf .gds_install
  mkdir .gds_install
  unzip -q gamedevstory.zip -d .gds_install ||
      bail 'unzip failed on device -- live folder NOT touched'
  [ -f .gds_install/gamedevstory/loader2 ] ||
      bail 'staged tree missing loader2 -- aborting, live folder NOT touched'
  [ -f .gds_install/gamedevstory/libil2cpp.so ] ||
      bail 'staged tree missing libil2cpp.so -- aborting, live folder NOT touched'
  STAGED_VER=\$(grep -a -oE 'reference-port 0\\.[0-9]+\\.[0-9]+(-[a-z0-9]+)?' .gds_install/gamedevstory/loader2 | head -1 | sed 's/reference-port //')
  [ -n \"\$STAGED_VER\" ] || bail 'staged loader2 has no version banner -- aborting, live folder NOT touched'
  echo \"  staged build verified: \$STAGED_VER\"

  # 4) carry over player state the zip does not ship:
  #    gamedevstory/home = the loader's home dir; RecordStore save slots +
  #    shared-preferences.bin live there (jni.c gds_home).  v1 wiped this on
  #    EVERY deploy -- only unnoticed because playtime was still at the
  #    Company-Name prompt.  Never again.
  if [ -d gamedevstory/home ]; then
      cp -a gamedevstory/home .gds_install/gamedevstory/home &&
          echo '  saves carried over (gamedevstory/home)'
  fi
  if [ -f gamedevstory/gds_env.cfg ]; then
      cp gamedevstory/gds_env.cfg .gds_install/gamedevstory/gds_env.cfg &&
          echo '  gds_env.cfg knobs carried over'
  fi

  # 5) zip is verified + extracted: drop it before the swap to keep peak low
  rm -f gamedevstory.zip

  # 6) swap live <-> staged; keep the old tree as .old until the new one
  #    is in place, so even here nothing is lost if the rename fails
  rm -rf gamedevstory.old
  [ -d gamedevstory ] && mv gamedevstory gamedevstory.old || true
  mv .gds_install/gamedevstory gamedevstory ||
      bail 'rename of staged tree failed -- previous tree kept at gamedevstory.old'
  rm -rf .gds_install gamedevstory.old
  # the launcher wrapper is NOT in the zip (one-time manual setup): a
  # missing wrapper must not fail the install AFTER the swap succeeded
  chmod +x '$PORTS_DIR/Game Dev Story.sh' 2>/dev/null || true
  chmod +x '$PORTS_DIR/gamedevstory/loader2'
  # 0.89: no auto-launch (user request) -- install only; launch is done
  # from the EmulationStation Ports menu.
  echo \"=== install complete (\$STAGED_VER); launch from the Ports menu ===\"
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

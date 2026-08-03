#!/usr/bin/env bash
# gds_deploy.sh - one-command deploy + test + log-pull for Game Dev Story on an
# R36S over SSH.  Run from your PC.  NO repo checkout needed - it downloads the
# pre-built gamedevstory.zip from GitHub, installs it, runs it, pulls the log.
#
# Includes: device health check, progress bars, step status, and timeouts so a
# flaky R36S Wi-Fi never leaves you staring at a frozen "uploading".
#
# Usage:
#   ./gds_deploy.sh [user@host]
#   GDS_R36S_HOST=user@host ./gds_deploy.sh          (works with bash -c one-liner)
#
# Env overrides:
#   GDS_R36S_HOST   SSH host (default: ark@10.1.1.2)
#   GDS_PORTS_DIR   ports dir on device (default: /roms/ports)
#   GDS_BRANCH      git branch (default: arena/019fc860-asdf)
#   GDS_ZIP_URL     full URL/path to a gamedevstory.zip
#   GDS_SSH_TIMEOUT ssh/scp timeout in seconds (default: 25)
#
set -uo pipefail

HOST="${GDS_R36S_HOST:-${1:-ark@10.1.1.2}}"
PORTS_DIR="${GDS_PORTS_DIR:-/roms/ports}"
BRANCH="${GDS_BRANCH:-arena/019fc860-asdf}"
TIMEOUT="${GDS_SSH_TIMEOUT:-25}"
HERE="$(cd "$(dirname "$0")" && pwd)"
LOGDIR="$HERE/gds_logs"

# ---- pretty helpers (color if tty) ----
if [ -t 1 ]; then C_GREEN=$'\e[32m'; C_CYAN=$'\e[36m'; C_YEL=$'\e[33m'; C_RED=$'\e[31m'; C_DIM=$'\e[2m'; C_BOLD=$'\e[1m'; C_RST=$'\e[0m'; else C_GREEN= C_CYAN= C_YEL= C_RED= C_DIM= C_BOLD= C_RST=; fi

step() { echo -e "${C_CYAN}──▶${C_RST} ${C_BOLD}$1${C_RST}"; }
ok()   { echo -e "   ${C_GREEN}✓${C_RST} $1"; }
info() { echo -e "   ${C_DIM}$1${C_RST}"; }
warn() { echo -e "   ${C_YEL}!${C_RST} $1"; }
fail() { echo -e "   ${C_RED}✗ $1${C_RST}"; }

# ---- tiny animated spinner ----
spin() {
  local pid=$1 msg=$2 i=0 chars='⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏'
  while kill -0 "$pid" 2>/dev/null; do
    printf "\r   ${C_CYAN}%s${C_RST} %s " "${chars:i:1}" "$msg"
    i=$(( (i+1) % ${#chars} )); sleep 0.15
  done
  printf "\r%*s\r" "$((${#msg}+8))" ""
}

# ---- 1. device health check (fast) ----
step "Checking R36S connectivity: $HOST"
if ! timeout "$TIMEOUT" ssh -o BatchMode=yes -o ConnectTimeout=8 "$HOST" 'echo ok' >/dev/null 2>&1; then
  fail "Cannot reach $HOST over SSH."
  echo
  echo "  ${C_YEL}This usually means the R36S is asleep / off / Wi-Fi dropped.${C_RST}"
  echo "  ${C_YEL}On the R36S:${C_RST}"
  echo "    • Wake it up and make sure it's on the same network"
  echo "    • If Wi-Fi is flaky, restart the R36S, then re-run this script"
  echo "    • Confirm SSH is enabled in ArkOS settings"
  echo
  exit 1
fi
ok "Device is online and reachable"

# ---- 2. get the port zip ----
step "Fetching gamedevstory.zip"
ZIP=""
if [ -n "${GDS_ZIP_URL:-}" ] && [ -f "$GDS_ZIP_URL" ]; then
  ZIP="$GDS_ZIP_URL"; ok "using local $ZIP"
elif [ -n "${GDS_ZIP_URL:-}" ]; then
  ZIP="$HERE/gamedevstory.zip"
  echo "   downloading..."
  curl -sL --progress-bar -o "$ZIP" "$GDS_ZIP_URL"
  echo
  ok "downloaded from $GDS_ZIP_URL"
else
  URL="https://github.com/jackomix/asdf/raw/$BRANCH/GDS_Unity/gamedevstory.zip"
  ZIP="$HERE/gamedevstory.zip"
  echo "   downloading..."
  curl -sL --progress-bar -o "$ZIP" "$URL"
  echo
  ok "downloaded $ZIP"
fi

if [ ! -s "$ZIP" ] || ! unzip -t "$ZIP" >/dev/null 2>&1; then
  fail "gamedevstory.zip is missing or corrupt. Try again."
  exit 1
fi
SIZE=$(du -h "$ZIP" | cut -f1)
ok "zip ready ($SIZE)"

# ---- 3. upload ----
step "Uploading to $HOST:$PORTS_DIR  (this is the big 38 MB transfer)"
if command -v pv >/dev/null 2>&1; then
  # pv shows a real progress bar
  pv -f -N "scp" "$ZIP" | timeout "$TIMEOUT" scp -o BatchMode=yes "$ZIP" "$HOST:$PORTS_DIR/gamedevstory.zip" 2>/dev/null
  echo
else
  # scp has its own progress bar; run it with a timeout and show a note
  if ! timeout "$TIMEOUT" scp -o BatchMode=yes "$ZIP" "$HOST:$PORTS_DIR/gamedevstory.zip" 2>/dev/null; then
    fail "Upload failed/timed out. The R36S may have gone offline."
    echo "  ${C_YEL}Tip: restart the R36S and re-run.${C_RST}"
    exit 1
  fi
fi
ok "uploaded"

# ---- 4. install + run ----
step "Installing and running on the device"
if ! timeout "$TIMEOUT" ssh -o BatchMode=yes "$HOST" "
  set -e
  rm -rf '$PORTS_DIR/gamedevstory'
  cd '$PORTS_DIR'
  unzip -o gamedevstory.zip >/dev/null 2>&1
  rm -f gamedevstory.zip
  chmod +x '$PORTS_DIR/Game Dev Story.sh' '$PORTS_DIR/gamedevstory/loader2'
  echo '=== running launcher ==='
  bash '$PORTS_DIR/Game Dev Story.sh'
  echo '=== launcher exited with code '\$?' ==='
" 2>&1; then
  fail "Install/run failed. Device may have gone offline mid-run."
  exit 1
fi

# ---- 5. pull logs ----
step "Pulling logs"
mkdir -p "$LOGDIR"
rm -f "$LOGDIR"/port_launch.log "$LOGDIR"/loader.log "$LOGDIR"/gamedevstory_loader.log
timeout "$TIMEOUT" scp -o BatchMode=yes "$HOST:$PORTS_DIR/port_launch.log" "$LOGDIR/" 2>/dev/null || true
timeout "$TIMEOUT" scp -o BatchMode=yes "$HOST:$PORTS_DIR/gamedevstory/loader.log" "$LOGDIR/" 2>/dev/null || true
timeout "$TIMEOUT" scp -o BatchMode=yes "$HOST:/tmp/gamedevstory_loader.log" "$LOGDIR/" 2>/dev/null || true

echo
echo "=== Logs saved to $LOGDIR/ ==="
for f in "$LOGDIR"/*.log; do
  [ -f "$f" ] && echo "----- $(basename "$f") -----" && cat "$f" && echo
done
echo -e "${C_GREEN}Done.${C_RST} Send the loader.log output to the developer."

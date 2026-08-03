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
#   GDS_XFER_TIMEOUT upload timeout in seconds (default: 300; 38 MB over slow
#                     handheld Wi-Fi can take a while)
#
set -uo pipefail

HOST="${GDS_R36S_HOST:-${1:-ark@10.1.1.2}}"
PORTS_DIR="${GDS_PORTS_DIR:-/roms/ports}"
BRANCH="${GDS_BRANCH:-arena/019fc860-asdf}"
TIMEOUT="${GDS_SSH_TIMEOUT:-25}"
XFER_TIMEOUT="${GDS_XFER_TIMEOUT:-300}"
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

# ---- 1. device health check (TCP port 22 reachability, no auth needed) ----
# We deliberately do NOT use `ssh` for the health check: it requires
# authentication and `-o BatchMode=yes` would block password logins and falsely
# report "offline".  Instead we just check that the R36S answers on port 22,
# which works whether you use a password or an SSH key.  Then the real
# scp/ssh steps below use your normal login.
step "Checking R36S connectivity: $HOST"

# extract host/ip (strip any user@)
HC_HOST="${HOST##*@}"

# use /dev/tcp (bash builtin) if possible, else fall back to nc
tcp_ok() {
  if command -v nc >/dev/null 2>&1; then
    nc -z -w 5 "$HC_HOST" 22 >/dev/null 2>&1
  else
    timeout 6 bash -c "exec 3<>/dev/tcp/$HC_HOST/22" >/dev/null 2>&1
  fi
}

alive=0
for attempt in 1 2 3; do
  if tcp_ok; then alive=1; break; fi
  if [ "$attempt" -lt 3 ]; then
    warn "port 22 not answering (attempt $attempt/3) - retrying..."
    sleep 2
  fi
done

if [ "$alive" != "1" ]; then
  # Give an immediate reason: ping to see if it's a route issue vs. port issue
  PINGREASON=""
  if command -v ping >/dev/null 2>&1; then
    if ping -c 1 -W 2 "$HC_HOST" >/dev/null 2>&1; then
      PINGREASON="device ANSWERS ping (so it's reachable, but SSH/port 22 isn't open or is filtered)"
    else
      PINGREASON="device does NOT answer ping (no route / different subnet / device off)"
    fi
  fi
  fail "Cannot reach $HC_HOST:22 (SSH)."
  [ -n "$PINGREASON" ] && echo "  ${C_DIM}Diagnostic: $PINGREASON${C_RST}"
  echo
  echo "  ${C_YEL}This usually means the R36S is asleep / off / Wi-Fi dropped,${C_RST}"
  echo "  ${C_YEL}or it needs a restart because the Wi-Fi went flaky.${C_RST}"
  echo "  On the R36S:"
  echo "    • Wake it up, confirm it's on the same network as this PC"
  echo "    • If the Wi-Fi dropped, restart the R36S and re-run this script"
  echo "    • Make sure SSH is enabled in ArkOS settings"
  echo
  echo "  Diagnostics (run these from your PC to pinpoint it):"
  if command -v ping >/dev/null 2>&1; then
    echo "    ping -c 3 $HC_HOST      # can we reach it at all?"
  fi
  echo "    nc -vz $HC_HOST 22   # is SSH (port 22) open? (or: nc -vz $HC_HOST 22)"
  echo "    ssh ${HOST} 'echo hi'      # does SSH login itself work?"
  echo
  echo "  Common gotcha: if ping fails but the R36S is on, the two devices are"
  echo "  probably on DIFFERENT networks/subnets. Check the R36S IP under"
  echo "  ArkOS Settings -> Network, and make sure your Mac is on the same LAN."
  echo
  exit 1
fi
ok "Device is online and reachable"

# SSH options used by later steps: NOT BatchMode (so password login works),
# but tolerant of ArkOS host-key changes.
SSHOPTS=(-o ConnectTimeout=10 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ServerAliveInterval=5)

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

# ---- 3. upload (retry loop - the R36S's flaky Wi-Fi / changing host key can
#      make scp fail on the first attempt; keep retrying until it succeeds or
#      the overall timeout runs out) ----
step "Uploading to $HOST:$PORTS_DIR  (this is the big 38 MB transfer)"
echo "   (be patient - 38 MB over handheld Wi-Fi can take a minute or two;"
echo "    scp shows a progress bar below. A failed attempt is retried"
echo "    automatically up to ${XFER_TIMEOUT}s.)"

SCP_BASE="scp -o ConnectTimeout=15 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
start=$(date +%s)
uploaded=0
while [ $(( $(date +%s) - start )) -lt "$XFER_TIMEOUT" ]; do
  if $SCP_BASE "$ZIP" "$HOST:$PORTS_DIR/gamedevstory.zip"; then
    uploaded=1; break
  fi
  warn "scp failed - retrying in 3s..."
  sleep 3
done
if [ "$uploaded" != "1" ]; then
  fail "Upload failed after retries. The R36S Wi-Fi may be too flaky."
  echo "  ${C_YEL}Tip: restart the R36S and re-run, or use a wired/USB network.${C_RST}"
  exit 1
fi
ok "uploaded"

# ---- 4. install + run ----
step "Installing and running on the device"
if ! timeout "$TIMEOUT" ssh "${SSHOPTS[@]}" "$HOST" "
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
timeout "$TIMEOUT" scp -o ConnectTimeout=10 -o StrictHostKeyChecking=accept-new "$HOST:$PORTS_DIR/port_launch.log" "$LOGDIR/" 2>/dev/null || true
timeout "$TIMEOUT" scp -o ConnectTimeout=10 -o StrictHostKeyChecking=accept-new "$HOST:$PORTS_DIR/gamedevstory/loader.log" "$LOGDIR/" 2>/dev/null || true
timeout "$TIMEOUT" scp -o ConnectTimeout=10 -o StrictHostKeyChecking=accept-new "$HOST:/tmp/gamedevstory_loader.log" "$LOGDIR/" 2>/dev/null || true

echo
echo "=== Logs saved to $LOGDIR/ ==="
for f in "$LOGDIR"/*.log; do
  [ -f "$f" ] && echo "----- $(basename "$f") -----" && cat "$f" && echo
done
echo -e "${C_GREEN}Done.${C_RST} Send the loader.log output to the developer."

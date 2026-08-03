#!/bin/bash
# Game Dev Story - PortMaster launcher for the R36S.
# This script is deliberately robust: it writes its OWN log file immediately
# (before touching the game), fixes permissions, verifies the binary exists,
# and only then runs the game.  So even if something fails you get a log.

# --- Where this port lives ---
SCRIPTDIR="$(cd "$(dirname "$0")" && pwd)"
# The game binary is in the inner gamedevstory/ folder next to this script.
GAMEDIR="$SCRIPTDIR/gamedevstory"

# --- Log EVERYTHING this script does, at the port root (easy to find) ---
LOG="$SCRIPTDIR/port_launch.log"
{
echo "=== Game Dev Story launcher started $(date) ==="
echo "script: $0"
echo "script dir: $SCRIPTDIR"
echo "game dir: $GAMEDIR"
echo "whoami: $(id -un 2>/dev/null || echo unknown)"
} > "$LOG" 2>&1

# --- Fix permissions (vfat SD cards don't store exec bits) ---
{
echo "--- fixing permissions ---"
chmod +x "$SCRIPTDIR/Game Dev Story.sh" 2>&1
chmod +x "$GAMEDIR/loader2" 2>&1
chmod 755 "$GAMEDIR"/libil2cpp.so "$GAMEDIR"/libunity.so "$GAMEDIR"/libmain.so 2>&1
ls -la "$GAMEDIR" 2>&1
} >> "$LOG" 2>&1

# --- Verify the game binary + libs are present ---
{
echo "--- checking files ---"
if [ ! -x "$GAMEDIR/loader2" ]; then
  echo "FATAL: loader2 not found or not executable at $GAMEDIR/loader2"
  ls -la "$GAMEDIR" 2>&1
  echo "Put the contents of the gamedevstory/ inner folder here."
  exit 1
fi
for lib in libil2cpp.so libunity.so libmain.so; do
  if [ ! -f "$GAMEDIR/$lib" ]; then
    echo "WARNING: missing $lib"
  fi
done
if [ ! -d "$GAMEDIR/data" ]; then
  echo "WARNING: missing data/ folder"
fi
echo "--- all checks done, launching ---"
} >> "$LOG" 2>&1

# --- Make the firmware's graphics libs findable ---
# Terraria's launcher does this: SDL2, libEGL/libGLESv2 (Mali) and libz live in
# these dirs on ArkOS, and our loader dlopens them at runtime (kv_egl_dlopen +
# SDL_Init).  Without LD_LIBRARY_PATH, SDL_Init(VIDEO)/SDL_CreateWindow can't
# find them -> "Can't load EGL/GL library on window creation" -> no GL context.
firmware_libs="/usr/local/lib/aarch64-linux-gnu:/usr/local/lib:/usr/lib/aarch64-linux-gnu:/lib/aarch64-linux-gnu:/usr/lib:/lib"
export LD_LIBRARY_PATH="$firmware_libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export TER_GAMEDIR="$GAMEDIR"

# --- Run the game.  loader2 self-logs to loader.log next to itself. ---
cd "$GAMEDIR" || { echo "cannot cd $GAMEDIR" >> "$LOG"; exit 1; }

{
echo "=== launching ./loader2 from $(pwd) ==="
echo "LD_LIBRARY_PATH=$LD_LIBRARY_PATH"
./loader2
echo "=== loader2 exited with code $? ==="
} >> "$LOG" 2>&1

echo "=== done. See $LOG for details ==="
sleep 2
printf "\033c" >> /dev/tty1 2>/dev/null
exit 0

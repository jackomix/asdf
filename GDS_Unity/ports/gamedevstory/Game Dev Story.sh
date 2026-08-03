#!/bin/bash
# Game Dev Story - PortMaster launcher for the R36S.
# Runs loader2, which loads libil2cpp/libunity/libmain and boots the game.
# loader2 writes its own loader.log next to itself (in gamedevstory/).

# --- Locate the game directory robustly (works with or without PortMaster) ---
GAMEDIR="$(cd "$(dirname "$0")" && pwd)"
GAMEDIR="$GAMEDIR/gamedevstory"
cd "$GAMEDIR" || { echo "cannot cd to $GAMEDIR" >&2; sleep 5; exit 1; }

# --- Use PortMaster's control/input setup if available, else carry on ---
controlfolder=""
if [ -d "/opt/system/Tools/PortMaster" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/roms/ports/PortMaster" ]; then
  controlfolder="/roms/ports/PortMaster"
elif [ -d "/storage/roms/ports/PortMaster" ]; then
  controlfolder="/storage/roms/ports/PortMaster"
fi
if [ -n "$controlfolder" ] && [ -f "$controlfolder/control.txt" ]; then
  source "$controlfolder/control.txt"
  [ -f "$controlfolder/mod_${CFW_NAME}.txt" ] && source "$controlfolder/mod_${CFW_NAME}.txt"
  get_controls 2>/dev/null
fi

# --- Ensure uinput is usable for future input mapping ---
chmod 666 /dev/uinput 2>/dev/null

echo "=== Game Dev Story: launching loader2 from $GAMEDIR ==="
echo "=== This also writes loader.log in $GAMEDIR ==="
./loader2 2>&1

echo "=== Game Dev Story exited (code $?) ==="
echo "If it crashed, send $GAMEDIR/loader.log to the developer."
sleep 3
printf "\033c" >> /dev/tty1 2>/dev/null
exit 0

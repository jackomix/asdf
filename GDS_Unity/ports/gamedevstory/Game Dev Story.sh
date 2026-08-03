#!/bin/bash

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source "$controlfolder/control.txt"
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"

get_controls

GAMEDIR="/$directory/ports/gamedevstory"
CONFDIR="$GAMEDIR/conf/"

mkdir -p "$CONFDIR"
cd "$GAMEDIR"

> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

export XDG_DATA_HOME="$CONFDIR"

# The game files (loader + libs + data) live in the gamedevstory/ subdir,
# matching the PortMaster convention of a port folder containing its files.
cd "$GAMEDIR/gamedevstory"

export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

# --- Launch ---
# Game Dev Story is Android/Unity IL2CPP.  loader2 is our native aarch64 loader
# that supplies the Android-underneath (bionic shims, JNI, EGL stubs) and runs
# the game's ARM64 machine code natively on the R36S.
$ESUDO chmod 666 /dev/uinput 2>/dev/null
./loader2 2>&1 | tee -a "$GAMEDIR/log.txt"

# Clean up input helpers if we use them (placeholder until input shim is done)
pm_finish
printf "\033c" >> /dev/tty1

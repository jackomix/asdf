#!/bin/sh
# Run Game Dev Story on the R36S via the native loader.
# Place this script + loader2 + libil2cpp.so + libunity.so + libmain.so + data/
# in one folder (e.g. /roms/ports/gamedevstory/).  Run from that folder.
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"
echo "=== GDS loader start $(date) ===" > loader.log
./loader2 2>>loader.log 1>>loader.log
echo "exit code: $?" >> loader.log
echo "=== log written to $DIR/loader.log ==="

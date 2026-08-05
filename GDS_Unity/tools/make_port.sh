#!/usr/bin/env bash
# Build the PortMaster package for Game Dev Story from the extracted APK.
#
# Requires: GDS_Unity/out/apk (from tools/extract_apk.sh) and loader/loader2
# (from loader/build.sh).  Produces GDS_Unity/GameDevStory_PortMaster.zip -
# extract its contents into /roms/ports/ on the R36S and it shows up in
# EmulationStation under Ports.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
cd "$here"

APKROOT="${1:-$here/out/apk}"
[ -d "$APKROOT/lib/arm64-v8a" ] || { echo "missing $APKROOT/lib/arm64-v8a - run tools/extract_apk.sh first"; exit 1; }
[ -f loader_ref/loader2 ] || { echo "missing loader_ref/loader2 - run bash loader_ref/build.sh first"; exit 1; }

PD="ports/gamedevstory/gamedevstory"
mkdir -p "$PD/licenses"
cp loader_ref/loader2 "$PD/loader2"
cp loader_ref/loader2 "loader/loader2_glibc"
cp "$APKROOT/lib/arm64-v8a/libil2cpp.so" \
   "$APKROOT/lib/arm64-v8a/libunity.so" \
   "$APKROOT/lib/arm64-v8a/libmain.so" "$PD/"
rm -rf "$PD/data"
cp -r "$APKROOT/assets/bin/Data" "$PD/data"
grep -q "job-worker-count" "$PD/data/boot.config" 2>/dev/null || printf "job-worker-count=0\nbackground-job-worker-count=0\n" >> "$PD/data/boot.config"
chmod +x "$PD/loader2" "ports/gamedevstory/Game Dev Story.sh"

rm -f GameDevStory_PortMaster.zip gamedevstory.zip
# Zip the CONTENTS of ports/gamedevstory (not the gamedevstory/ parent dir),
# so the resulting zip extracts flat: ./Game Dev Story.sh, ./gamedevstory/loader2,
# ...  The deploy's version-check greps `gamedevstory/loader2` (flat path), and
# the launcher extracts straight into /roms/ports/, so a nested layout breaks
# both.  Previous commits zipped contents via `cd ports/gamedevstory && zip`.
( cd ports/gamedevstory && zip -q -r -y "$here/GameDevStory_PortMaster.zip" . )
( cd ports/gamedevstory && zip -q -r -y "$here/gamedevstory.zip" . )
echo "built GameDevStory_PortMaster.zip and gamedevstory.zip (0.60.2-ref)"
echo "extract into /roms/ports/ on the R36S, then EmulationStation -> Ports -> Game Dev Story"

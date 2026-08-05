#!/usr/bin/env bash
# Unpack the shipped APK into out/apk (nothing from the APK is committed).
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
apk="${1:-$here/../apks/Game+Dev+Story_2.6.9.apk}"
dst="$here/out/apk"
rm -rf "$dst"; mkdir -p "$dst"
unzip -q -o "$apk" -d "$dst"
echo "extracted $(basename "$apk") -> $dst"
ls -l "$dst/lib/arm64-v8a/" "$dst/assets/bin/Data/Managed/Metadata/"

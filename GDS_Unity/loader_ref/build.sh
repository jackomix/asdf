#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
python3 -m ziglang cc -target aarch64-linux-gnu -O0 -fno-sanitize=undefined \
  -Wno-incompatible-function-pointer-types -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast \
  -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable \
  -Wno-deprecated-declarations -Wno-unknown-warning-option \
  -I . \
  main.c nx_elf.c bionic.c pthread_bridge.c android.c gds_egl.c egl_shim.c input.c audio.c jni.c zlib_stub.c gds_fs.c \
  -o loader2 -ldl -rdynamic
# Stamp the version marker file the deploy script reads.  The version is
# extracted from the binary itself (same regex the deploy uses), so this file
# can NEVER disagree with the loader2 it ships with.
VER=$(grep -a -oE "0\.[0-9]+\.[0-9]+(-[a-z0-9]+)?" loader2 | head -1)
if [ -z "${VER}" ]; then echo "!! could not extract version from loader2"; exit 1; fi
printf '%s\n' "${VER}" > VERSION
echo "=== built (${VER}) ==="

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
mkdir -p ../loader ../ports/gamedevstory/gamedevstory
cp -f loader2 ../loader/loader2_glibc
cp -f loader2 ../ports/gamedevstory/gamedevstory/loader2
echo "=== built loader2 (0.60.3-ref) ==="

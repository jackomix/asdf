#!/usr/bin/env bash
# Build the glibc-linked Game Dev Story loader (PATH 2 - the R36S graphics build).
#
# Unlike the freestanding loader (build.sh), this links against glibc + libdl so
# it can dlopen the REAL Mali GPU drivers (libEGL/libGLESv2) on the R36S.  That
# is what lets Unity's nativeRecreateGfxState create a real GL context.
#
# Requires: ziglang pip package.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
cd "$here"
python3 -m ziglang cc -target aarch64-linux-gnu -O0 -fno-sanitize=undefined \
    -Wno-incompatible-function-pointer-types -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast \
    -DKV_USE_GLIBC \
    -I . \
    loader_glibc_main.c jni_shim.c host_syms.c glibc_shims.c egl_shim.c bionic_bridge.c fs_redirect.c \
    -o loader2_glibc -ldl -rdynamic
echo "built loader/loader2_glibc"

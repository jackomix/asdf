#!/usr/bin/env bash
# Build the freestanding aarch64 loader (R36S native port).
# jni_shim.c is compiled in; the -Wno-* flags relax pedantic C checks that are
# irrelevant for a JNI function-pointer shim (ARM64 ABI is what matters).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
cd "$here"
python3 -m ziglang cc -target aarch64-linux-gnu -ffreestanding -nostdlib \
    -fno-pic -fno-pie -O0 -fno-sanitize=undefined \
    -Wno-incompatible-function-pointer-types -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast \
    loader.c freestdlib.c host_syms.c jni_shim.c -o loader2
echo "built loader/loader2"

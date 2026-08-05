#!/usr/bin/env bash
# Build the glibc-linked Game Dev Story loader (PATH 2 - the R36S graphics build).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
cd "$here/../loader_ref"
bash build.sh


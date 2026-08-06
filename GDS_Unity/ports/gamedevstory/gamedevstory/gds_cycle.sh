#!/bin/bash
# GDS experiment cycler (0.76) -- runs 4 display-experiments back-to-back.
#
# For each experiment you will see:
#   1) a black frame, then a solid COLOR TAG (~1.2s): which experiment is live
#   2) the game running (~16s) under that experiment's settings
#   3) then the next experiment
#
# EXPERIMENTS / TAGS:
#   RED    = defaults (raw contexts + SDL present + surface-config parity)
#   GREEN  = defaults + GDS_RTFLASH=1 (extra probes at ~1s magenta,
#            ~2s yellow, ~3s cyan -- note WHICH of those appear)
#   BLUE   = GDS_CTXMODEL=sdl (Horizon Chase's shipped KMSDRM model)
#   WHITE  = GDS_PRESENT=raw (raw swap route with parity config)
#
# What to report: for each tag color -- did the GAME show pixels after it
# (logo / title / anything but black)? During GREEN, which of magenta/yellow/
# cyan flashed?  Then send: port_launch.log (has a digest per experiment)
#
# Enable:  create the file gds_cycle.enable next to this script
# Disable: delete that file (and this script leavies gds_env.cfg cleaned)
GAMEDIR="$(cd "$(dirname "$0")" && pwd)"
cd "$GAMEDIR" || exit 1

EXP_SECONDS="${GDS_CYCLE_SECONDS:-18}"

run_exp() {
  local n="$1" tag="$2"; shift 2
  echo ""
  echo "################ EXPERIMENT $n (tag: $tag) ################"
  echo "# env: $*"
  {
    echo "GDS_TAGCOLOR=$tag"
    for kv in "$@"; do echo "$kv"; done
  } > gds_env.cfg
  sed 's/^/#   cfg: /' gds_env.cfg
  rm -f "loader_cycle_$n.log"
  timeout -s KILL "$EXP_SECONDS" ./loader2 </dev/null >"loader_cycle_$n.log" 2>&1
  echo "# experiment $n finished (rc=$?). key lines follow:"
  grep -aE "TAG FLASH|theories:|== WINDOW|parity|CONTRACT fallback|window-matching|\[rtf\]|raw ctx id=|worker ctx id=|r_eglCreateContext|SwapBuffers\(real|Invalid texture|signal [0-9]|GL_VERSION=|refresh" "loader_cycle_$n.log" 2>/dev/null | head -70 | sed 's/^/#   /'
  echo "################ END EXPERIMENT $n ################"
  sync
  sleep 2
}

# A stale loader holds the DRM master; window creation would fail.
pkill -9 -x loader2 2>/dev/null && sleep 1

run_exp 1 RED
run_exp 2 GREEN GDS_RTFLASH=1
run_exp 3 BLUE GDS_CTXMODEL=sdl
run_exp 4 WHITE GDS_PRESENT=raw

rm -f gds_env.cfg
echo ""
echo "==== CYCLE DONE (gds_env.cfg cleaned; defaults restored) ===="
echo "==== per-experiment logs: $GAMEDIR/loader_cycle_[1-4].log ===="

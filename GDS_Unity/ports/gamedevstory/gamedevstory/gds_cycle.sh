#!/bin/bash
# GDS experiment cycler (0.77) -- runs 3 display-experiments back-to-back.
#
# For each experiment you will see:
#   1) a black frame, then a solid COLOR TAG (~1.2s): which experiment is live
#   2) the game running (~16s) under that experiment's settings
#   3) then the next experiment
#
# EXPERIMENTS / TAGS:
#   RED    = defaults + GDS_RTFLASH=1 probes:
#              ~1s  MAGENTA (raw ctx draw + route swap -- expected INVISIBLE)
#              ~2s  ORANGE  (raw ctx draw, swap under share root, no redraw)
#              ~3s  CYAN    (raw ctx bound THROUGH SDL, draw+swap)
#            NOTE WHICH of magenta/orange/cyan appear!
#   GREEN  = GDS_PRESENT=shrswap (steady-state: draw raw, swap under
#            share root every frame) -- the compose-test fix, if f120 works
#   BLUE   = GDS_CTXMODEL=sdl + GDS_CLAMPGL=1 (Horizon's KMSDRM model with
#            clamped GL limits so the ES1.1 crash path may survive)
#
# What to report: after which tag did the GAME show pixels (logo/title/blue
# columns -- anything not black)? During RED: which of magenta/orange/cyan
# flashed?  Then send: port_launch.log (has a digest per experiment)
#
# Enable:  create the file gds_cycle.enable next to this script
# Disable: delete that file (cycler leaves gds_env.cfg cleaned up)
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
  grep -aE "TAG FLASH|theories:|== WINDOW|parity|CONTRACT fallback|window-matching|\[rtf\]|CLAMPGL|raw ctx id=|worker ctx id=|r_eglCreateContext|SwapBuffers\(real|Invalid texture|signal [0-9]|GL_VERSION=|refresh" "loader_cycle_$n.log" 2>/dev/null | head -70 | sed 's/^/#   /'
  echo "################ END EXPERIMENT $n ################"
  sync
  sleep 2
}

# A stale loader holds the DRM master; window creation would fail.
pkill -9 -x loader2 2>/dev/null && sleep 1

run_exp 1 RED GDS_RTFLASH=1
run_exp 2 GREEN GDS_PRESENT=shrswap
run_exp 3 BLUE GDS_CTXMODEL=sdl GDS_CLAMPGL=1

rm -f gds_env.cfg
echo ""
echo "==== CYCLE DONE (gds_env.cfg cleaned; defaults restored) ===="
echo "==== per-experiment logs: $GAMEDIR/loader_cycle_[1-3].log ===="

#!/bin/bash
# Run Wonder on the Mac with a quality preset. usage: tools/wonder_mac.sh [quality|balanced|performance] [build dir]
#   quality      docked 1080p, plain scaling to the window (the GPU needs ~13 ms/frame on an M1: little headroom)
#   balanced     the game renders 720p (handheld mode) and FSR 1 upscales to the window (GPU ~5 ms/frame)  [default]
#   performance  as balanced with softer FSR sharpening (NX_FSR_SHARP=1.0): less shimmer on fine detail
# Extra environment still applies (NX_FSR_SHARP=0..2 stops, 0 = sharpest; NX_MEM_LIMIT_GB; NX_LOG).
cd "$(dirname "$0")/.." || exit 1
MODE=${1:-balanced}
BUILD=${2:-private/wonder/build-w}
[ -x "$BUILD/wonder" ] || BUILD=private/wonder/build
case "$MODE" in
  quality) ;;
  balanced) export NX_HANDHELD=1 NX_FSR=1 ;;
  performance) export NX_HANDHELD=1 NX_FSR=1 NX_FSR_SHARP=${NX_FSR_SHARP:-1.0} ;;
  *) echo "unknown preset: $MODE"; exit 1 ;;
esac
export NX_LOG=${NX_LOG:-private/wonder/nx_play.log}
exec "$BUILD/wonder"

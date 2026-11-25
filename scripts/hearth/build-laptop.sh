#!/usr/bin/env bash
# Hearth Engine: configure and build on a low-memory laptop (tested target: Intel i3-N305, 8 GB RAM).
#
#   scripts/hearth/build-laptop.sh [project path]
#
# - Engine-centric build: only the gems the project turns on (plus what the editor needs) get built.
# - Profile configuration only (no debug build: saves tens of GB of disk and hours of compile time).
# - Few parallel jobs and low CPU/IO priority, so the machine stays usable and never runs out of memory.
# - ccache, if installed, makes later rebuilds much faster.
set -euo pipefail
ENGINE="$(cd "$(dirname "$0")/../.." && pwd)"
PROJECT="${1:-$HOME/O3DE/Projects/NoclipProtocol}"
BUILD="$ENGINE/build/linux"
JOBS="${HEARTH_JOBS:-3}"
export CC="${CC:-clang}" CXX="${CXX:-clang++}"
LAUNCHER=()
if command -v ccache >/dev/null; then LAUNCHER=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache); fi

cmake -S "$ENGINE" -B "$BUILD" -G "Ninja Multi-Config" \
  -DLY_3RDPARTY_PATH="$HOME/.o3de/3rdParty" \
  -DLY_PROJECTS="$PROJECT" \
  -DLY_UNITY_BUILD=ON \
  -DLY_STRIP_DEBUG_SYMBOLS=ON \
  "${LAUNCHER[@]}"

PROJECT_NAME="$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['project_name'])" "$PROJECT/project.json")"
nice -n 15 ionice -c3 cmake --build "$BUILD" --config profile -j "$JOBS" --target \
  Editor AssetProcessor AssetBuilder AssetProcessorBatch "$PROJECT_NAME.GameLauncher"

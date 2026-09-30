#!/usr/bin/env bash
# Build Dolphin for PS4 (run scripts/configure-dolphin.sh first). Extra args go to ninja.
#   build-dolphin.sh [ninja args...]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/ps4-env.sh"  # also puts PacBrew ld.lld's libxml2 compat link on LD_LIBRARY_PATH
ninja -C "$PS4_BUILD_ROOT/build/dolphin" -j"${JOBS:-8}" "$@"

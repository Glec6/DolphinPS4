#!/usr/bin/env bash
# Build the hardware probe the love-ps4 way and package it with love-ps4's launch settings
# (RetroArch PAID + param.sfo, Piglet/shader modules bundled):
#   DLPH00001  Dolphin Probe        love-ps4 toolchain as is (clang 12, libc++ 12, C++20)
#   DLPH00003  Dolphin Probe C++23  same, but clang 21 + libc++ 21 (toolchain/ps4-love-modern.cmake)
#   build-probe.sh [upload]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/ps4-env.sh"
B="$PS4_BUILD_ROOT/build"
OUT="$PS4_BUILD_ROOT/out"
HOST="ftp://${PS4_HOST:-192.168.0.90}:${PS4_FTP_PORT:-2121}"
PKGS=()
variant() {  # tag title_id title label cxx_std toolchain_file
    local build="$B/$1"
    rm -rf "$build"
    ps4_cmake -S "$HERE/../probe" -B "$build" -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE="$6" \
        -DPROBE_TAG="$1" -DPROBE_CXX_STANDARD="$5" >/dev/null
    make -C "$build" -j"${JOBS:-4}" 2>&1 | grep -E "error|check-oelf|Error" || true
    [ -f "$build/probe_eboot/eboot.bin" ] || { echo "$1: build failed" >&2; exit 1; }
    local stage="$build/stage"; mkdir -p "$stage/sce_sys"
    cp "$build/probe_eboot/eboot.bin" "$stage/"; cp "$HERE/../sce_sys/icon0.png" "$stage/sce_sys/"
    local pkg; pkg="$("$HERE/make-pkg.sh" "$stage" "$2" "$3" "${PROBE_VERSION:-01.02}" "$4" "$OUT" | tail -1)"
    echo "$1: $(basename "$pkg")"
    PKGS+=("$pkg")
}
variant probe DLPH00001 "Dolphin Probe" DOLPHINPROBE 20 "$OPENORBIS/cmake/ps4.cmake"
variant probe-cxx23 DLPH00003 "Dolphin Probe C++23" DOLPHINPROBECXX 23 "$HERE/../toolchain/ps4-love-modern.cmake"
if [ "${1:-}" = upload ]; then
    for p in "${PKGS[@]}"; do curl -sS -T "$p" "$HOST/data/pkg/"; echo "uploaded $(basename "$p")"; done
fi

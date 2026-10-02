#!/usr/bin/env bash
# Build the GNM probe (probe-gnm/) and package it as DLPH00004 "Dolphin GNM Probe".
#
# Launch identity: a plain OpenOrbis homebrew game (default PAID 0x3800000000000035, "plain"
# param.sfo: APP_TYPE 1, CATEGORY gd), not Dolphin's RetroArch identity. Under RetroArch's
# identity Piglet composites through the system shell, and the process was killed inside
# sceVideoOutOpen (no signal, v01.01); GNM needs the video output of its own.
#   build-gnm-probe.sh [upload]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/ps4-env.sh"
build="$PS4_BUILD_ROOT/build/gnm-probe"
OUT="$PS4_BUILD_ROOT/out"
HOST="ftp://${PS4_HOST:-192.168.0.90}:${PS4_FTP_PORT:-2121}"

rm -rf "$build"
ps4_cmake -S "$HERE/../probe-gnm" -B "$build" -G "Unix Makefiles" \
    -DCMAKE_TOOLCHAIN_FILE="$HERE/../toolchain/ps4-love-modern.cmake" \
    -DPS4_MALLOC_REPLACE=OFF -DPS4_PAID=0x3800000000000035 >/dev/null
make -C "$build" -j"${JOBS:-4}" 2>&1 | grep -E "error|check-oelf|Error" || true
[ -f "$build/gnm_probe_eboot/eboot.bin" ] || { echo "gnm probe: build failed" >&2; exit 1; }

stage="$build/stage"
mkdir -p "$stage/sce_sys"
cp "$build/gnm_probe_eboot/eboot.bin" "$stage/"
cp "$HERE/../sce_sys/icon0.png" "$stage/sce_sys/"
pkg="$(SFO_STYLE=plain "$HERE/make-pkg.sh" "$stage" DLPH00004 "Dolphin GNM Probe" \
    "${PROBE_VERSION:-01.00}" DOLPHINGNMPROBE "$OUT" | tail -1)"
echo "gnm probe: $(basename "$pkg")"
if [ "${1:-}" = upload ]; then
    curl -sS -T "$pkg" "$HOST/data/pkg/"
    echo "uploaded $(basename "$pkg")"
fi

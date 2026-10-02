#!/usr/bin/env bash
# Build the GNM probe (probe-gnm/) and package it as DLPH00004 "Dolphin GNM Probe", with the
# same launch settings as Dolphin (RetroArch PAID + param.sfo).
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
    -DPS4_MALLOC_REPLACE=OFF >/dev/null
make -C "$build" -j"${JOBS:-4}" 2>&1 | grep -E "error|check-oelf|Error" || true
[ -f "$build/gnm_probe_eboot/eboot.bin" ] || { echo "gnm probe: build failed" >&2; exit 1; }

stage="$build/stage"
mkdir -p "$stage/sce_sys"
cp "$build/gnm_probe_eboot/eboot.bin" "$stage/"
cp "$HERE/../sce_sys/icon0.png" "$stage/sce_sys/"
pkg="$("$HERE/make-pkg.sh" "$stage" DLPH00004 "Dolphin GNM Probe" "${PROBE_VERSION:-01.00}" \
    DOLPHINGNMPROBE "$OUT" | tail -1)"
echo "gnm probe: $(basename "$pkg")"
if [ "${1:-}" = upload ]; then
    curl -sS -T "$pkg" "$HOST/data/pkg/"
    echo "uploaded $(basename "$pkg")"
fi

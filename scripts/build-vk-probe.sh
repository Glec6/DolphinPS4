#!/usr/bin/env bash
# Build the Vulkan probe (probe-vk/: RADV on GNM) and package it as DLPH00004 (the probe slot
# shared with the GNM probe). Plain homebrew identity: 4.5 GiB of direct memory, no Piglet.
#   build-vk-probe.sh [upload]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/ps4-env.sh"
build="$PS4_BUILD_ROOT/build/vk-probe"
OUT="$PS4_BUILD_ROOT/out"
HOST="ftp://${PS4_HOST:-192.168.0.90}:${PS4_FTP_PORT:-2121}"

rm -rf "$build"
ps4_cmake -S "$HERE/../probe-vk" -B "$build" -G "Unix Makefiles" \
    -DCMAKE_TOOLCHAIN_FILE="$HERE/../toolchain/ps4-love-modern.cmake" \
    -DPS4_MALLOC_REPLACE=OFF -DPS4_PAID=0x3800000000000035 >/dev/null
make -C "$build" -j"${JOBS:-4}" 2>&1 | grep -E "error|undefined|check-oelf|Error" | head -60 || true
eboot="$build/vk_probe_eboot/eboot.bin"
[ -f "$eboot" ] || { echo "vk probe: build failed" >&2; exit 1; }

stage="$build/stage"
mkdir -p "$stage/sce_sys"
cp "$eboot" "$stage/"
cp "$HERE/../sce_sys/icon0.png" "$stage/sce_sys/"
pkg="$(SFO_STYLE=plain "$HERE/make-pkg.sh" "$stage" DLPH00004 "Dolphin GNM Probe" \
    "${PROBE_VERSION:-01.00}" DOLPHINGNMPROBE "$OUT" | tail -1)"
echo "vk probe: $(basename "$pkg")"
if [ "${1:-}" = upload ]; then
    curl -sS -T "$pkg" "$HOST/data/pkg/"
    echo "uploaded $(basename "$pkg")"
fi

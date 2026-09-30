#!/usr/bin/env bash
# Launch-failure bisection (all with PSChrome's proven PAID + plain SFO, no Sony modules):
#   DLPH00081 e1: no suspect imports, no sce_sys/about/right.sprx
#   DLPH00082 e2: e1 + sceKernelJit* imports
#   DLPH00083 e3: e1 + shm_open, sceKernelIsNeoMode/GetCpuFrequency/Usleep
#   DLPH00084 e4: e1 + right.sprx
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/../toolchain/env.sh"
HOST="ftp://${PS4_HOST:-192.168.0.90}:${PS4_FTP_PORT:-2121}"
EMPTY="$PSB/build/empty-modules"; mkdir -p "$EMPTY"
variant() {  # tag id defs no_right
    local tag=$1 id=$2 defs=$3 noright=$4 build="$PSB/build/$1"
    rm -rf "$build"
    cmake -G Ninja -S "$HERE/../probe" -B "$build" -DCMAKE_TOOLCHAIN_FILE="$HERE/../toolchain/ps4-dolphin.cmake" \
        -DPROBE_TAG="$tag" -DPS4_PAID=0x3800000000000035 "-DPROBE_DEFS=$defs" >/dev/null
    ninja -C "$build" >/dev/null
    local stage="$build/stage"; mkdir -p "$stage/sce_sys"
    cp "$build/probe_eboot/eboot.bin" "$stage/"; cp "$HERE/../sce_sys/icon0.png" "$stage/sce_sys/"
    local pkg; pkg="$(NO_RIGHT_SPRX=$noright SFO_STYLE=plain DOLPHIN_PS4_MODULES_DIR=$EMPTY "$HERE/make-pkg.sh" "$stage" "$id" "Dolphin Test ${tag}" 01.00 "DOLPHIN${tag}" "$PSB/out" | tail -1)"
    echo "$tag: $(basename "$pkg")  imports-of-interest: $(llvm-nm-21 -u "$build/probe" | awk '{print $2}' | grep -E 'Jit|shm_open|IsNeoMode|CpuFrequency|Usleep' | tr '\n' ' ')"
    echo "   files: $(cd "$stage" && find . -type f | sort | tr '\n' ' ')"
    curl -sS -T "$pkg" "$HOST/data/pkg/"
}
ALL="PROBE_NO_JIT;PROBE_NO_SHM;PROBE_NO_MISC"
variant e1 DLPH00081 "$ALL" 1
variant e2 DLPH00082 "PROBE_NO_SHM;PROBE_NO_MISC" 1
variant e3 DLPH00083 "PROBE_NO_JIT" 1
variant e4 DLPH00084 "$ALL" 0
echo uploaded

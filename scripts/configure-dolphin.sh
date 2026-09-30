#!/usr/bin/env bash
# Configure Dolphin for PS4 with the modern love-ps4-style toolchain: the NoGUI frontend with the
# PS4 platform, linked like love-ps4 (ps4-love-style.cmake is included into the top-level project).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/ps4-env.sh"
SRC="${DOLPHIN_SRC:-$PS4_BUILD_ROOT/src/dolphin}"
BUILD="$PS4_BUILD_ROOT/build/dolphin"
ps4_cmake -S "$SRC" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$HERE/../toolchain/ps4-love-modern.cmake" \
    "-DCMAKE_PROJECT_dolphin-emu_INCLUDE=$HERE/../toolchain/ps4-love-style.cmake" \
    -DENABLE_QT=OFF -DENABLE_NOGUI=ON -DENABLE_HEADLESS=ON -DENABLE_CLI_TOOL=OFF \
    -DENABLE_X11=OFF -DENABLE_EGL=OFF -DENABLE_VULKAN=OFF -DENABLE_LLVM=OFF -DENABLE_TESTS=OFF \
    -DENABLE_ALSA=OFF -DENABLE_PULSEAUDIO=OFF -DENABLE_CUBEB=OFF -DENABLE_SDL=OFF \
    -DENABLE_EVDEV=OFF -DENABLE_HWDB=OFF -DENABLE_AUTOUPDATE=OFF -DENABLE_ANALYTICS=OFF \
    -DUSE_DISCORD_PRESENCE=OFF -DUSE_MGBA=OFF -DUSE_RETRO_ACHIEVEMENTS=OFF -DUSE_UPNP=OFF \
    -DENCODE_FRAMEDUMPS=OFF -DENABLE_LTO=OFF -DENABLE_GENERIC=OFF \
    "$@"

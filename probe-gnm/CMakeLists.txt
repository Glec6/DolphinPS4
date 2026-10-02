# GNM probe (Phase 0 of a GNM video backend). See scripts/build-gnm-probe.sh.
#
# GNM_PROBE_PLAIN (default ON): build exactly like the OpenOrbis samples - PacBrew's ps4.cmake
# with its stock crt1.o, libc and add_self(), none of DolphinPS4's runtime (heap, malloc
# wrapping, crash log). Under DolphinPS4's runtime the process was killed inside sceVideoOutOpen.
cmake_minimum_required(VERSION 3.13)
project(dolphinps4_gnm_probe C CXX)

option(GNM_PROBE_PLAIN "Build like the OpenOrbis samples (no DolphinPS4 runtime)" ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

if (GNM_PROBE_PLAIN)
    set(CMAKE_CXX_STANDARD 17)
    add_executable(gnm_probe main.cpp)
    target_compile_options(gnm_probe PRIVATE -Wall -Wno-format)
    target_link_libraries(gnm_probe PRIVATE c c++ kernel SceVideoOut SceGnmDriver SceSysmodule
                          SceSystemService SceUserService)
    add_self(gnm_probe)
else ()
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    include(${CMAKE_CURRENT_LIST_DIR}/../toolchain/ps4-love-style.cmake)
    add_executable(gnm_probe main.cpp ${PS4_RUNTIME_SOURCES})
    target_compile_options(gnm_probe PRIVATE -Wall -Wno-format)
    target_link_libraries(gnm_probe PRIVATE SceVideoOut SceGnmDriver SceSysmodule SceSystemService
                          SceUserService pthread)
    ps4_add_eboot(gnm_probe)
endif ()

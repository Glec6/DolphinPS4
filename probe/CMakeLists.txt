# Configure with PacBrew's toolchain file (love-ps4 setup, see scripts/build-probe.sh).
cmake_minimum_required(VERSION 3.13)
project(dolphinps4_probe C CXX)

set(PROBE_CXX_STANDARD 20 CACHE STRING "C++ standard (23 needs the modern toolchain)")
set(CMAKE_CXX_STANDARD ${PROBE_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()
set(PROBE_TAG "probe" CACHE STRING "Names the log file: /data/DolphinPS4/<tag>.log")
set(PROBE_DEFS "" CACHE STRING "Extra definitions (PROBE_NO_JIT;PROBE_NO_SHM;PROBE_NO_MISC)")

include(${CMAKE_CURRENT_LIST_DIR}/../toolchain/ps4-love-style.cmake)

add_executable(probe main.cpp ${PS4_RUNTIME_SOURCES})
# EGL/GLES headers for types only: Piglet is loaded and resolved at run time.
target_compile_definitions(probe PRIVATE __PIGLET__ PROBE_TAG="${PROBE_TAG}" ${PROBE_DEFS})
target_compile_options(probe PRIVATE -Wall -Wno-format)
target_link_libraries(probe PRIVATE SceSysmodule SceSystemService SceVideoOut SceUserService pthread)
ps4_add_eboot(probe)

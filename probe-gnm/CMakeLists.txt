# GNM probe (Phase 0 of a GNM video backend). Configure with toolchain/ps4-love-modern.cmake,
# see scripts/build-gnm-probe.sh.
cmake_minimum_required(VERSION 3.13)
project(dolphinps4_gnm_probe C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

include(${CMAKE_CURRENT_LIST_DIR}/../toolchain/ps4-love-style.cmake)

add_executable(gnm_probe main.cpp ${PS4_RUNTIME_SOURCES})
target_compile_options(gnm_probe PRIVATE -Wall -Wno-format)
target_link_libraries(gnm_probe PRIVATE SceVideoOut SceGnmDriver SceSysmodule SceSystemService
                      SceUserService pthread)
ps4_add_eboot(gnm_probe)

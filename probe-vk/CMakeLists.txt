# Vulkan probe: RADV (Mesa, scripts/build-mesa.sh) on GNM. See scripts/build-vk-probe.sh.
cmake_minimum_required(VERSION 3.13)
project(dolphinps4_vk_probe C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()
include(${CMAKE_CURRENT_LIST_DIR}/../toolchain/ps4-love-style.cmake)

set(MESA_SRC "$ENV{HOME}/dolphinps4-build/src/mesa" CACHE PATH "Mesa source")

add_executable(vk_probe main.c ${PS4_RUNTIME_SOURCES})
target_include_directories(vk_probe PRIVATE ${MESA_SRC}/include)
target_compile_options(vk_probe PRIVATE -Wall -Wno-format)
ps4_link_mesa(vk_probe)  # RADV on GNM (toolchain/ps4-love-style.cmake)
target_link_libraries(vk_probe PRIVATE
                      z SceSysmodule SceSystemService SceUserService SceVideoOut pthread)
ps4_add_eboot(vk_probe)

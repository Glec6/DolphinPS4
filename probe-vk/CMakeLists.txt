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
set(MESA_BUILD "$ENV{HOME}/dolphinps4-build/build/mesa" CACHE PATH "Mesa PS4 build")
# libvulkan_radeon.a already contains the Vulkan runtime and WSI objects (meson link_whole).
set(MESA_LIBS
    src/amd/vulkan/libvulkan_radeon.a
    src/amd/common/libamd_common.a
    src/amd/compiler/libaco.a
    src/amd/addrlib/libaddrlib.a
    src/vulkan/util/libvulkan_util.a
    src/compiler/spirv/libvtn.a
    src/compiler/nir/libnir.a
    src/compiler/libcompiler.a
    src/util/libxmlconfig.a
    src/util/libmesa_util.a
    src/util/libmesa_util_simd.a
    src/util/libmesa_util_clflush.a
    src/util/libmesa_util_clflushopt.a
    src/util/libparson.a
    src/util/blake3/libblake3.a
    src/c11/impl/libmesa_util_c11.a)
list(TRANSFORM MESA_LIBS PREPEND "${MESA_BUILD}/")

add_executable(vk_probe main.c ${PS4_RUNTIME_SOURCES})
target_include_directories(vk_probe PRIVATE ${MESA_SRC}/include)
target_compile_options(vk_probe PRIVATE -Wall -Wno-format)
# Whole archives, as Mesa's own shared-library link does (link_whole): the dispatch tables refer
# to entrypoints through weak symbols, which don't pull archive members, so e.g.
# vk_common_GetPhysicalDeviceProperties2 was left out and resolved to NULL. ps4.cmake calls
# ld.lld directly, so these are raw linker flags.
target_link_libraries(vk_probe PRIVATE --whole-archive ${MESA_LIBS} --no-whole-archive
                      z SceSysmodule SceSystemService SceUserService pthread)
ps4_add_eboot(vk_probe)

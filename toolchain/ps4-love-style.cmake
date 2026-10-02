# Build setup copied from love-ps4 (J:\Mari0\love-ps4\platform\ps4), which runs on the test
# console. Include from a project configured with PacBrew's toolchain file:
#
#   cmake -DCMAKE_TOOLCHAIN_FILE=/opt/pacbrew/ps4/openorbis/cmake/ps4.cmake ...
#   include(<this file>)
#
# Provides ps4_add_eboot(<target>) and PS4_RUNTIME_SOURCES (heap + thread-exit fixes, crash log).

set(PS4_RUNTIME_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/../port/ps4_runtime.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../port/ps4_dlmalloc.c
    ${CMAKE_CURRENT_LIST_DIR}/../port/ps4_crashlog.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../port/ps4_sampler.cpp)

# Generate code for the PS4's CPU (AMD Jaguar: SSE4.2, AVX, BMI1, F16C, MOVBE; no AVX2) rather
# than baseline x86-64 (SSE2).
add_compile_options(-march=btver2)

# System libc heap: system modules (Piglet: program links, shader compiles, command buffers)
# allocate from it, and OpenOrbis' crt1.o leaves its size at the ~12.7 MiB default, which
# Dolphin exhausts within minutes. Link a copy of crt1.o declaring an explicit sceLibcHeapSize.
set(PS4_LIBC_HEAP_MIB 64 CACHE STRING "System libc heap size (MiB) declared in crt1.o")
set(PS4_CRT1 ${CMAKE_BINARY_DIR}/ps4-crt1-heap${PS4_LIBC_HEAP_MIB}.o)
execute_process(
  COMMAND sh ${CMAKE_CURRENT_LIST_DIR}/patch-crt1.sh ${OPENORBIS}/lib/crt1.o ${PS4_CRT1}
          ${PS4_LIBC_HEAP_MIB}
  RESULT_VARIABLE _ps4_crt1_result)
if(NOT _ps4_crt1_result EQUAL 0)
  message(FATAL_ERROR "patch-crt1.sh failed: ${_ps4_crt1_result}")
endif()
foreach(_lang C CXX)
  string(REPLACE "${OPENORBIS}/lib/crt1.o" "${PS4_CRT1}" CMAKE_${_lang}_LINK_EXECUTABLE
                 "${CMAKE_${_lang}_LINK_EXECUTABLE}")
endforeach()

# The toolchain's linker script only collects plain .init_array, so prioritized constructors
# (.init_array.NNN - e.g. libc++'s iostream setup) end up in an orphan section the loader never
# runs. Link with a copy that includes them, in priority order.
set(PS4_LINK_SCRIPT_IN ${OPENORBIS}/link.x)
set(PS4_LINK_SCRIPT ${CMAKE_BINARY_DIR}/ps4-link.x)
file(READ ${PS4_LINK_SCRIPT_IN} PS4_LINK_SCRIPT_TEXT)
string(FIND "${PS4_LINK_SCRIPT_TEXT}" "*(.init_array);" PS4_INIT_ARRAY_POS)
if (PS4_INIT_ARRAY_POS EQUAL -1)
    message(FATAL_ERROR "Unexpected ${PS4_LINK_SCRIPT_IN}: no '*(.init_array);' rule to patch")
endif ()
string(REPLACE "*(.init_array);"
    "KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*))); KEEP(*(.init_array));"
    PS4_LINK_SCRIPT_TEXT "${PS4_LINK_SCRIPT_TEXT}")
file(WRITE ${PS4_LINK_SCRIPT} "${PS4_LINK_SCRIPT_TEXT}")
string(REPLACE "${PS4_LINK_SCRIPT_IN}" "${PS4_LINK_SCRIPT}" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
if (NOT CMAKE_EXE_LINKER_FLAGS MATCHES "ps4-link.x")
    string(APPEND CMAKE_EXE_LINKER_FLAGS " --script ${PS4_LINK_SCRIPT}")
endif ()

# The libc heap can't initialize on retail consoles; route the malloc family to
# port/ps4_runtime.cpp. ps4.cmake links with ld.lld directly, so these are raw linker flags.
foreach (fn malloc free calloc realloc memalign __memalign)
    string(APPEND CMAKE_EXE_LINKER_FLAGS " --wrap=${fn}")
endforeach ()
# The kernel's pthread_once writes 16 bytes into the headers' 4-byte pthread_once_t
# (port/ps4_runtime.cpp has a replacement).
string(APPEND CMAKE_EXE_LINKER_FLAGS " --wrap=pthread_once")
# Large anonymous mmaps go to the system flexible pool, keeping regular flexible memory for Piglet.
string(APPEND CMAKE_EXE_LINKER_FLAGS " --wrap=mmap")

# Program authority ID: the system Piglet (OpenGL ES) only gives a display to processes with a
# system authority ID. This is RetroArch for PS4's value, as used by love-ps4.
set(PS4_PAID "0x3100000000000002" CACHE STRING "Program authority ID of eboot.bin")
set(PS4_AUTH_INFO "000000000000000000000000001C004000FF000000000080000000000000000000000000000000000000008000400040000000000000008000000000000000080040FFFF000000F000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000")

# ELF -> <binary dir>/<target>_eboot/eboot.bin (fake SELF), then check the result: PacBrew's
# create-fself (Nov 2024) predates OpenOrbis create-fself 626d440, and depending on the size of
# .data.rel.ro it emits the data PT_LOAD twice, which the console rejects at launch
# ("CE-41839-5", LNC 0x80aa001a).
function(ps4_add_eboot target)
    set(out_dir ${CMAKE_CURRENT_BINARY_DIR}/${target}_eboot)
    add_custom_command(
        OUTPUT ${out_dir}/eboot.bin
        COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
        COMMAND ${CMAKE_COMMAND} -E env OO_PS4_TOOLCHAIN=${OPENORBIS}
            ${OPENORBIS}/bin/create-fself -in=$<TARGET_FILE:${target}>
            -out=${out_dir}/${target}.oelf --eboot ${out_dir}/eboot.bin
            --paid ${PS4_PAID} --authinfo ${PS4_AUTH_INFO}
        COMMAND ${CMAKE_COMMAND} -DOELF=${out_dir}/${target}.oelf
            -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/check-oelf.cmake
        DEPENDS ${target}
        VERBATIM)
    add_custom_target(${target}_eboot ALL DEPENDS ${out_dir}/eboot.bin)
endfunction()

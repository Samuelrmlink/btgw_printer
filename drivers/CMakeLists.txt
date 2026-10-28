# Printer PEX modules, built out of tree against the parent's PEX SDK:
#   cmake -S drivers -B build/drivers -G Ninja && cmake --build build/drivers
# Each module directory adds itself with a module.cmake, so branches that add
# a driver never touch this file.
cmake_minimum_required(VERSION 3.20)
project(btgw_printer_drivers NONE)
include(${CMAKE_CURRENT_LIST_DIR}/../btgw/pex/sdk/cmake/pex.cmake)

set(PRN_INCLUDE ${CMAKE_CURRENT_LIST_DIR}/include)
if(NOT PRN_OUT)
    set(PRN_OUT ${CMAKE_BINARY_DIR}/pex)
endif()

# The parent's pex_add_module() links with debug info, and the module memory on
# the board (48 KB, shared with every other module) is tight: a .pex with its
# DEBUG section did not fit next to the Anker module on the bench. So each
# module is linked twice: ${PRN_OUT}/debug/<name>.<arch>.pex keeps the debug
# info (for pexlink dump and fault addresses), and ${PRN_OUT}/<name>.<arch>.pex
# is the same ELF linked with --no-debug. -DPRN_PEX_DEBUG=ON keeps debug info
# in the main copy instead.
option(PRN_PEX_DEBUG "keep the DEBUG section in the .pex files" OFF)
# The code model of every module (btgw/docs/pex-format.md section 6): reloc
# (model A, gcc, relocated at install) or pic (model B, clang ROPI+RWPI, RW
# through r9). Model B runs in place as soon as it is installed; model A
# installed after boot runs from RAM until the next boot. A module.cmake may
# still pin one with MODEL. docs/notes/pex-model-b.md
set(PRN_PEX_MODEL reloc CACHE STRING "PEX code model for the printer modules: reloc or pic")
set_property(CACHE PRN_PEX_MODEL PROPERTY STRINGS reloc pic)
# clang has no C library headers for arm-none-eabi, and our sources include
# <string.h> (memcpy and friends come from the host API either way): under
# model B take newlib's headers from the gcc toolchain, as model A does.
execute_process(COMMAND ${PEX_GCC} -print-sysroot OUTPUT_VARIABLE _prn_sysroot
    OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0 OR NOT EXISTS "${_prn_sysroot}/include/string.h")
    message(FATAL_ERROR "prn: no newlib headers via ${PEX_GCC} -print-sysroot")
endif()
set(PRN_PIC_CFLAGS -isystem ${_prn_sysroot}/include)
function(prn_add_module name)
    cmake_parse_arguments(M "LUA" "MODEL;KIND;VERSION;DESCRIPTION;FLAGS;OUTPUT_DIR" "SOURCES;TARGETS;CFLAGS;INCLUDES" ${ARGN})
    if(NOT M_MODEL)
        set(M_MODEL ${PRN_PEX_MODEL})
    endif()
    if(NOT M_TARGETS)
        set(M_TARGETS m33)
    endif()
    if(NOT M_OUTPUT_DIR)
        set(M_OUTPUT_DIR ${PRN_OUT})
    endif()
    set(_lua)
    if(M_LUA)
        set(_lua LUA)
    endif()
    if(M_MODEL STREQUAL "pic")
        list(APPEND M_CFLAGS ${PRN_PIC_CFLAGS})
    endif()
    pex_add_module(${name} SOURCES ${M_SOURCES} TARGETS ${M_TARGETS} MODEL ${M_MODEL} KIND ${M_KIND}
        VERSION ${M_VERSION} DESCRIPTION ${M_DESCRIPTION} CFLAGS ${M_CFLAGS} INCLUDES ${M_INCLUDES}
        OUTPUT_DIR ${M_OUTPUT_DIR}/debug ${_lua})
    if(PRN_PEX_DEBUG)
        set(_nodebug)
    else()
        set(_nodebug --no-debug)
    endif()
    set(_outs)
    foreach(t ${M_TARGETS})
        set(_work ${CMAKE_CURRENT_BINARY_DIR}/pex_obj/${name}_${M_MODEL}/${t})
        set(_sus)
        foreach(src ${M_SOURCES})
            get_filename_component(_b ${src} NAME_WE)
            list(APPEND _sus --su ${_work}/${_b}.su)
        endforeach()
        set(_args link ${_work}/${name}.elf -o ${M_OUTPUT_DIR}/${name}.${t}.pex --target ${t} --model ${M_MODEL}
            --api ${PEX_API_JSON} --name ${name} --version ${M_VERSION} --kind ${M_KIND} ${_sus} ${_nodebug})
        if(M_LUA)
            list(APPEND _args --lua)
        endif()
        if(M_DESCRIPTION)
            list(APPEND _args --desc ${M_DESCRIPTION})
        endif()
        if(M_FLAGS)
            list(APPEND _args --flags ${M_FLAGS})
        endif()
        add_custom_command(OUTPUT ${M_OUTPUT_DIR}/${name}.${t}.pex
            COMMAND ${CMAKE_COMMAND} -E make_directory ${M_OUTPUT_DIR}
            COMMAND ${Python3_EXECUTABLE} ${PEX_ROOT}/tools/pexlink.py ${_args}
            DEPENDS ${M_OUTPUT_DIR}/debug/${name}.${t}.pex ${PEX_ROOT}/tools/pexlink.py VERBATIM)
        list(APPEND _outs ${M_OUTPUT_DIR}/${name}.${t}.pex)
    endforeach()
    add_custom_target(prn_${name} ALL DEPENDS ${_outs})
    add_dependencies(prn_${name} pex_${name}_${M_MODEL})
endfunction()

file(GLOB_RECURSE _prn_modules CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/*/module.cmake)
list(SORT _prn_modules)
foreach(_m ${_prn_modules})
    include(${_m})
endforeach()

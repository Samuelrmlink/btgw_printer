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

file(GLOB_RECURSE _prn_modules CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/*/module.cmake)
list(SORT _prn_modules)
foreach(_m ${_prn_modules})
    include(${_m})
endforeach()

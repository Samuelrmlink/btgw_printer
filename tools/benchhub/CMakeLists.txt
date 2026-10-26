# Bench-only stand-in for printer_hub (see benchhub.c):
#   cmake -S tools/benchhub -B build/benchhub -G Ninja && cmake --build build/benchhub
cmake_minimum_required(VERSION 3.20)
project(benchhub NONE)
include(${CMAKE_CURRENT_LIST_DIR}/../../btgw/pex/sdk/cmake/pex.cmake)
pex_add_module(printer_hub
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/benchhub.c
    INCLUDES ${CMAKE_CURRENT_LIST_DIR}/../../drivers/include
    TARGETS m33 MODEL reloc KIND app VERSION 0.0.1
    DESCRIPTION "bench stand-in for printer_hub"
    OUTPUT_DIR ${CMAKE_BINARY_DIR}/pex)

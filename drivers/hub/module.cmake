prn_add_module(printer_hub
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/printer_hub.c ${CMAKE_CURRENT_LIST_DIR}/../core/prn_core.c
    TARGETS m33 m0p
    KIND app
    VERSION 0.1.0
    DESCRIPTION "printer.* ops, driver registry and job streamer"
    INCLUDES ${PRN_INCLUDE} ${CMAKE_CURRENT_LIST_DIR}/..
    OUTPUT_DIR ${PRN_OUT})

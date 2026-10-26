prn_add_module(nelko_pm220
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/pm220.c ${CMAKE_CURRENT_LIST_DIR}/pm220_proto.c
            ${CMAKE_CURRENT_LIST_DIR}/../../../core/prn_core.c ${CMAKE_CURRENT_LIST_DIR}/../../../proto/tspl/tspl.c
    TARGETS m33 m0p
    MODEL reloc
    KIND app
    VERSION 0.1.0
    DESCRIPTION "Nelko PM220 (Bluetooth Classic SPP)"
    INCLUDES ${PRN_INCLUDE} ${CMAKE_CURRENT_LIST_DIR}/../../..
    OUTPUT_DIR ${PRN_OUT})

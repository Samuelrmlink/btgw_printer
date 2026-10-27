# Nelko TSPL printers over Bluetooth Classic SPP: PM220, P21 (a row each in
# MODELS[], nelko_tspl.c). docs/protocols/nelko-tspl.md, nelko-p21.md
prn_add_module(nelko_tspl
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/nelko_tspl.c ${CMAKE_CURRENT_LIST_DIR}/nelko_proto.c
            ${CMAKE_CURRENT_LIST_DIR}/../../../core/prn_core.c ${CMAKE_CURRENT_LIST_DIR}/../../../proto/tspl/tspl.c
    TARGETS m33 m0p
    MODEL reloc
    KIND app
    VERSION 0.2.0
    DESCRIPTION "Nelko TSPL printers: PM220, P21 (Bluetooth Classic SPP)"
    INCLUDES ${PRN_INCLUDE} ${CMAKE_CURRENT_LIST_DIR}/../../..
    OUTPUT_DIR ${PRN_OUT})

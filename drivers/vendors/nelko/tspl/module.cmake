# Nelko TSPL printers over Bluetooth Classic SPP: PM220 (a row in MODELS[],
# nelko_tspl.c; the P21 is nelko_ble's, over BLE). docs/protocols/nelko-tspl.md
prn_add_module(nelko_tspl
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/nelko_tspl.c ${CMAKE_CURRENT_LIST_DIR}/nelko_proto.c
            ${CMAKE_CURRENT_LIST_DIR}/../../../core/prn_core.c ${CMAKE_CURRENT_LIST_DIR}/../../../proto/tspl/tspl.c
    TARGETS m33 m0p
    MODEL reloc
    KIND app
    VERSION 0.3.0
    DESCRIPTION "Nelko TSPL printers: PM220 (Bluetooth Classic SPP)"
    INCLUDES ${PRN_INCLUDE} ${CMAKE_CURRENT_LIST_DIR}/../../..
    OUTPUT_DIR ${PRN_OUT})

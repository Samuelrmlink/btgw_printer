# Nelko TSPL printers over BLE (GATT ff00): P21 (a row in MODELS[],
# nelko_ble.c). docs/protocols/nelko-p21.md. The reply parsing is nelko_tspl's.
set(_d ${CMAKE_CURRENT_LIST_DIR})
set(_r ${CMAKE_CURRENT_LIST_DIR}/../../..)
prn_add_module(nelko_ble
    SOURCES ${_d}/nelko_ble.c ${_r}/vendors/nelko/tspl/nelko_proto.c ${_r}/core/prn_core.c ${_r}/proto/tspl/tspl.c
    INCLUDES ${PRN_INCLUDE} ${_r}
    TARGETS m33
    KIND app
    VERSION 0.1.0
    DESCRIPTION "Nelko TSPL printers over BLE: P21"
    OUTPUT_DIR ${PRN_OUT})

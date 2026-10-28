# Nelko PL70e-BT (BLE) driver module.
set(_d ${CMAKE_CURRENT_LIST_DIR})
set(_r ${CMAKE_CURRENT_LIST_DIR}/../../..)
prn_add_module(nelko_pl70e
    SOURCES ${_d}/pl70e.c ${_d}/pl70e_proto.c ${_r}/core/prn_core.c ${_r}/proto/tspl/tspl.c
    INCLUDES ${PRN_INCLUDE} ${_r}
    TARGETS m33
    KIND app
    VERSION 0.1.0
    DESCRIPTION "Nelko PL70e-BT label printer (BLE, TSPL)"
    OUTPUT_DIR ${PRN_OUT})

# Phomemo D30 (Bluetooth Classic SPP). docs/protocols/phomemo-d30.md
pex_add_module(phomemo_d30
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/d30_app.c ${CMAKE_CURRENT_LIST_DIR}/d30_proto.c
    TARGETS m33
    MODEL reloc
    KIND app
    VERSION 0.1.0
    DESCRIPTION "Phomemo D30 label printer"
    INCLUDES ${PRN_INCLUDE} ${CMAKE_CURRENT_LIST_DIR}/../../..
    OUTPUT_DIR ${PRN_OUT})

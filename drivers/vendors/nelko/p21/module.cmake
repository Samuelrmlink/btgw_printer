# Nelko P21 (Bluetooth Classic SPP). docs/protocols/nelko-p21.md
pex_add_module(nelko_p21
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/p21_app.c ${CMAKE_CURRENT_LIST_DIR}/p21_proto.c
    TARGETS m33
    MODEL reloc
    KIND app
    VERSION 0.1.0
    DESCRIPTION "Nelko P21 label printer"
    INCLUDES ${PRN_INCLUDE} ${CMAKE_CURRENT_LIST_DIR}/../../..
    OUTPUT_DIR ${PRN_OUT})

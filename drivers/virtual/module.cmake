pex_add_module(printer_virtual
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/virtual.c ${CMAKE_CURRENT_LIST_DIR}/../core/prn_core.c
    TARGETS m33 m0p
    MODEL reloc
    KIND app
    VERSION 0.1.0
    DESCRIPTION "A printer that is not there, for tests"
    INCLUDES ${PRN_INCLUDE} ${CMAKE_CURRENT_LIST_DIR}/..
    OUTPUT_DIR ${PRN_OUT})

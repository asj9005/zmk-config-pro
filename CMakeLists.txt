# Copyright (c) 2026 asj9005
# SPDX-License-Identifier: MIT

zephyr_library()

set(TOTEM_WEST_TOPDIR "${APPLICATION_SOURCE_DIR}/../..")
set(TOTEM_ESB_MODULE_DIR "${TOTEM_WEST_TOPDIR}/zmk-feature-split-esb")
set(TOTEM_PROSPECTOR_MODULE_DIR "${TOTEM_WEST_TOPDIR}/prospector-zmk-module")
set(TOTEM_ZMK_CENTRAL_H "${APPLICATION_SOURCE_DIR}/include/zmk/split/central.h")

# The pinned upstream ESB module edits this ZMK header in place. Always make
# configuration deterministic so an ESB build cannot contaminate a later BLE
# build in the same west workspace.
if(EXISTS "${TOTEM_ZMK_CENTRAL_H}")
    file(READ "${TOTEM_ZMK_CENTRAL_H}" TOTEM_CENTRAL_H_CONTENTS)
    if(CONFIG_TOTEM_ESB_COMPAT)
        set(TOTEM_CENTRAL_COUNT_LINE
            "#define ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT ${CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT}")
    else()
        set(TOTEM_CENTRAL_COUNT_LINE
            "#define ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT MAX(BLE_PERIPHERAL_COUNT, WIRED_PERIPHERAL_COUNT)")
    endif()
    string(REGEX REPLACE
        "#define[ \t]+ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT[^\r\n]*"
        "${TOTEM_CENTRAL_COUNT_LINE}"
        TOTEM_CENTRAL_H_PATCHED
        "${TOTEM_CENTRAL_H_CONTENTS}")
    if(NOT TOTEM_CENTRAL_H_CONTENTS STREQUAL TOTEM_CENTRAL_H_PATCHED)
        file(WRITE "${TOTEM_ZMK_CENTRAL_H}" "${TOTEM_CENTRAL_H_PATCHED}")
    endif()
endif()

if(CONFIG_TOTEM_ESB_COMPAT)
    if(NOT EXISTS "${TOTEM_ESB_MODULE_DIR}/src/split/esb/central.c")
        message(FATAL_ERROR "Pinned zmk-feature-split-esb project was not found")
    endif()

    foreach(TOTEM_ESB_FILE common.h common.c central.c peripheral.c app_esb.h app_esb.c)
        configure_file(
            "${ZEPHYR_CURRENT_MODULE_DIR}/compat/zmk-feature-split-esb/src/split/esb/${TOTEM_ESB_FILE}"
            "${TOTEM_ESB_MODULE_DIR}/src/split/esb/${TOTEM_ESB_FILE}"
            COPYONLY)
    endforeach()

    zephyr_include_directories(include)
    zephyr_library_sources(src/totem_esb_hooks.c)

    if(CONFIG_TOTEM_ESB_PROSPECTOR)
        if(NOT EXISTS "${TOTEM_PROSPECTOR_MODULE_DIR}/CMakeLists.txt")
            message(FATAL_ERROR "Pinned Prospector project was not found")
        endif()

        configure_file(
            "${ZEPHYR_CURRENT_MODULE_DIR}/compat/prospector-zmk-module/src/split/bluetooth/central_status_changed_observer.c"
            "${TOTEM_PROSPECTOR_MODULE_DIR}/src/split/bluetooth/central_status_changed_observer.c"
            COPYONLY)
        foreach(TOTEM_OPERATOR_FILE output.c output.h)
            configure_file(
                "${ZEPHYR_CURRENT_MODULE_DIR}/compat/prospector-zmk-module/boards/shields/prospector_adapter/src/layouts/operator/${TOTEM_OPERATOR_FILE}"
                "${TOTEM_PROSPECTOR_MODULE_DIR}/boards/shields/prospector_adapter/src/layouts/operator/${TOTEM_OPERATOR_FILE}"
                COPYONLY)
        endforeach()

        zephyr_compile_definitions(
            PERIPHERAL_COUNT=${CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT})
    endif()
endif()

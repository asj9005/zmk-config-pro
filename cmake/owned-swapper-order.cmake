# SPDX-License-Identifier: MIT
include_guard(GLOBAL)

# The pinned Zephyr already requires CMake 3.20, including DEFER support.
# A custom keymap without this behavior needs neither ordering nor assertions.
if(NOT CONFIG_DT_HAS_TOTEM_BEHAVIOR_OWNED_SWAPPER_ENABLED)
    return()
endif()

if(CMAKE_VERSION VERSION_LESS 3.20)
    message(FATAL_ERROR "Owned swapper ordering requires the pinned Zephyr's CMake 3.20 minimum")
endif()

function(totem_owned_swapper_order_sources)
    get_target_property(_totem_owned_sources app SOURCES)
    get_property(_totem_owned_app_dir GLOBAL PROPERTY TOTEM_OWNED_SWAPPER_APP_DIR)
    get_property(_totem_owned_replay_path GLOBAL PROPERTY TOTEM_OWNED_SWAPPER_REPLAY_PATH)
    get_property(_totem_owned_raw_path GLOBAL PROPERTY TOTEM_OWNED_SWAPPER_RAW_PATH)

    # Normalize only ordinary paths for comparison; preserve each original
    # SOURCES entry (including generator expressions) when writing the list.
    set(_totem_owned_replay_entries)
    set(_totem_owned_raw_count 0)
    set(_totem_owned_keymap_count 0)
    foreach(_totem_owned_source IN LISTS _totem_owned_sources)
        if(_totem_owned_source MATCHES "^\\$<")
            continue()
        endif()
        get_filename_component(_totem_owned_absolute "${_totem_owned_source}" ABSOLUTE
                               BASE_DIR "${_totem_owned_app_dir}")
        if(_totem_owned_absolute STREQUAL _totem_owned_replay_path)
            list(APPEND _totem_owned_replay_entries "${_totem_owned_source}")
        elseif(_totem_owned_absolute STREQUAL _totem_owned_raw_path)
            math(EXPR _totem_owned_raw_count "${_totem_owned_raw_count} + 1")
        elseif(_totem_owned_absolute STREQUAL "${_totem_owned_app_dir}/src/keymap.c")
            math(EXPR _totem_owned_keymap_count "${_totem_owned_keymap_count} + 1")
            set(_totem_owned_keymap_entry "${_totem_owned_source}")
        endif()
    endforeach()
    list(LENGTH _totem_owned_replay_entries _totem_owned_replay_count)
    if(NOT _totem_owned_replay_count EQUAL 1 OR NOT _totem_owned_raw_count EQUAL 1 OR
       NOT _totem_owned_keymap_count EQUAL 1)
        message(FATAL_ERROR "Owned swapper ordering requires exactly one raw, replay, and pinned keymap source")
    endif()

    # Module sources are registered while find_package(Zephyr) is running,
    # before app/CMakeLists.txt adds the capturers and keymap. Defer this edit
    # until the application's source list is complete.
    list(REMOVE_ITEM _totem_owned_sources ${_totem_owned_replay_entries})
    list(FIND _totem_owned_sources "${_totem_owned_keymap_entry}" _totem_owned_keymap_index)
    list(INSERT _totem_owned_sources ${_totem_owned_keymap_index} ${_totem_owned_replay_entries})

    set(_totem_owned_capturers)
    if(CONFIG_ZMK_BEHAVIOR_HOLD_TAP AND CONFIG_DT_HAS_ZMK_BEHAVIOR_HOLD_TAP_ENABLED)
        list(APPEND _totem_owned_capturers "src/behaviors/behavior_hold_tap.c")
    endif()
    if(CONFIG_DT_HAS_ZMK_COMBOS_ENABLED)
        list(APPEND _totem_owned_capturers "src/combo.c")
    endif()
    if(CONFIG_ZMK_BEHAVIOR_TAP_DANCE AND CONFIG_DT_HAS_ZMK_BEHAVIOR_TAP_DANCE_ENABLED)
        list(APPEND _totem_owned_capturers "src/behaviors/behavior_tap_dance.c")
    endif()

    set(_totem_owned_normalized)
    foreach(_totem_owned_source IN LISTS _totem_owned_sources)
        if(_totem_owned_source MATCHES "^\\$<")
            list(APPEND _totem_owned_normalized "${_totem_owned_source}")
        else()
            get_filename_component(_totem_owned_absolute "${_totem_owned_source}" ABSOLUTE
                                   BASE_DIR "${_totem_owned_app_dir}")
            list(APPEND _totem_owned_normalized "${_totem_owned_absolute}")
        endif()
    endforeach()
    list(FIND _totem_owned_normalized "${_totem_owned_raw_path}" _totem_owned_raw_index)
    list(FIND _totem_owned_normalized "${_totem_owned_replay_path}" _totem_owned_replay_index)
    list(FIND _totem_owned_normalized "${_totem_owned_app_dir}/src/keymap.c" _totem_owned_keymap_index)
    if(NOT _totem_owned_raw_index LESS _totem_owned_replay_index OR
       NOT _totem_owned_replay_index LESS _totem_owned_keymap_index)
        message(FATAL_ERROR "Owned swapper raw/replay/keymap ordering is invalid")
    endif()
    foreach(_totem_owned_capturer IN LISTS _totem_owned_capturers)
        list(FIND _totem_owned_normalized "${_totem_owned_app_dir}/${_totem_owned_capturer}"
             _totem_owned_capture_index)
        if(_totem_owned_capture_index LESS 0 OR
           NOT _totem_owned_raw_index LESS _totem_owned_capture_index OR
           NOT _totem_owned_capture_index LESS _totem_owned_replay_index)
            message(FATAL_ERROR "Owned swapper requires raw < ${_totem_owned_capturer} < replay < keymap")
        endif()
    endforeach()
    set_property(TARGET app PROPERTY SOURCES "${_totem_owned_sources}")
endfunction()

get_filename_component(_totem_owned_app_dir "${APPLICATION_SOURCE_DIR}" ABSOLUTE)
get_filename_component(_totem_owned_replay_path
                       "${CMAKE_CURRENT_LIST_DIR}/../src/owned_swapper_replay_listener.c" ABSOLUTE)
get_filename_component(_totem_owned_raw_path
                       "${CMAKE_CURRENT_LIST_DIR}/../src/behavior_owned_swapper.c" ABSOLUTE)
set_property(GLOBAL PROPERTY TOTEM_OWNED_SWAPPER_APP_DIR "${_totem_owned_app_dir}")
set_property(GLOBAL PROPERTY TOTEM_OWNED_SWAPPER_REPLAY_PATH "${_totem_owned_replay_path}")
set_property(GLOBAL PROPERTY TOTEM_OWNED_SWAPPER_RAW_PATH "${_totem_owned_raw_path}")
zephyr_linker_sources(SECTIONS "${CMAKE_CURRENT_LIST_DIR}/../linker/owned-swapper-order.ld")
cmake_language(DEFER DIRECTORY "${APPLICATION_SOURCE_DIR}" CALL totem_owned_swapper_order_sources)

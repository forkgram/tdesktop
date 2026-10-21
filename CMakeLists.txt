# This file is part of Telegram Desktop,
# the official desktop application for the Telegram messaging service.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

cmake_minimum_required(VERSION 3.25...3.31)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Apply local patches to submodules at configure time.
# Filename prefix (split on '_') is matched against submodule directories,
# trying the longest prefix first, both at the repo root and under Telegram/.
file(GLOB _submodule_patches CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/patches/*.patch")
foreach(_patch ${_submodule_patches})
    get_filename_component(_patch_stem ${_patch} NAME_WE)
    get_filename_component(_patch_name ${_patch} NAME)
    string(REPLACE "_" ";" _parts ${_patch_stem})
    list(LENGTH _parts _n)
    set(_applied FALSE)
    set(_already FALSE)
    set(_matched "")
    while(_n GREATER 0 AND NOT _applied AND NOT _already)
        list(SUBLIST _parts 0 ${_n} _head)
        string(REPLACE ";" "_" _candidate "${_head}")
        foreach(_root "Telegram/" "")
            set(_dir "${CMAKE_SOURCE_DIR}/${_root}${_candidate}")
            if (IS_DIRECTORY ${_dir} AND EXISTS ${_dir}/.git)
                if (NOT _matched)
                    set(_matched "${_root}${_candidate}")
                endif()
                execute_process(
                    COMMAND git apply --check ${_patch}
                    WORKING_DIRECTORY ${_dir}
                    RESULT_VARIABLE _check_result
                    OUTPUT_VARIABLE _check_output
                    ERROR_VARIABLE _check_output
                )
                if (_check_result EQUAL 0 AND NOT _check_output MATCHES "Skipped patch")
                    execute_process(
                        COMMAND git apply ${_patch}
                        WORKING_DIRECTORY ${_dir}
                    )
                    message(STATUS "Applied patch ${_patch_name} in ${_root}${_candidate}")
                    set(_applied TRUE)
                    break()
                endif()
                # Forward apply failed. Only a patch that reverses cleanly is
                # already applied; anything else has rotted against a moved
                # submodule and would otherwise be dropped without a trace.
                execute_process(
                    COMMAND git apply --reverse --check ${_patch}
                    WORKING_DIRECTORY ${_dir}
                    RESULT_VARIABLE _reverse_result
                    OUTPUT_VARIABLE _reverse_output
                    ERROR_VARIABLE _reverse_output
                )
                if (_reverse_result EQUAL 0 AND NOT _reverse_output MATCHES "Skipped patch")
                    message(STATUS "Skipped patch ${_patch_name} (already applied in ${_root}${_candidate})")
                    set(_already TRUE)
                    break()
                endif()
            endif()
        endforeach()
        math(EXPR _n "${_n} - 1")
    endwhile()
    if (NOT _applied AND NOT _already)
        if (_matched)
            message(WARNING
                "Patch ${_patch_name} no longer applies to ${_matched} and is not "
                "already applied there. It is being ignored -- refresh it against "
                "the current submodule or delete it.")
        else()
            message(STATUS "Skipped patch ${_patch_name} (no matching submodule)")
        endif()
    endif()
endforeach()

include(cmake/validate_special_target.cmake)
include(cmake/version.cmake)
desktop_app_parse_version(Telegram/build/version)

if (NOT DEFINED CMAKE_CONFIGURATION_TYPES)
    set(configuration_types_init 1)
endif()

project(Telegram
    LANGUAGES C CXX
    VERSION ${desktop_app_version_cmake}
    DESCRIPTION "Official Telegram Desktop messenger"
    HOMEPAGE_URL "https://desktop.telegram.org"
)

if (APPLE)
    enable_language(OBJC OBJCXX)
endif()

if (configuration_types_init
    AND CMAKE_CONFIGURATION_TYPES
    AND NOT MinSizeRel IN_LIST CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES};MinSizeRel" CACHE STRING "" FORCE)
endif()

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT Telegram)

get_filename_component(third_party_loc "Telegram/ThirdParty" REALPATH)
get_filename_component(submodules_loc "Telegram" REALPATH)
get_filename_component(cmake_helpers_loc "cmake" REALPATH)

if (NOT DESKTOP_APP_USE_PACKAGED AND WIN32)
    set(Python3_EXECUTABLE ${CMAKE_CURRENT_SOURCE_DIR}/../ThirdParty/python/Scripts/python)
endif()

include(cmake/variables.cmake)
include(cmake/nice_target_sources.cmake)
include(cmake/target_compile_options_if_exists.cmake)
include(cmake/target_link_frameworks.cmake)
include(cmake/target_link_options_if_exists.cmake)
include(cmake/init_target.cmake)
include(cmake/generate_target.cmake)
include(cmake/nuget.cmake)
include(cmake/validate_d3d_compiler.cmake)
include(cmake/target_prepare_qrc.cmake)

include(cmake/options.cmake)

include(cmake/external/qt/package.cmake)

set(desktop_app_skip_libs
    glibmm
    variant
)

add_subdirectory(cmake)
add_subdirectory(Telegram)

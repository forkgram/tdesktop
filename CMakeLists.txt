# This file is part of Telegram Desktop,
# the official desktop application for the Telegram messaging service.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

cmake_minimum_required(VERSION 3.31)

# Workaround some linking errors in GCC due to static library order.
if (POLICY CMP0156)
    cmake_policy(SET CMP0156 OLD)
endif()
if (POLICY CMP0179)
    cmake_policy(SET CMP0179 OLD)
endif()

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
    while(_n GREATER 0 AND NOT _applied)
        list(SUBLIST _parts 0 ${_n} _head)
        string(REPLACE ";" "_" _candidate "${_head}")
        foreach(_root "Telegram/" "")
            set(_dir "${CMAKE_SOURCE_DIR}/${_root}${_candidate}")
            if (IS_DIRECTORY ${_dir} AND EXISTS ${_dir}/.git)
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
            endif()
        endforeach()
        math(EXPR _n "${_n} - 1")
    endwhile()
    if (NOT _applied)
        message(STATUS "Skipped patch ${_patch_name} (no matching submodule or already applied)")
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
    set(QT_NO_PRIVATE_MODULE_WARNING ON)
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

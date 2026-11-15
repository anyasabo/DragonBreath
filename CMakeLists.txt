# DragonBreath — open firmware for the BIGTREETECH Panda Breath (ESP32-C3)
cmake_minimum_required(VERSION 3.16)
# Shared board-agnostic services come from justinh-rahb/dragon-core through the
# ESP-IDF component manager. Product-specific pb_* components remain local.
# App version stamped into the ESP app descriptor (esp_app_get_description()->version,
# also shown in the web UI): the exact git tag when HEAD is tagged (releases), else
# the short commit hash (PR / local builds), with "-dirty" if the tree is modified.
execute_process(
    COMMAND git describe --tags --exact-match
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE DB_TAG OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET RESULT_VARIABLE DB_TAG_RC)
if(DB_TAG_RC EQUAL 0 AND DB_TAG)
    set(PROJECT_VER "${DB_TAG}")
else()
    execute_process(
        COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE DB_HASH OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE DB_HASH_RC)
    execute_process(
        COMMAND git status --porcelain --untracked-files=no
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE DB_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT DB_HASH_RC EQUAL 0)
        set(PROJECT_VER "unknown")
    elseif(DB_DIRTY)
        set(PROJECT_VER "${DB_HASH}-dirty")
    else()
        set(PROJECT_VER "${DB_HASH}")
    endif()
endif()
message(STATUS "DragonBreath version: ${PROJECT_VER}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Fork-only: dependencies.lock in git is for one ESP-IDF version (its "idf:"
# entry). A build with another ESP-IDF version keeps its lock in the build
# directory, so it does not rewrite the tracked file.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock")
    file(READ "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock" DB_LOCK_TEXT)
    if(DB_LOCK_TEXT MATCHES "\n  idf:\n    source:\n      type: idf\n    version: ([0-9.]+)")
        set(DB_LOCK_IDF "${CMAKE_MATCH_1}")
        if(NOT DB_LOCK_IDF STREQUAL "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}")
            message(STATUS "ESP-IDF differs from dependencies.lock (${DB_LOCK_IDF}): "
                           "using ${CMAKE_BINARY_DIR}/dependencies.lock")
            idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_BINARY_DIR}/dependencies.lock")
        endif()
    endif()
endif()

# Stack-usage files for tools/elf_check.py --stack. Off by default: the .su
# files do not change the firmware, but the option changes the compile flags,
# so a build dir with it rebuilds all. Use a separate build dir:
#   idf.py -B build-stack -D DB_STACK_USAGE=ON build
option(DB_STACK_USAGE "Write -fstack-usage .su files for each object" OFF)
if(DB_STACK_USAGE)
    idf_build_set_property(COMPILE_OPTIONS "-fstack-usage" APPEND)
endif()

project(dragonbreath)

# ESP-IDF 5.3.1's HTTP server can process and recycle a socket while an
# asynchronous request worker still owns it. DragonBreath's SSE endpoint uses
# that API, so require Espressif's 8b559ce61454 fix, first released in
# ESP-IDF 5.3.2.
# Use the numeric version from ESP-IDF's version.cmake. IDF_VER is a git
# description and can have two parts, for example "v6.1".
if(NOT DEFINED IDF_VERSION_MAJOR OR NOT DEFINED IDF_VERSION_MINOR
   OR NOT DEFINED IDF_VERSION_PATCH)
    message(FATAL_ERROR "Cannot determine the ESP-IDF version")
endif()
set(DB_IDF_SEMVER "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}")
if(DB_IDF_SEMVER VERSION_LESS "5.3.2")
    message(FATAL_ERROR
        "DragonBreath requires ESP-IDF v5.3.2 or newer: older releases can "
        "concurrently process sockets owned by asynchronous HTTP requests")
endif()
# CI and release builds use the ESP-IDF tag in .idf-version. Warn if this build differs.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.idf-version")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/.idf-version" DB_IDF_PINNED LIMIT_COUNT 1)
    string(STRIP "${DB_IDF_PINNED}" DB_IDF_PINNED)
    string(REGEX REPLACE "^v" "" DB_IDF_PINNED_SEMVER "${DB_IDF_PINNED}")
    if(NOT DB_IDF_SEMVER VERSION_EQUAL DB_IDF_PINNED_SEMVER)
        message(WARNING "ESP-IDF ${DB_IDF_SEMVER} differs from .idf-version (${DB_IDF_PINNED}), "
                        "the version of the CI and release builds")
    endif()
endif()

# E1 (Phase E) — warnings-as-errors for OUR OWN first-party components only.
# Applied here (after project(), when the per-component library targets exist) as a
# PRIVATE option so it never leaks to ESP-IDF's own components or to dependents.
# ESP-IDF framework code is deliberately excluded (we do not control its warning
# cleanliness). Both the default and release builds must stay clean under these flags.
#
# The pb_* and db_* components are auto-discovered from components/ (so a new one is
# covered automatically). main (app_main.c) is first-party too. We assert at least one
# matched: if ESP-IDF ever changes the internal component-target naming
# (__idf_<name>), this fails the configure loudly instead of silently applying
# -Werror to nothing.
#
# DB_EXTRA_WARNINGS=ON adds more warnings to the same components. With it, a plain
# -Wno-error keeps all warnings as warnings, so a CI job can count them without a
# failed build. The default build keeps -Werror.
option(DB_EXTRA_WARNINGS "Add extra warnings (not errors) to first-party components" OFF)
set(DB_EXTRA_WARNING_FLAGS
    -Wdouble-promotion -Wshadow -Wformat=2 -Wundef -Wstrict-prototypes)
file(GLOB DB_OWN_COMPONENT_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/components/pb_*
    ${CMAKE_CURRENT_SOURCE_DIR}/components/db_*)
set(DB_OWN_COMPONENTS main)
foreach(db_dir ${DB_OWN_COMPONENT_DIRS})
    if(IS_DIRECTORY ${db_dir})
        get_filename_component(db_comp ${db_dir} NAME)
        list(APPEND DB_OWN_COMPONENTS ${db_comp})
    endif()
endforeach()
set(DB_WERROR_APPLIED 0)
foreach(db_comp ${DB_OWN_COMPONENTS})
    if(TARGET __idf_${db_comp})
        target_compile_options(__idf_${db_comp} PRIVATE -Wall -Wextra -Werror)
        if(DB_EXTRA_WARNINGS)
            target_compile_options(__idf_${db_comp} PRIVATE ${DB_EXTRA_WARNING_FLAGS} -Wno-error)
        endif()
        math(EXPR DB_WERROR_APPLIED "${DB_WERROR_APPLIED} + 1")
    endif()
endforeach()
if(DB_WERROR_APPLIED EQUAL 0)
    message(FATAL_ERROR "E1: -Werror gate matched ZERO first-party components — "
                        "the ESP-IDF component-target naming may have changed")
endif()
message(STATUS "E1: -Wall -Wextra -Werror applied to ${DB_WERROR_APPLIED} first-party components")
if(DB_EXTRA_WARNINGS)
    message(STATUS "DB_EXTRA_WARNINGS: ${DB_EXTRA_WARNING_FLAGS} (not errors)")
endif()

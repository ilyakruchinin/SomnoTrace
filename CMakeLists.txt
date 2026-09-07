# SomnoTrace - top-level ESP-IDF project file
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS ${EXTRA_COMPONENT_DIRS} "third_party/esp-idf-ftpServer" "third_party/libsmb2" "components/uploader")

# --- Derive firmware version from git tags ----------------------------------
# Same logic as scripts/build-dist.sh:
#   on a tag:           v1.2.3
#   N commits after:    v1.2.3-dev+N
#   uncommitted changes: ...+dirty
#   no tags:            0.0.0+<count>.g<sha>[+dirty]
#
# Inside Docker the project dir may have different ownership, causing
# git to refuse operations ("dubious ownership").  Add a safe.directory
# exception so git works regardless of UID/GID mapping.
execute_process(
    COMMAND git config --global --add safe.directory ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_QUIET ERROR_QUIET)

execute_process(
    COMMAND git describe --tags --long --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE _git_describe
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _git_result)

if(_git_result EQUAL 0)
    set(_dirty "")
    if(_git_describe MATCHES "-dirty$")
        set(_dirty "+dirty")
        string(REGEX REPLACE "-dirty$" "" _git_describe "${_git_describe}")
    endif()
    string(REGEX REPLACE "-g[0-9a-f]+$" "" _git_describe "${_git_describe}")
    string(REGEX REPLACE "^.*-([0-9]+)$" "\\1" _commits "${_git_describe}")
    string(REGEX REPLACE "-${_commits}$" "" _tag "${_git_describe}")
    if(_commits STREQUAL "0")
        set(FIRMWARE_VERSION "${_tag}${_dirty}")
    else()
        set(FIRMWARE_VERSION "${_tag}-dev+${_commits}${_dirty}")
    endif()
else()
    execute_process(
        COMMAND git rev-list --count HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        OUTPUT_VARIABLE _count
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _count_result)
    if(NOT _count_result EQUAL 0)
        set(_count "0")
    endif()
    execute_process(
        COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        OUTPUT_VARIABLE _sha
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _sha_result)
    if(NOT _sha_result EQUAL 0)
        set(_sha "nogit")
    endif()
    set(FIRMWARE_VERSION "0.0.0+${_count}.g${_sha}")
endif()

message(STATUS "Firmware version: ${FIRMWARE_VERSION}")

# PROJECT_VER is an ESP-IDF-specific variable picked up by esp_app_desc
# to populate esp_app_desc_t.version at runtime.  Set it before project()
# so it's available to all components.  Do NOT pass it as VERSION to
# project() — CMake requires semantic version format (x.y.z only).
# If build-dist.sh wrote a stamp file with the authoritative version, use it
# instead of the git-derived value.  The stamp file triggers a reconfigure
# whenever the version changes (CMAKE_CONFIGURE_DEPENDS), which the .git/HEAD
# approach could not do reliably (HEAD content doesn't change on new commits
# when on a branch).
if(EXISTS "${CMAKE_BINARY_DIR}/version.stamp")
    file(STRINGS "${CMAKE_BINARY_DIR}/version.stamp" _stamp_ver LIMIT_COUNT 1)
    if(_stamp_ver)
        set(FIRMWARE_VERSION "${_stamp_ver}")
        message(STATUS "Firmware version (from stamp): ${FIRMWARE_VERSION}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_BINARY_DIR}/version.stamp")
endif()

set(PROJECT_VER "${FIRMWARE_VERSION}")

# ── Source repository stamp ─────────────────────────────────────────────────
# WHICH REPOSITORY THIS BINARY WAS BUILT FROM, decided at build time rather than
# written into the source. A fork that builds through its own CI stamps its own
# slug without anyone having to remember to: GITHUB_REPOSITORY is set by GitHub
# Actions for every run, in every fork, and cannot be inherited by accident.
#
# ⚠️ THIS IS NOT AN AUTHENTICITY CONTROL and must never be presented as one. Any
# string compiled into a binary can be copied verbatim by anyone rebuilding it.
# It answers "where did this build come from?" honestly for honest builds, which
# is what makes the GPLv3 §7 attribution easy to comply with rather than a chore.
# Proving origin needs a signature over the artifact, which is a separate matter.
if(DEFINED ENV{GITHUB_REPOSITORY})
    set(SOURCE_REPO "$ENV{GITHUB_REPOSITORY}")
else()
    set(SOURCE_REPO "local build")
endif()
message(STATUS "Source repository: ${SOURCE_REPO}")
add_compile_definitions(SNT_SOURCE_REPO="${SOURCE_REPO}")

# The project this source descends from. Unlike SOURCE_REPO above this is NOT read
# from the environment: it travels IN THE TREE, so a fork inherits it by copying the
# code and would have to edit this line deliberately to remove it. That is the point.
# SOURCE_REPO alone answers "where was this built?", which leaves a fork's portal
# naming a repository the reader has no reference point for; this answers "what is it
# a fork OF?", which is the attribution NOTICE already requires be preserved.
set(UPSTREAM_REPO "ilyakruchinin/SomnoTrace")
add_compile_definitions(SNT_UPSTREAM_REPO="${UPSTREAM_REPO}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(somnotrace)

# The 7-inch UI creates a much larger LVGL object tree than the original
# display. Scope its allocator to PSRAM first so small widgets and copied text
# do not consume the internal heap needed by Wi-Fi, BLE and RTOS stacks.
if(CONFIG_SOMNOTRACE_BOARD_WAVESHARE_7B)
    idf_component_get_property(_somnotrace_lvgl_lib lvgl__lvgl COMPONENT_LIB)
    target_include_directories(${_somnotrace_lvgl_lib} PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/main")
    target_compile_definitions(${_somnotrace_lvgl_lib} PRIVATE
        LV_MEM_CUSTOM_INCLUDE="somnotrace_lvgl_psram.h"
        LV_MEM_CUSTOM_ALLOC=somnotrace_lvgl_alloc
        LV_MEM_CUSTOM_FREE=somnotrace_lvgl_free
        LV_MEM_CUSTOM_REALLOC=somnotrace_lvgl_realloc)
endif()

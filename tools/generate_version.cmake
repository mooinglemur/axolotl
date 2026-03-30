set(CMAKE_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/..")

execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    RESULT_VARIABLE GIT_RESULT
    OUTPUT_VARIABLE GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(NOT GIT_RESULT EQUAL 0)
    set(GIT_HASH "unknown")
endif()

if(NOT AXOLOTL_IGNORE_DIRTY)
    # Check for dirty state
    execute_process(
        COMMAND git status --porcelain
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE GIT_STATUS
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT "${GIT_STATUS}" STREQUAL "")
        set(GIT_HASH "${GIT_HASH}+")
    endif()
endif()

set(VERSION_H_CONTENT "
#pragma once

#define AXOLOTL_VERSION_MAJOR 0
#define AXOLOTL_VERSION_MINOR 1
#define AXOLOTL_VERSION_PATCH 1
#define AXOLOTL_VERSION_STRING \"0.1.1\"
#define GIT_HASH \"${GIT_HASH}\"
")

set(VERSION_H_PATH "${CMAKE_SOURCE_DIR}/src/version.h")

if(EXISTS "${VERSION_H_PATH}")
    file(READ "${VERSION_H_PATH}" OLD_CONTENT)
else()
    set(OLD_CONTENT "")
endif()

if(NOT "${VERSION_H_CONTENT}" STREQUAL "${OLD_CONTENT}")
    file(WRITE "${VERSION_H_PATH}" "${VERSION_H_CONTENT}")
    message(STATUS "Updated ${VERSION_H_PATH} with GIT_HASH=${GIT_HASH}")
endif()

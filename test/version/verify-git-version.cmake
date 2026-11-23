if (NOT PROJECT_SOURCE_DIR OR NOT PROJECT_VERSION OR NOT WORK_DIR)
    message(FATAL_ERROR "PROJECT_SOURCE_DIR, PROJECT_VERSION, and WORK_DIR are required")
endif ()

find_package(Git REQUIRED)

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/refs")

execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${PROJECT_SOURCE_DIR}" rev-parse HEAD
        OUTPUT_VARIABLE HEAD_SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY
)
execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${PROJECT_SOURCE_DIR}" rev-parse --git-common-dir
        OUTPUT_VARIABLE GIT_COMMON_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY
)
cmake_path(ABSOLUTE_PATH GIT_COMMON_DIR BASE_DIRECTORY "${PROJECT_SOURCE_DIR}" NORMALIZE)

file(WRITE "${WORK_DIR}/HEAD" "${HEAD_SHA}\n")
file(CREATE_LINK "${GIT_COMMON_DIR}/objects" "${WORK_DIR}/objects" SYMBOLIC)

set(ENV{GIT_DIR} "${WORK_DIR}")
set(ENV{GIT_WORK_TREE} "${PROJECT_SOURCE_DIR}")
execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${PROJECT_SOURCE_DIR}" rev-parse --short HEAD
        OUTPUT_VARIABLE SHORT_SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY
)

set(CMAKE_SOURCE_DIR "${PROJECT_SOURCE_DIR}")
include("${PROJECT_SOURCE_DIR}/cmake/GitVersion.cmake")

unset(ENV{GIT_DIR})
unset(ENV{GIT_WORK_TREE})
file(REMOVE_RECURSE "${WORK_DIR}")

if (NOT GIT_AVAILABLE)
    message(FATAL_ERROR "Git metadata was not retrieved from a detached checkout without refs")
endif ()
if (NOT GIT_DESCRIBE_CLEAN STREQUAL "g${SHORT_SHA}")
    message(FATAL_ERROR
            "Expected commit fallback g${SHORT_SHA}, got '${GIT_DESCRIBE_CLEAN}'")
endif ()

message(STATUS "Detached checkout without refs produced ${GIT_DESCRIBE_CLEAN}")

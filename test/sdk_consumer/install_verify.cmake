cmake_minimum_required(VERSION 3.28)
if(NOT DEFINED BUILD_DIR OR NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "Pass BUILD_DIR and WORK_DIR")
endif()
# Concurrent CTest processes must never share a mutable install/relocation tree.
string(RANDOM LENGTH 32 ALPHABET 0123456789abcdef run_id)
set(WORK_DIR "${WORK_DIR}/run-${run_id}")
set(SDK_PREFIX "${WORK_DIR}/sdk")
foreach(component dev glaze_Development)
    execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${SDK_PREFIX}"
        --component "${component}" RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "Install failed: ${output}${error}")
    endif()
endforeach()
include("${CMAKE_CURRENT_LIST_DIR}/verify.cmake")

# Keep failed runs for diagnosis; only a successful invocation removes its own tree.
file(REMOVE_RECURSE "${WORK_DIR}")

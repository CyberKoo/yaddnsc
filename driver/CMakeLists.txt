cmake_minimum_required(VERSION 3.14 FATAL_ERROR)
project(drivers)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/objs/driver)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/objs/driver)
include_directories("${PROJECT_SOURCE_DIR}/../include")

add_custom_target(drivers)

# On macOS the linker requires exported symbols from shared modules to be
# resolved at link time by default, but driver modules resolve them from
# the main executable at dlopen time.  Allow undefined symbols at link
# time so that symbols such as CoreLogger::* can be resolved at runtime.
if (APPLE)
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,-undefined,dynamic_lookup")
endif ()

# add all drivers
file(GLOB DRIVER_DIRS "./**" LIST_DIRECTORIES true)
foreach (DIR_NAME ${DRIVER_DIRS})
    IF (IS_DIRECTORY ${DIR_NAME})
        get_filename_component(DRIVER_NAME ${DIR_NAME} NAME)
        message(STATUS "Build driver ${DRIVER_NAME}")

        add_subdirectory(${DIR_NAME})
        set_target_properties(${DRIVER_NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)
        set_target_properties(${DRIVER_NAME} PROPERTIES PREFIX "")
        add_dependencies(drivers ${DRIVER_NAME})
    ENDIF ()
endforeach ()

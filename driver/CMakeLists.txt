cmake_minimum_required(VERSION 3.14 FATAL_ERROR)
project(drivers)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/objs/driver)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/objs/driver)
include_directories("${PROJECT_SOURCE_DIR}/../include")

# add all drivers
file(GLOB DRIVERS "./**" LIST_DIRECTORIES true)
foreach(dir ${DRIVERS})
    IF(IS_DIRECTORY ${dir})
        get_filename_component(DIR_NAME ${dir} NAME)
        message(STATUS "Build driver ${dir}")

        add_subdirectory(${dir})
        target_link_libraries(${DIR_NAME} PRIVATE spdlog::spdlog fmt::fmt nlohmann_json::nlohmann_json)
    ENDIF()
endforeach()

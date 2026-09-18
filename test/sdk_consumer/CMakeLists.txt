cmake_minimum_required(VERSION 3.28 FATAL_ERROR)
project(yaddnsc_sdk_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# This project intentionally lives outside the yaddnsc build graph when it is
# configured in CI/manual validation.  It proves the installed package—not
# source-tree include paths or CPM targets—supplies every supported SDK edge.
find_package(yaddnsc CONFIG REQUIRED COMPONENTS plugin_sdk plugin_sdk_xml plugin_crypto)

add_library(example_c_abi MODULE c_abi_driver.cpp)
target_link_libraries(example_c_abi PRIVATE yaddnsc::plugin_sdk)

add_library(example_http MODULE http_driver.cpp)
target_link_libraries(example_http PRIVATE yaddnsc::plugin_sdk)

add_library(example_xml MODULE xml_driver.cpp)
target_link_libraries(example_xml PRIVATE yaddnsc::plugin_sdk_xml)

add_library(example_crypto MODULE crypto_driver.cpp)
target_link_libraries(example_crypto PRIVATE yaddnsc::plugin_sdk yaddnsc::plugin_crypto)

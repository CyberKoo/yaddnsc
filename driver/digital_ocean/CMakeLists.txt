cmake_minimum_required(VERSION 3.14)
project("digital ocean driver")

add_library(digital_ocean MODULE digital_ocean.cpp digital_ocean.h)
target_link_libraries(digital_ocean PRIVATE spdlog::spdlog fmt::fmt nlohmann_json::nlohmann_json)
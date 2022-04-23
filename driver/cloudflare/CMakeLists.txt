cmake_minimum_required(VERSION 3.14)
project("cloudflare driver")

add_library(cloudflare MODULE cloudflare.cpp cloudflare.h)
target_link_libraries(cloudflare PRIVATE spdlog::spdlog fmt::fmt nlohmann_json::nlohmann_json string_util)
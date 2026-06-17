cmake_minimum_required(VERSION 3.14)
project("cloudflare driver")

add_library(cloudflare MODULE cloudflare.cpp)
target_link_libraries(cloudflare PRIVATE spdlog::spdlog yaddnsc_fmt glaze::glaze string_util)

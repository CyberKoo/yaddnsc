cmake_minimum_required(VERSION 3.14)
project("simple driver")

add_library(simple MODULE simple.cpp simple.h)
target_link_libraries(simple PRIVATE spdlog::spdlog fmt::fmt)
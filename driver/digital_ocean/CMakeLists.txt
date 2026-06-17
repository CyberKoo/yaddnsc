cmake_minimum_required(VERSION 3.14)
project("digital ocean driver")

add_library(digital_ocean MODULE digital_ocean.cpp)
target_link_libraries(digital_ocean PRIVATE yaddnsc_fmt glaze::glaze)

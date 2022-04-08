cmake_minimum_required(VERSION 3.14)
project("digital ocean driver")

add_library(digital_ocean MODULE digital_ocean.cpp digital_ocean.h ../driver.h)
set_target_properties(digital_ocean PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(digital_ocean PROPERTIES PREFIX "")

cmake_minimum_required(VERSION 3.14)
project("simple driver")

add_library(simple MODULE simple.cpp simple.h ../driver.h)
set_target_properties(simple PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(simple PROPERTIES PREFIX "")

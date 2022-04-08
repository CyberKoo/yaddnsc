cmake_minimum_required(VERSION 3.14)
project("cloudflare driver")

add_library(cloudflare MODULE cloudflare.cpp cloudflare.h ../driver.h)
set_target_properties(cloudflare PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(cloudflare PROPERTIES PREFIX "")

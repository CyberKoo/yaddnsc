cmake_minimum_required(VERSION 3.14)
project("DNSPod driver")

add_library(dnspod MODULE dnspod.cpp)
target_link_libraries(dnspod PRIVATE yaddnsc_fmt glaze::glaze string_util)

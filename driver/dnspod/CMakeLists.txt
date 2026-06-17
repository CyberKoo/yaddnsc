cmake_minimum_required(VERSION 3.14)
project("DNSPod driver")

add_library(dnspod MODULE dnspod.cpp dnspod.h)
target_link_libraries(dnspod PRIVATE spdlog::spdlog yaddnsc_fmt glaze::glaze string_util)

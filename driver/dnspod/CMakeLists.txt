cmake_minimum_required(VERSION 3.14)
project("DNSPod driver")

add_library(dnspod MODULE dnspod.cpp dnspod.h)
target_link_libraries(dnspod PRIVATE spdlog::spdlog fmt::fmt nlohmann_json::nlohmann_json string_util)
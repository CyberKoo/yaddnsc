cmake_minimum_required(VERSION 3.14)
project("cloudflare driver")

add_library(cloudflare MODULE cloudflare.cpp cloudflare.h ../driver.h)
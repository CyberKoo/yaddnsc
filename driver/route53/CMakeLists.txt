find_package(LibXml2 QUIET)

if(NOT LibXml2_FOUND)
  message(WARNING "libxml2 not found — route53 driver will not be built")
  return()
endif()

add_library(route53 MODULE route53.cpp)
target_link_libraries(route53 PRIVATE yaddnsc_fmt LibXml2::LibXml2)
message(STATUS "libxml2 found — building route53 driver")

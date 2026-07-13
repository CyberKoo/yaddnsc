find_package(LibXml2 QUIET)

if(NOT LibXml2_FOUND)
  message(WARNING "libxml2 not found — namecheap driver will not be built")
  return()
endif()

add_library(namecheap MODULE namecheap.cpp)
target_link_libraries(namecheap PRIVATE yaddnsc_fmt LibXml2::LibXml2)
message(STATUS "libxml2 found — building namecheap driver")

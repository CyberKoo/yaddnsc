# libxml2 is an opt-in SDK component: cmake/PluginSdk.cmake performs the single
# find_package(LibXml2) and defines yaddnsc_plugin_sdk_xml when it is available.
if(NOT YADDNSC_HAS_SDK_XML_COMPONENT)
  message(WARNING "libxml2 not found — namecheap driver will not be built")
  return()
endif()

add_library(namecheap MODULE namecheap.cpp)
target_link_libraries(namecheap PRIVATE yaddnsc_plugin_sdk_xml)
message(STATUS "libxml2 found — building namecheap driver")

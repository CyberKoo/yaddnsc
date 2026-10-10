# libxml2 is an opt-in SDK component: cmake/PluginSdk.cmake performs the single
# find_package(LibXml2) and defines yaddnsc_plugin_sdk_xml when it is available.
if(NOT YADDNSC_HAS_SDK_XML_COMPONENT)
  message(WARNING "libxml2 not found — route53 driver will not be built")
  return()
endif()

add_library(route53 MODULE route53.cpp)
target_link_libraries(route53 PRIVATE yaddnsc_plugin_sdk_xml yaddnsc_plugin_support_crypto)
message(STATUS "libxml2 found — building route53 driver")

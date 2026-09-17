add_library(porkbun MODULE porkbun.cpp)
target_link_libraries(porkbun PRIVATE yaddnsc_plugin_sdk)

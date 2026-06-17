add_library(dnspod MODULE dnspod.cpp)
target_link_libraries(dnspod PRIVATE yaddnsc_fmt glaze::glaze)

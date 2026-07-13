add_library(duckdns MODULE duckdns.cpp)
target_link_libraries(duckdns PRIVATE yaddnsc_fmt)

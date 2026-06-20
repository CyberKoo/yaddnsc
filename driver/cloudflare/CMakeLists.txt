add_library(cloudflare MODULE cloudflare.cpp)
target_link_libraries(cloudflare PRIVATE yaddnsc_fmt)

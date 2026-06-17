add_library(digital_ocean MODULE digital_ocean.cpp)
target_link_libraries(digital_ocean PRIVATE yaddnsc_fmt glaze::glaze)

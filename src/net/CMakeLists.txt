add_library(am_net STATIC
    catalog_parser.cpp
    curl_reactor_bridge.cpp
)
target_link_libraries(am_net PUBLIC am_pal am_common)
am_enable_warnings(am_net)

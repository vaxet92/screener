# config/CMakeLists.txt
#
# A plain struct plus flag parsing. No JSON, so no simdjson: there is no
# config file - one venue, one market, one timeframe pair, nothing an operator
# must choose before the first run.
add_library(config STATIC
    config.cpp
)

target_include_directories(config PUBLIC
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(config PUBLIC
    types
)

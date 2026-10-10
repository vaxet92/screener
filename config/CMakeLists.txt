# config/CMakeLists.txt
#
# A plain struct plus flag parsing, plus an optional config.json for the
# filter thresholds - simdjson is already a project dependency (md_provider
# uses it for the wire format), so reading five scalar fields reuses it
# rather than adding a second JSON library.
find_package(simdjson CONFIG REQUIRED)

add_library(config STATIC
    config.cpp
)

target_include_directories(config PUBLIC
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(config PUBLIC
    types
    simdjson::simdjson
)

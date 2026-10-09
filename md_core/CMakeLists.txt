# md_core/CMakeLists.txt
#
# The domain: candle state, the indicators, and the ACTIVE/INACTIVE decision.
# Depends on types and logger only - never on md_provider, so no venue
# protocol detail can reach it.
add_library(md_core STATIC
    core.cpp
)

target_include_directories(md_core PUBLIC
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(md_core PUBLIC
    types
    logger
)

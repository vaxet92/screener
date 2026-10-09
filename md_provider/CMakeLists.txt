# md_provider/CMakeLists.txt
#
# Everything that knows Bybit's wire format, plus the WebSocket and REST
# plumbing. Links md_core for the Candle it produces, never the other way
# round.

find_package(Boost REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
find_package(simdjson CONFIG REQUIRED)

add_library(md_provider STATIC
    md_provider.cpp
    bybit_parser.cpp
    rest.cpp
    async_rest.cpp
    ws/ws.cpp
)

target_include_directories(md_provider PUBLIC
    ${PROJECT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/ws
)

target_link_libraries(md_provider PUBLIC
    types
    md_core
    logger
    config
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
    simdjson::simdjson
)

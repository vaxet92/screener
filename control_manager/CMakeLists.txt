# control_manager/CMakeLists.txt
#
# Startup, REST, and the single io_context every other component runs on.
#
# The library and the executable are split so unit tests can link the
# lifecycle code without pulling in main().

find_package(Boost REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)

add_library(control_manager STATIC
    control_manager.cpp
)

target_include_directories(control_manager PUBLIC
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(control_manager PUBLIC
    types
    config
    logger
    md_core
    md_provider
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
)

add_executable(screener
    main.cpp
)

target_link_libraries(screener PRIVATE
    control_manager
)

# tests/unit_tests/CMakeLists.txt
find_package(GTest REQUIRED)

add_executable(unit_tests
    src/test_indicators.cpp
    src/test_htf_aggregator.cpp
    src/test_core.cpp
    src/test_bybit_parser.cpp
    src/test_rate_limiter.cpp
    src/test_spsc_queue.cpp
    src/test_telegram_notifier.cpp
    src/test_config.cpp
)

# md_core, config and md_provider are NOT listed: notifier links md_provider
# PUBLIC, which links md_core and config PUBLIC, so naming any of them here
# too makes the linker see the same archive twice ("ignoring duplicate
# libraries"). One edge per dependency.
target_link_libraries(unit_tests PRIVATE
    GTest::gtest_main
    notifier
)

# The repo root, so the manual Telegram smoke test can find .env no matter
# which directory ctest runs the binary from.
target_compile_definitions(unit_tests PRIVATE
    SCREENER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

include(GoogleTest)

# Under the thread sanitizer, point TSan at the checked-in suppressions file
# (see tsan.supp - currently empty on purpose). No effect on the Release/ASan
# builds, which never set SANITIZER=thread.
#
# The MVP has no threads at all, so the only TSan target today is
# test_spsc_queue - which is Phase 1 work kept building so the queue is ready
# when the parse moves off the io_context thread.
if(SANITIZER STREQUAL "thread")
    gtest_discover_tests(unit_tests
        PROPERTIES ENVIRONMENT "TSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/tsan.supp")
else()
    gtest_discover_tests(unit_tests)
endif()

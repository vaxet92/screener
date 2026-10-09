cmake_minimum_required(VERSION 3.20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# vcpkg integration (only if not already set)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file")
endif()

project(screener CXX)

# NOTE: configuring prints a CMP0167 dev warning from
# vcpkg_installed/.../boost/vcpkg-cmake-wrapper.cmake. It is vcpkg's wrapper
# calling the removed FindBoost module, inside vcpkg's own policy scope -
# setting CMP0167 here does NOT suppress it (tried). Boost still resolves
# through BoostConfig.cmake, which is what we want. Harmless; not ours to fix.


set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -Werror from the start, not "once it is clean". Every warning this project
# would hit is a real defect class in it: -Wswitch catches an unhandled
# TimeFrame or Arrival enumerator, -Wconversion-adjacent warnings catch a
# silent narrowing of a scaled int64, and -Wunused catches an indicator field
# that is declared and never fed.
#
# Set before any add_subdirectory so the flags reach every target.
add_compile_options(-Wall -Wextra -Werror)

option(BUILD_TESTS "Build unit tests" ON)
option(BUILD_BENCHMARKS "Build latency micro-benchmarks" OFF)

# Sanitizers. address and thread are MUTUALLY EXCLUSIVE - clang rejects
# -fsanitize=address,thread - so they get separate build directories
# (see CMakePresets.json). UBSan is compatible with ASan and rides along
# at no extra cost.
#
# Set here, before any add_subdirectory, so the flags reach every target.
# Compile AND link flags are both required: a sanitizer that is compiled in
# but not linked fails at link time, and one that is linked but not compiled
# silently detects nothing.
#
# NOTE: LeakSanitizer is unsupported on macOS arm64, so the address build
# finds memory ERRORS but not leaks.
#
# NOTE: TSan cannot see inside uninstrumented vcpkg libraries (Boost,
# OpenSSL), so it may report false positives from their internals. Our own
# code is the signal; unit_tests is where that signal is cleanest.
#
# The MVP is single-threaded, so there is nothing of ours for TSan to find
# yet. The thread build exists for SpscQueue, which Phase 1 needs when the
# parse moves off the io_context thread.
set(SANITIZER "none" CACHE STRING "Sanitizer: none | address | thread")
set_property(CACHE SANITIZER PROPERTY STRINGS none address thread)

if(SANITIZER STREQUAL "address")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
elseif(SANITIZER STREQUAL "thread")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
elseif(NOT SANITIZER STREQUAL "none")
    message(FATAL_ERROR "SANITIZER must be none, address, or thread (got '${SANITIZER}')")
endif()

add_subdirectory(types)
add_subdirectory(config)
add_subdirectory(logger)
add_subdirectory(md_core)
add_subdirectory(md_provider)
add_subdirectory(control_manager)


if (BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests/unit_tests)
endif()
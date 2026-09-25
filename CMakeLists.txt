cmake_minimum_required(VERSION 3.16)

project(OrderBook_Cpp
    VERSION 6.0.0
    DESCRIPTION "Limit order book – from naive to hardware-sympathetic"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Release when no build type is given (single-config generators).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

# ── Options ──────────────────────────────────────────────────────────────────
option(OB_BUILD_TESTS     "Build the test suite"                ON)
option(OB_BUILD_BENCH     "Build the micro-benchmark"           ON)
option(OB_BUILD_PIPELINE  "Build the SPSC pipeline engine"      ON)
option(OB_ENABLE_SANITIZERS "Build with ASan+UBSan (tests)"     OFF)
option(OB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# ── Compiler flags ───────────────────────────────────────────────────────────
#
# Everything below is guarded by feature detection so the tree builds with
# GCC, Clang and Apple Clang on x86-64 AND arm64 (Apple Silicon).  In
# particular, `-march=native` is NOT portable to Apple Silicon toolchains,
# so the arch flag is probed per-platform.
include(CheckCXXCompilerFlag)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(OB_WARNINGS_AS_ERRORS)
        add_compile_options(-Werror)
    endif()

    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        # Optimisation flags come from CMake's Release profile (-O3); add an
        # architecture flag only when the compiler accepts it.
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
            check_cxx_compiler_flag("-mcpu=native" OB_HAS_MCPU_NATIVE)
            if(OB_HAS_MCPU_NATIVE)
                add_compile_options(-mcpu=native)
            endif()
        else()
            check_cxx_compiler_flag("-march=native" OB_HAS_MARCH_NATIVE)
            if(OB_HAS_MARCH_NATIVE)
                add_compile_options(-march=native)
            endif()
        endif()
    endif()
elseif(MSVC)
    add_compile_options(/W4)
endif()

if(OB_ENABLE_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    else()
        message(WARNING "Sanitizers requested but unsupported for this toolchain")
    endif()
endif()

# ── Threading (the pipeline uses std::thread) ────────────────────────────────
find_package(Threads REQUIRED)

# ── Core library ─────────────────────────────────────────────────────────────
add_library(orderbook STATIC src/orderbook.cpp)
target_include_directories(orderbook PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(orderbook PUBLIC Threads::Threads)
add_library(ob::orderbook ALIAS orderbook)

# ── Pipeline engine (producer → SPSC ring → matching engine) ─────────────────
if(OB_BUILD_PIPELINE)
    add_executable(matching_engine src/pipeline_main.cpp)
    target_link_libraries(matching_engine PRIVATE ob::orderbook)
endif()

# ── Micro-benchmark ──────────────────────────────────────────────────────────
if(OB_BUILD_BENCH)
    add_executable(ob_bench bench/benchmark.cpp)
    target_link_libraries(ob_bench PRIVATE ob::orderbook)
endif()

# ── Tests ────────────────────────────────────────────────────────────────────
if(OB_BUILD_TESTS)
    enable_testing()
    add_executable(ob_tests
        tests/test_main.cpp
        tests/test_bitset.cpp
        tests/test_flat_map.cpp
        tests/test_orderbook.cpp
        tests/test_differential_fuzz.cpp
    )
    target_include_directories(ob_tests PRIVATE tests)
    target_link_libraries(ob_tests PRIVATE ob::orderbook)

    # Tests must run with the sanitizers' runtime loaded correctly.
    add_test(NAME orderbook_tests COMMAND ob_tests)

    add_custom_target(run_tests
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
        DEPENDS ob_tests
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Running order book test suite")
endif()

# ── Convenience: everything ──────────────────────────────────────────────────
add_custom_target(all_apps
    DEPENDS ob_tests
    COMMENT "Building all applications")

if(OB_BUILD_BENCH)
    add_dependencies(all_apps ob_bench)
endif()
if(OB_BUILD_PIPELINE)
    add_dependencies(all_apps matching_engine)
endif()

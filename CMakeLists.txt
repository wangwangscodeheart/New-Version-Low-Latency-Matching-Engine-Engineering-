# ============================================================================
# CMakeLists.txt - Matching Engine Build Configuration
# ============================================================================

cmake_minimum_required(VERSION 3.14)
project(MatchingEngine VERSION 2.0.0 LANGUAGES CXX)

execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE MATCHING_ENGINE_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT MATCHING_ENGINE_GIT_COMMIT)
    set(MATCHING_ENGINE_GIT_COMMIT "unknown")
endif()

find_package(Threads REQUIRED)

# ============================================================================
# Compiler Settings
# ============================================================================

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer (GCC/Clang)" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer (GCC/Clang)" OFF)
option(BENCHMARK_SMOKE "Use a small workload for CI benchmark smoke tests" OFF)

# Compilation Options
if(MSVC)
    add_compile_options(/W4 /utf-8)
    # Prefer the dynamically linked MSVC runtime. The local toolchain does not
    # ship the static C++ runtime library (libcpmt.lib).
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    add_link_options(/NODEFAULTLIB:libcpmt.lib /DEFAULTLIB:msvcprt.lib)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter)
endif()

if(ENABLE_TSAN)
    if(MSVC)
        message(WARNING "TSan is not supported by MSVC; use Clang/GCC")
    else()
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
        add_link_options(-fsanitize=thread)
    endif()
endif()

if(ENABLE_ASAN)
    if(MSVC)
        add_compile_options(/fsanitize=address)
    else()
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()
endif()

if(ENABLE_UBSAN)
    if(MSVC)
        message(WARNING "UBSan is not supported by this MSVC configuration; use Clang/GCC")
    else()
        add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=undefined)
    endif()
endif()

# ============================================================================
# Include Directories
# ============================================================================

include_directories(${CMAKE_SOURCE_DIR}/src)

# ============================================================================
# Main Executable
# ============================================================================

add_executable(matching_engine_demo
    src/main.cpp
)

add_executable(matching_engine_v2_demo
    src/v2_main.cpp
)
target_link_libraries(matching_engine_v2_demo PRIVATE Threads::Threads)

add_executable(matching_engine_market_data_generator
    src/market_data_generator_main.cpp
)

# ============================================================================
# Unit Tests
# ============================================================================

add_executable(matching_engine_unit_tests
    tests/unit_tests.cpp
)

# ============================================================================
# Property Tests
# ============================================================================

add_executable(matching_engine_property_tests
    tests/property_tests.cpp
)

add_executable(matching_engine_differential_tests
    tests/differential_tests.cpp
)

# ============================================================================
# Snapshot Tests
# ============================================================================

add_executable(matching_engine_snapshot_tests
    tests/snapshot_tests.cpp
)

add_executable(matching_engine_market_snapshot_tests
    tests/market_snapshot_tests.cpp
)

add_executable(matching_engine_market_manager_tests
    tests/market_manager_tests.cpp
)

add_executable(matching_engine_event_system_tests
    tests/event_system_tests.cpp
)

add_executable(matching_engine_journal_replay_tests
    tests/journal_replay_tests.cpp
)

add_executable(matching_engine_async_logger_tests
    tests/async_logger_tests.cpp
)
target_link_libraries(matching_engine_async_logger_tests PRIVATE Threads::Threads)

add_executable(matching_engine_monitor_tests
    tests/monitor_tests.cpp
)
target_link_libraries(matching_engine_monitor_tests PRIVATE Threads::Threads)

add_executable(matching_engine_market_data_tests
    tests/market_data_tests.cpp
)
target_link_libraries(matching_engine_market_data_tests PRIVATE Threads::Threads)

add_executable(matching_engine_strategy_tests
    tests/strategy_tests.cpp
)
target_link_libraries(matching_engine_strategy_tests PRIVATE Threads::Threads)

add_executable(matching_engine_async_journal_tests
    tests/async_journal_tests.cpp
)
target_link_libraries(matching_engine_async_journal_tests PRIVATE Threads::Threads)

add_executable(matching_engine_market_data_generator_tests
    tests/market_data_generator_tests.cpp
)
target_link_libraries(matching_engine_market_data_generator_tests PRIVATE Threads::Threads)

add_executable(matching_engine_feed_gateway_tests
    tests/feed_gateway_tests.cpp
)

add_executable(matching_engine_capacity_metrics_tests
    tests/capacity_metrics_tests.cpp
)

add_executable(matching_engine_risk_engine_tests
    tests/risk_engine_tests.cpp
)

add_executable(matching_engine_order_type_tests
    tests/order_type_tests.cpp
)

# ============================================================================
# Benchmarks
# ============================================================================

add_executable(matching_engine_benchmarks
    benchmarks/perf.cpp
)

add_executable(matching_engine_v2_benchmarks
    benchmarks/v2_perf.cpp
)
target_link_libraries(matching_engine_v2_benchmarks PRIVATE Threads::Threads)
string(TOUPPER "${CMAKE_BUILD_TYPE}" MATCHING_ENGINE_BUILD_TYPE_UPPER)
set(MATCHING_ENGINE_ACTIVE_FLAGS
    "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${MATCHING_ENGINE_BUILD_TYPE_UPPER}}")
string(STRIP "${MATCHING_ENGINE_ACTIVE_FLAGS}" MATCHING_ENGINE_ACTIVE_FLAGS)
target_compile_definitions(matching_engine_v2_benchmarks PRIVATE
    MATCHING_ENGINE_GIT_COMMIT="${MATCHING_ENGINE_GIT_COMMIT}"
    MATCHING_ENGINE_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    MATCHING_ENGINE_COMPILER="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}"
    MATCHING_ENGINE_OS="${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_VERSION}"
    MATCHING_ENGINE_COMPILER_FLAGS="${MATCHING_ENGINE_ACTIVE_FLAGS}"
)
if(BENCHMARK_SMOKE)
    target_compile_definitions(matching_engine_v2_benchmarks PRIVATE
        MATCHING_ENGINE_V2_BENCHMARK_PAIRS=100
    )
endif()

# ============================================================================
# CTest Integration
# ============================================================================

include(CTest)
if(BUILD_TESTING)
    add_test(NAME matching_engine_unit COMMAND matching_engine_unit_tests)
    add_test(NAME matching_engine_properties COMMAND matching_engine_property_tests)
    add_test(NAME matching_engine_differential COMMAND matching_engine_differential_tests)
    add_test(NAME matching_engine_snapshot COMMAND matching_engine_snapshot_tests)
    add_test(NAME matching_engine_market_snapshot COMMAND matching_engine_market_snapshot_tests)
    add_test(NAME matching_engine_market_manager COMMAND matching_engine_market_manager_tests)
    add_test(NAME matching_engine_event_system COMMAND matching_engine_event_system_tests)
    add_test(NAME matching_engine_journal_replay COMMAND matching_engine_journal_replay_tests)
    add_test(NAME matching_engine_async_logger COMMAND matching_engine_async_logger_tests)
    add_test(NAME matching_engine_monitor COMMAND matching_engine_monitor_tests)
    add_test(NAME matching_engine_market_data COMMAND matching_engine_market_data_tests)
    add_test(NAME matching_engine_strategy COMMAND matching_engine_strategy_tests)
    add_test(NAME matching_engine_async_journal COMMAND matching_engine_async_journal_tests)
    add_test(NAME matching_engine_market_data_generator COMMAND matching_engine_market_data_generator_tests)
    add_test(NAME matching_engine_feed_gateway COMMAND matching_engine_feed_gateway_tests)
    add_test(NAME matching_engine_capacity_metrics COMMAND matching_engine_capacity_metrics_tests)
    add_test(NAME matching_engine_risk_engine COMMAND matching_engine_risk_engine_tests)
    add_test(NAME matching_engine_order_types COMMAND matching_engine_order_type_tests)
    add_test(NAME matching_engine_demo_integration COMMAND matching_engine_demo)
    add_test(NAME matching_engine_v2_demo_integration COMMAND matching_engine_v2_demo)
endif()

# ============================================================================
# Build Types
# ============================================================================

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
    set(CMAKE_CXX_FLAGS_DEBUG "/Od /DDEBUG /MDd")
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /DNDEBUG /MD")
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "/O2 /Zi /MD")
else()
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native")
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g")
endif()

# ============================================================================
# Installation
# ============================================================================

install(TARGETS 
    matching_engine_demo
    matching_engine_v2_demo
    matching_engine_market_data_generator
    matching_engine_unit_tests
    matching_engine_property_tests
    matching_engine_snapshot_tests
    matching_engine_market_manager_tests
    matching_engine_event_system_tests
    matching_engine_journal_replay_tests
    matching_engine_async_logger_tests
    matching_engine_monitor_tests
    matching_engine_market_data_tests
    matching_engine_strategy_tests
    matching_engine_async_journal_tests
    matching_engine_market_data_generator_tests
    matching_engine_benchmarks
    matching_engine_v2_benchmarks
    DESTINATION bin
)

# ============================================================================
# Summary
# ============================================================================

message(STATUS "")
message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
message(STATUS "  Matching Engine Build Configuration")
message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Standard: C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Compiler: ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "")
message(STATUS "  Targets:")
message(STATUS "    - matching_engine_demo           (Main demo)")
message(STATUS "    - matching_engine_v2_demo        (V2 infrastructure demo)")
message(STATUS "    - matching_engine_market_data_generator (synthetic CSV generator)")
message(STATUS "    - matching_engine_unit_tests     (Unit tests)")
message(STATUS "    - matching_engine_property_tests (Property tests)")
message(STATUS "    - matching_engine_snapshot_tests (Snapshot tests)")
message(STATUS "    - matching_engine_benchmarks     (Performance)")
message(STATUS "    - matching_engine_v2_benchmarks  (V2 routed event path)")
message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
message(STATUS "")

# ============================================================================
# CMakeLists.txt - Matching Engine Build Configuration
# ============================================================================

cmake_minimum_required(VERSION 3.14)
project(MatchingEngine VERSION 1.0.0 LANGUAGES CXX)

# ============================================================================
# Compiler Settings
# ============================================================================

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer (GCC/Clang)" OFF)

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

# ============================================================================
# Snapshot Tests
# ============================================================================

add_executable(matching_engine_snapshot_tests
    tests/snapshot_tests.cpp
)

# ============================================================================
# Benchmarks
# ============================================================================

add_executable(matching_engine_benchmarks
    benchmarks/perf.cpp
)

# ============================================================================
# CTest Integration
# ============================================================================

include(CTest)
if(BUILD_TESTING)
    add_test(NAME matching_engine_unit COMMAND matching_engine_unit_tests)
    add_test(NAME matching_engine_properties COMMAND matching_engine_property_tests)
    add_test(NAME matching_engine_snapshot COMMAND matching_engine_snapshot_tests)
    add_test(NAME matching_engine_demo_integration COMMAND matching_engine_demo)
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
    matching_engine_unit_tests
    matching_engine_property_tests
    matching_engine_snapshot_tests
    matching_engine_benchmarks
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
message(STATUS "    - matching_engine_unit_tests     (Unit tests)")
message(STATUS "    - matching_engine_property_tests (Property tests)")
message(STATUS "    - matching_engine_snapshot_tests (Snapshot tests)")
message(STATUS "    - matching_engine_benchmarks     (Performance)")
message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
message(STATUS "")

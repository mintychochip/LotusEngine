cmake_minimum_required(VERSION 3.10)
project(lotus)

# Enable testing
enable_testing()

# Add main program
add_executable(lotus main.cc)
target_include_directories(lotus PRIVATE core)
target_include_directories(lotus PRIVATE ecs)

# Add GoogleTest
add_subdirectory(googletest)

# Collect all .cc test files in the test/ folder
file(GLOB TEST_SOURCES test/*.cc)

# Add the test executable with all collected sources
add_executable(unit_tests ${TEST_SOURCES})

# Include the core/ directory for headers like "core/math.hpp"
target_include_directories(unit_tests PRIVATE core)

# Link GoogleTest libraries
target_link_libraries(unit_tests gtest gtest_main)

# Register tests with CTest
include(GoogleTest)
gtest_discover_tests(unit_tests)
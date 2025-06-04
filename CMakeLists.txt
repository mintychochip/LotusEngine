cmake_minimum_required(VERSION 3.10)
project(lotus)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Add GLFW (as submodule or dependency)
add_subdirectory(dependencies/glfw)

# Find OpenGL
find_package(OpenGL REQUIRED)

# === Engine Library ===

# Gather all implementation files from src/, excluding app/main.cc
file(GLOB_RECURSE LOTUS_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(REMOVE_ITEM LOTUS_SOURCES "${CMAKE_SOURCE_DIR}/src/main.cpp" "${CMAKE_SOURCE_DIR}/src/main.cc") # Just in case

add_library(lotus_engine STATIC ${LOTUS_SOURCES} src/glad.c)

target_include_directories(lotus_engine PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(lotus_engine
    glfw
    OpenGL::GL
)

# === Main Executable ===

# Replace with app/main.cpp or app/main.cc
add_executable(lotus app/main.cpp)

target_link_libraries(lotus
    lotus_engine
)

set_target_properties(lotus PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# === Runtime Assets ===
file(COPY ${CMAKE_SOURCE_DIR}/shaders DESTINATION ${CMAKE_BINARY_DIR})
file(COPY ${CMAKE_SOURCE_DIR}/assets DESTINATION ${CMAKE_BINARY_DIR})

# === Testing ===
include(CTest)
enable_testing()

add_subdirectory(dependencies/googletest)

# Gather test files
file(GLOB_RECURSE LOTUS_TEST_SOURCES CONFIGURE_DEPENDS test/*.cpp)

# Only add tests if sources are found
if (LOTUS_TEST_SOURCES)
    add_executable(lotus_tests ${LOTUS_TEST_SOURCES})

    target_link_libraries(lotus_tests
        lotus_engine
        gtest
        gtest_main
    )

    target_include_directories(lotus_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/test
    )

    set_target_properties(lotus_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )

    add_test(NAME LotusTests COMMAND lotus_tests)

    # Optional: copy assets to test working directory
    file(COPY ${CMAKE_SOURCE_DIR}/shaders DESTINATION ${CMAKE_BINARY_DIR}/test)
    file(COPY ${CMAKE_SOURCE_DIR}/assets DESTINATION ${CMAKE_BINARY_DIR}/test)
endif()

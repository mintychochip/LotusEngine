cmake_minimum_required(VERSION 3.10)
project(lotus)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)

# Add the main executable
add_executable(lotus src/main.cc)

# Include core and ecs directories
target_include_directories(lotus PRIVATE 
    ${CMAKE_SOURCE_DIR}/src
    include 
)

# Add GLFW (assuming it's added as a submodule or installed system-wide)
add_subdirectory(dependencies/glfw)

# Find OpenGL
find_package(OpenGL REQUIRED)

# Add GLAD source file
target_sources(lotus PRIVATE src/glad.c)

# Link the necessary libraries
target_link_libraries(lotus glfw OpenGL::GL)
message(STATUS "Include directories for lotus: ${CMAKE_SOURCE_DIR}/src/core, ${CMAKE_SOURCE_DIR}/src/ecs")

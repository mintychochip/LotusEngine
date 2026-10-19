cmake_minimum_required(VERSION 3.16)
project(lotus)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Set C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(lotus_apply_warning_flags target)
    target_compile_options(${target} PRIVATE -Wall -Wextra)
endfunction()

# Add EnTT (entity-component-system, header-only, submodule)
add_subdirectory(dependencies/entt)

# === Jolt Physics (static lib, submodule) ===
set(JPH_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
add_subdirectory(dependencies/jolt/Build EXCLUDE_FROM_ALL)

# === nlohmann/json (scene serialization, header-only, submodule) ===
add_library(nlohmann_json INTERFACE)
add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
target_include_directories(nlohmann_json INTERFACE
    ${CMAKE_SOURCE_DIR}/dependencies/json/include)

# === Lua (scripting, submodule) ===
file(GLOB LUA_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/dependencies/lua/*.c)
list(REMOVE_ITEM LUA_SOURCES
    "${CMAKE_SOURCE_DIR}/dependencies/lua/lua.c"
    "${CMAKE_SOURCE_DIR}/dependencies/lua/onelua.c")
add_library(lua STATIC ${LUA_SOURCES})
target_include_directories(lua PUBLIC ${CMAKE_SOURCE_DIR}/dependencies/lua)
target_link_libraries(lua PUBLIC m)

include(FindLotusSDL3)

# === Dear ImGui (debug overlay) ===
add_library(imgui STATIC
    dependencies/imgui/imgui.cpp
    dependencies/imgui/imgui_draw.cpp
    dependencies/imgui/imgui_tables.cpp
    dependencies/imgui/imgui_widgets.cpp
    dependencies/imgui/backends/imgui_impl_sdl3.cpp
    dependencies/imgui/backends/imgui_impl_sdlgpu3.cpp
)
target_include_directories(imgui PUBLIC
    ${CMAKE_SOURCE_DIR}/dependencies/imgui
    ${CMAKE_SOURCE_DIR}/dependencies/imgui/backends
)
target_link_libraries(imgui PUBLIC SDL3::SDL3)

# === Engine Library ===

# Gather all implementation files from src/, excluding app/main.cc
file(GLOB_RECURSE LOTUS_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(REMOVE_ITEM LOTUS_SOURCES
    "${CMAKE_SOURCE_DIR}/src/main.cpp"
    "${CMAKE_SOURCE_DIR}/src/main.cc")

add_library(lotus_engine STATIC ${LOTUS_SOURCES})

target_include_directories(lotus_engine PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(lotus_engine
    PUBLIC
    SDL3::SDL3
    EnTT::EnTT
    nlohmann_json::nlohmann_json
    lua
    imgui
    Jolt
)
lotus_apply_warning_flags(lotus_engine)

# === Main Executable ===

add_executable(lotus app/main.cpp)

target_link_libraries(lotus
    lotus_engine
)
lotus_apply_warning_flags(lotus)

set_target_properties(lotus PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# === RPG Executable ===

add_executable(lotus_rpg app/rpg_main.cpp)

target_link_libraries(lotus_rpg
    lotus_engine
)
lotus_apply_warning_flags(lotus_rpg)

set_target_properties(lotus_rpg PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# === Render Benchmark ===
# Deterministic offscreen frame-time benchmark; see autoresearch.sh.

add_executable(lotus_render_bench bench/render_bench.cpp)

target_link_libraries(lotus_render_bench
    lotus_engine
)
lotus_apply_warning_flags(lotus_render_bench)

set_target_properties(lotus_render_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# === Runtime Assets ===
file(COPY ${CMAKE_SOURCE_DIR}/shaders DESTINATION ${CMAKE_BINARY_DIR})
file(COPY ${CMAKE_SOURCE_DIR}/assets DESTINATION ${CMAKE_BINARY_DIR})
file(COPY ${CMAKE_SOURCE_DIR}/scripts DESTINATION ${CMAKE_BINARY_DIR})
if(EXISTS ${CMAKE_SOURCE_DIR}/scenes)
    file(COPY ${CMAKE_SOURCE_DIR}/scenes DESTINATION ${CMAKE_BINARY_DIR})
endif()

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

    lotus_apply_warning_flags(lotus_tests)

    add_test(NAME LotusTests COMMAND lotus_tests)

    # Optional: copy assets to test working directory
    file(COPY ${CMAKE_SOURCE_DIR}/shaders DESTINATION ${CMAKE_BINARY_DIR}/test)
    file(COPY ${CMAKE_SOURCE_DIR}/assets DESTINATION ${CMAKE_BINARY_DIR}/test)
    if(EXISTS ${CMAKE_SOURCE_DIR}/scenes)
        file(COPY ${CMAKE_SOURCE_DIR}/scenes DESTINATION ${CMAKE_BINARY_DIR}/test)
    endif()
endif()

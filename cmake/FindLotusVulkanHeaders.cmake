# Vulkan headers for lotus. The renderer drives Vulkan directly, but only
# through function pointers it resolves at run time (SDL provides the loader:
# SDL_Vulkan_LoadLibrary), so nothing is linked and no Vulkan SDK is needed,
# just the headers. Prefer an installed vulkan-headers, else the copy SDL
# bundles for its own Vulkan support.

find_path(LOTUS_VULKAN_INCLUDE_DIR
    NAMES vulkan/vulkan.h
    PATHS "${CMAKE_SOURCE_DIR}/dependencies/SDL/src/video/khronos")

if(NOT LOTUS_VULKAN_INCLUDE_DIR)
    message(
        FATAL_ERROR
        "Vulkan headers not found. Install them (sudo dnf install "
        "vulkan-headers) or initialize the dependencies/SDL submodule, which "
        "bundles a copy.")
endif()

# VK_NO_PROTOTYPES: every call goes through the loaded function table, so a
# stray vkFoo() fails to compile instead of failing to link.
add_library(Lotus::VulkanHeaders INTERFACE IMPORTED GLOBAL)
set_target_properties(
    Lotus::VulkanHeaders
    PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${LOTUS_VULKAN_INCLUDE_DIR}"
               INTERFACE_COMPILE_DEFINITIONS "VK_NO_PROTOTYPES")

message(STATUS "Lotus: Vulkan headers from ${LOTUS_VULKAN_INCLUDE_DIR}")

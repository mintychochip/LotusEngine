# Vulkan headers for lotus. The renderer drives Vulkan directly, but only
# through function pointers it resolves at run time from the Vulkan loader, so
# nothing is linked and no Vulkan SDK is needed, just the headers. They come
# from the dependencies/vulkan-headers submodule, so a checkout builds without
# any system Vulkan package.

set(LOTUS_VULKAN_INCLUDE_DIR
    "${CMAKE_SOURCE_DIR}/dependencies/vulkan-headers/include")

if(NOT EXISTS "${LOTUS_VULKAN_INCLUDE_DIR}/vulkan/vulkan.h")
    message(
        FATAL_ERROR
        "Vulkan headers not found. Initialize the dependencies/vulkan-headers "
        "submodule (git submodule update --init).")
endif()

# VK_NO_PROTOTYPES: every call goes through the loaded function table, so a
# stray vkFoo() fails to compile instead of failing to link.
add_library(Lotus::VulkanHeaders INTERFACE IMPORTED GLOBAL)
set_target_properties(
    Lotus::VulkanHeaders
    PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${LOTUS_VULKAN_INCLUDE_DIR}"
               INTERFACE_COMPILE_DEFINITIONS "VK_NO_PROTOTYPES")

message(STATUS "Lotus: Vulkan headers from ${LOTUS_VULKAN_INCLUDE_DIR}")

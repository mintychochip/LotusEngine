#include "gpu/vk/vk_loader.hpp"

#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace lotus::vk {

namespace {

#if defined(_WIN32)
constexpr const char* kLoaderNames[] = {"vulkan-1.dll"};
#elif defined(__APPLE__)
constexpr const char* kLoaderNames[] = {"libvulkan.1.dylib", "libvulkan.dylib",
                                        "libMoltenVK.dylib"};
#else
constexpr const char* kLoaderNames[] = {"libvulkan.so.1", "libvulkan.so"};
#endif

void* open_library(const char* name)
{
#if defined(_WIN32)
	return reinterpret_cast<void*>(LoadLibraryA(name));
#else
	return dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
}

void* find_symbol(void* library, const char* name)
{
#if defined(_WIN32)
	return reinterpret_cast<void*>(
	        GetProcAddress(static_cast<HMODULE>(library), name));
#else
	return dlsym(library, name);
#endif
}

void close_library(void* library)
{
#if defined(_WIN32)
	FreeLibrary(static_cast<HMODULE>(library));
#else
	dlclose(library);
#endif
}

} // namespace

Loader::Loader()
{
	std::string tried;
	for (const char* name : kLoaderNames) {
		library_ = open_library(name);
		if (library_ != nullptr) {
			break;
		}
		tried += tried.empty() ? "" : ", ";
		tried += name;
	}
	if (library_ == nullptr) {
		throw std::runtime_error("no Vulkan loader: could not open " + tried);
	}
	get_instance_proc_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
	        find_symbol(library_, "vkGetInstanceProcAddr"));
	if (get_instance_proc_ == nullptr) {
		close_library(library_);
		library_ = nullptr;
		throw std::runtime_error(
		        "no Vulkan loader: the library has no vkGetInstanceProcAddr");
	}
}

Loader::~Loader()
{
	if (library_ != nullptr) {
		close_library(library_);
	}
}

} // namespace lotus::vk

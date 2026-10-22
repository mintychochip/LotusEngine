#pragma once

#include "gpu/vk/vk_api.hpp"

namespace lotus::vk {

/// The system's Vulkan loader, opened at run time: nothing links libvulkan, so
/// a machine without one is an error to report rather than a library missing
/// at startup. Closing it (the destructor) has to wait until everything made
/// through it is destroyed.
class Loader {
public:
	/// Opens the loader and finds vkGetInstanceProcAddr. Throws
	/// std::runtime_error saying what it tried.
	Loader();
	~Loader();
	Loader(const Loader&)            = delete;
	Loader& operator=(const Loader&) = delete;

	PFN_vkGetInstanceProcAddr get_instance_proc() const
	{
		return get_instance_proc_;
	}

private:
	void* library_{nullptr};
	PFN_vkGetInstanceProcAddr get_instance_proc_{nullptr};
};

} // namespace lotus::vk

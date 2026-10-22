#include "lotus/gpu/renderer.hpp"

#include "gpu/vulkan/renderer_vulkan.hpp"

#include <memory>

namespace lotus {

/// The renderer this build links: the Vulkan backend. Never null.
std::unique_ptr<GpuRenderer> create_gpu_renderer()
{
	return std::make_unique<GpuRendererVulkan>();
}

} // namespace lotus

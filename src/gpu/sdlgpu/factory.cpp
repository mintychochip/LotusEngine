#include "lotus/gpu/renderer.hpp"

#include "gpu/sdlgpu/renderer_sdlgpu.hpp"

#include <memory>

namespace lotus {

/// The backend this build selected (LOTUS_GPU_BACKEND). Never null.
std::unique_ptr<GpuRenderer> create_gpu_renderer()
{
	return std::make_unique<GpuRendererSdlGpu>();
}

} // namespace lotus

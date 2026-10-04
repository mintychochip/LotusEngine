#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstdint>

struct SDL_Window;

namespace lotus {

/// SDL3 GPU backend (Vulkan on Linux). Owns device, pipeline, and frame submission.
class GpuRenderer {
public:
	GpuRenderer() = default;
	~GpuRenderer();

	GpuRenderer(const GpuRenderer&)            = delete;
	GpuRenderer& operator=(const GpuRenderer&) = delete;

	/// Creates a Vulkan-backed GPU device and claims \p window for presentation.
	void init(SDL_Window* window, bool debug_device = true);

	SDL_GPUDevice* device() const { return device_; }

	/// Uploads triangle-list vertex data (cube test layout: float3 pos + float3 color).
	void set_vertices(const void* data, uint32_t byte_size);

	/// Acquires swapchain, begins a color-only render pass (clears to \p clear_bgra-ish RGB).
	void begin_frame(SDL_Window* window, float clear_r, float clear_g, float clear_b);

	/// Binds pipeline, sets MVP uniform, draws \p vertex_count vertices.
	void draw(const float* model_view_projection_4x4_column_major,
	          uint32_t vertex_count);

	void end_frame();

	const char* driver_name() const;

private:
	SDL_GPUDevice* device_{nullptr};
	SDL_Window* claimed_window_{nullptr};
	SDL_GPUBuffer* vertex_buffer_{nullptr};
	SDL_GPUGraphicsPipeline* pipeline_{nullptr};
	SDL_GPUCommandBuffer* cmd_{nullptr};
	SDL_GPURenderPass* pass_{nullptr};
};

} // namespace lotus

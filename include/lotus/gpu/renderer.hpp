#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstdint>

struct SDL_Window;

namespace lotus {

/// SDL3 GPU backend (Vulkan on Linux). Owns the device, one graphics pipeline
/// (float3 position + float3 color, mat4 MVP), and a dynamic vertex buffer.
///
/// Frame stages (all on one command buffer):
///   begin_frame()  -> acquire command buffer + swapchain
///   upload_vertices() -> copy pass (must be outside a render pass)
///   begin_scene_pass() / draw() / end_scene_pass()
///   [UI layer may open its own pass here]
///   end_frame()    -> submit
class GpuRenderer {
public:
	GpuRenderer() = default;
	~GpuRenderer();

	GpuRenderer(const GpuRenderer&)            = delete;
	GpuRenderer& operator=(const GpuRenderer&) = delete;

	/// Creates the GPU device, claims \p window, builds the pipeline.
	void init(SDL_Window* window, bool debug_device = true);

	/// Releases all GPU resources. Safe to call more than once.
	void shutdown();

	SDL_GPUDevice* device() const { return device_; }
	const char* driver_name() const;

	// --- frame ---

	void begin_frame(SDL_Window* window, float clear_r, float clear_g,
	                 float clear_b);

	/// Uploads triangle-list vertices (float3 pos + float3 color) for this frame.
	void upload_vertices(const void* data, uint32_t byte_size);

	void begin_scene_pass();
	void draw(const float* mvp_4x4_column_major, uint32_t vertex_count);
	void end_scene_pass();

	void end_frame();

	// --- for UI layers that open their own pass on the same command buffer ---

	/// False when no swapchain texture was available (frame should be skipped).
	bool has_frame() const { return cmd_ != nullptr; }

	SDL_GPUCommandBuffer* command_buffer() const { return cmd_; }
	SDL_GPUTexture* swapchain_texture() const { return swapchain_; }
	SDL_GPUTextureFormat swapchain_format() const { return swapchain_format_; }

	uint32_t vertex_capacity() const { return capacity_; }

	/// Frames actually submitted (excludes skipped/stalled frames).
	uint64_t frames_rendered() const { return frames_rendered_; }

private:
	void ensure_capacity(uint32_t bytes);
	void destroy_buffers();

	SDL_GPUDevice* device_{nullptr};
	SDL_Window* window_{nullptr};
	SDL_GPUTextureFormat swapchain_format_{SDL_GPU_TEXTUREFORMAT_INVALID};
	SDL_GPUGraphicsPipeline* pipeline_{nullptr};

	SDL_GPUBuffer* vertex_buffer_{nullptr};
	SDL_GPUTransferBuffer* transfer_buffer_{nullptr};
	uint32_t capacity_{0};

	SDL_GPUCommandBuffer* cmd_{nullptr};
	SDL_GPURenderPass* pass_{nullptr};
	SDL_GPUTexture* swapchain_{nullptr};
	uint64_t frames_rendered_{0};

	float clear_r_{0.0f}, clear_g_{0.0f}, clear_b_{0.0f};
};

} // namespace lotus

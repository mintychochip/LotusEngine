#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "lotus/gpu/vertex.hpp"
#include "lotus/scene/depth_pyramid.hpp"

struct SDL_Window;

namespace lotus {

/// A directional light plus an ambient term (Blinn-Phong).
struct LightingSettings {
	/// Unit vector pointing toward the light.
	glm::vec3 direction{0.4f, 0.8f, 0.6f};
	glm::vec3 color{1.0f, 1.0f, 1.0f};
	float intensity{1.0f};
	glm::vec3 ambient{0.15f, 0.15f, 0.18f};
};

/// SDL3 GPU backend (Vulkan on Linux). Owns the device, the lit pipeline,
/// a depth buffer, and a dynamic vertex buffer.
///
/// Frame stages (all on one command buffer):
///   begin_frame()  -> acquire command buffer + swapchain (+ depth resize)
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

	void init(SDL_Window* window, bool debug_device = true);
	void shutdown();

	/// Headless device that renders into an offscreen colour target instead of
	/// a window. Used for tests and frame capture (and, later, the editor
	/// viewport).
	void init_offscreen(uint32_t width, uint32_t height);

	/// Starts a frame against the offscreen target. Use after init_offscreen.
	void begin_frame_offscreen();

	/// Downloads the offscreen colour target as tightly packed RGBA8.
	/// Returns an empty vector if this renderer is not in offscreen mode.
	std::vector<uint8_t> capture_rgba(uint32_t& width, uint32_t& height);

	/// Offscreen capture written as a binary PPM (P6), which needs no encoder.
	bool write_ppm(const std::string& path, std::string* error = nullptr);

	SDL_GPUDevice* device() const { return device_; }
	const char* driver_name() const;

	// --- frame ---

	void begin_frame(SDL_Window* window, float clear_r, float clear_g,
	                 float clear_b);

	/// Uploads this frame's vertices (world-space, lit).
	void upload_vertices(const GpuVertex* vertices, uint32_t count);

	void begin_scene_pass();

	/// Draws \p count vertices using the lit pipeline.
	void draw(const glm::mat4& view_projection,
	          const glm::vec3& camera_position, uint32_t count);

	void end_scene_pass();
	void end_frame();

	// --- for UI layers that open their own pass on the same command buffer ---

	/// False when no swapchain texture was available (frame should be skipped).
	bool has_frame() const { return cmd_ != nullptr; }

	SDL_GPUCommandBuffer* command_buffer() const { return cmd_; }
	SDL_GPUTexture* swapchain_texture() const { return swapchain_; }
	SDL_GPUTextureFormat swapchain_format() const { return swapchain_format_; }

	/// CPU copy of the previous frame's depth pyramid. Never null; empty when
	/// the fence has not landed yet, in which case occlusion is skipped.
	const DepthPyramid& occlusion_pyramid() const { return occlusion_pyramid_; }

	uint32_t vertex_capacity() const { return capacity_; }

	/// Frames actually submitted (excludes skipped/stalled frames).
	uint64_t frames_rendered() const { return frames_rendered_; }

	LightingSettings& lighting() { return lighting_; }
	const LightingSettings& lighting() const { return lighting_; }

private:
	void create_pipeline();
	void ensure_vertex_capacity(uint32_t bytes);
	void ensure_depth_target(uint32_t width, uint32_t height);
	void destroy_buffers();

	SDL_GPUDevice* device_{nullptr};
	SDL_Window* window_{nullptr};
	bool offscreen_{false};
	SDL_GPUTexture* offscreen_color_{nullptr};
	uint32_t offscreen_width_{0};
	uint32_t offscreen_height_{0};
	SDL_GPUTextureFormat swapchain_format_{SDL_GPU_TEXTUREFORMAT_INVALID};
	SDL_GPUGraphicsPipeline* pipeline_{nullptr};

	SDL_GPUBuffer* vertex_buffer_{nullptr};
	SDL_GPUTransferBuffer* transfer_buffer_{nullptr};
	uint32_t capacity_{0};

	SDL_GPUTexture* depth_texture_{nullptr};
	uint32_t depth_width_{0};
	uint32_t depth_height_{0};

	SDL_GPUCommandBuffer* cmd_{nullptr};
	SDL_GPURenderPass* pass_{nullptr};
	SDL_GPUTexture* swapchain_{nullptr};
	uint64_t frames_rendered_{0};

	DepthPyramid occlusion_pyramid_;

	LightingSettings lighting_;
	float clear_r_{0.0f}, clear_g_{0.0f}, clear_b_{0.0f};
};

} // namespace lotus

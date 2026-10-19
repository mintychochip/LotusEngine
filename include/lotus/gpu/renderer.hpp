#pragma once

#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "lotus/gpu/draw_batch.hpp"
#include "lotus/gpu/vertex.hpp"
#include "lotus/scene/depth_pyramid.hpp"

struct SDL_Window;

namespace lotus {

class AssetRegistry;

/// CPU mirror of shaders/lit.frag. The fragment shader owns the ground truth;
/// these helpers exist so unit tests can assert the new-model invariants
/// (energy conservation, monotonic roughness response, tone-map bounds)
/// without a GPU. Keep the constants and formulas in sync with lit.frag.
namespace pbr {

inline constexpr float kPi = 3.14159265f;
/// Base reflectance of a dielectric at normal incidence (matches lit.frag).
inline constexpr float kDielectricF0 = 0.04f;
/// Floor on alpha = roughness^2; without it the GGX denominator is 0/0 at
/// roughness 0 (matches MIN_ALPHA in lit.frag).
inline constexpr float kMinAlpha = 0.001f;

inline float clamp01(float x) { return std::min(std::max(x, 0.0f), 1.0f); }

/// Trowbridge-Reitz GGX normal distribution.
inline float ndf_ggx(float n_dot_h, float roughness)
{
	const float a     = std::max(roughness * roughness, kMinAlpha);
	const float a2    = a * a;
	const float denom = n_dot_h * n_dot_h * (a2 - 1.0f) + 1.0f;
	return a2 / (kPi * denom * denom);
}

/// Schlick-GGX approximation of the Smith geometry term (direct lighting).
inline float geometry_schlick_ggx(float n_dot, float roughness)
{
	const float r = roughness + 1.0f;
	const float k = (r * r) / 8.0f;
	return n_dot / (n_dot * (1.0f - k) + k);
}

/// Schlick Fresnel for a dielectric (F0 = kDielectricF0).
inline float fresnel_schlick(float cos_theta)
{
	const float t = clamp01(1.0f - cos_theta);
	return kDielectricF0 +
	       (1.0f - kDielectricF0) * t * t * t * t * t;
}

/// Fraction of light left for the diffuse lobe after Fresnel reflection.
/// Metallic is 0 (dielectric-only pipeline), so this is simply 1 - F.
inline float diffuse_fraction(float h_dot_v)
{
	return 1.0f - fresnel_schlick(h_dot_v);
}

/// Cook-Torrance specular term: D * G * F / (4 (N.L) (N.V)).
inline float specular(float n_dot_l, float n_dot_v, float n_dot_h,
                      float h_dot_v, float roughness)
{
	const float d = ndf_ggx(n_dot_h, roughness);
	const float g = geometry_schlick_ggx(n_dot_l, roughness) *
	                geometry_schlick_ggx(n_dot_v, roughness);
	const float denom = std::max(4.0f * n_dot_l * n_dot_v, 0.001f);
	return d * g * fresnel_schlick(h_dot_v) / denom;
}

/// Narkowicz ACES filmic fit (scalar lane of the shader's vec3 version).
inline float aces(float x)
{
	const float mapped =
	        (x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f);
	return clamp01(mapped);
}

inline float srgb_to_linear(float c)
{
	return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

inline float linear_to_srgb(float c)
{
	return c <= 0.0031308f
	        ? c * 12.92f
	        : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

} // namespace pbr

/// A directional light plus an ambient floor, evaluated by the
/// Cook-Torrance dielectric BRDF in shaders/lit.frag with ACES tone mapping
/// and sRGB output. `exposure` scales linear HDR before the tone map.
struct LightingSettings {
	/// Unit vector pointing toward the light.
	glm::vec3 direction{0.4f, 0.8f, 0.6f};
	glm::vec3 color{1.0f, 1.0f, 1.0f};
	float intensity{1.0f};
	glm::vec3 ambient{0.15f, 0.15f, 0.18f};
	float exposure{1.0f};
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
	/// Resizes the offscreen target, and with it the depth buffer and the
	/// occlusion pyramid. Offscreen mode only. Returns false, leaving the
	/// renderer untouched, when not in offscreen mode or the new colour
	/// target could not be created. The pyramid is invalidated either way, so
	/// occlusion_ready() is false until a frame has been recorded at the new
	/// size.
	bool resize_offscreen(uint32_t width, uint32_t height);

	/// Offscreen capture written as a binary PPM (P6), which needs no encoder.
	bool write_ppm(const std::string& path, std::string* error = nullptr);

	SDL_GPUDevice* device() const { return device_; }
	const char* driver_name() const;

	// --- frame ---

	void begin_frame(SDL_Window* window, float clear_r, float clear_g,
	                 float clear_b);

	/// Uploads this frame's vertices (world-space, lit) and parallel UVs.
	void upload_vertices(const GpuVertex* vertices, const glm::vec2* uvs,
	                     uint32_t count);

	void begin_scene_pass();

	/// Draws \p count vertices using the lit pipeline. \p view is uploaded
	/// alongside \p view_projection because the vertex stage also writes
	/// view-space depth for the occlusion pyramid.
	void draw(const glm::mat4& view, const glm::mat4& view_projection,
	          const glm::vec3& camera_position, const GpuDrawBatch* batches,
	          uint32_t batch_count, const AssetRegistry* assets);
	/// Single-range draw (tests and untextured paths).
	void draw(const glm::mat4& view, const glm::mat4& view_projection,
	          const glm::vec3& camera_position, uint32_t first_vertex,
	          uint32_t vertex_count);

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

	/// False when compute shaders or R32 storage are unavailable. Culling then
	/// falls back to frustum and LOD only, with no errors.
	bool occlusion_enabled() const { return occlusion_enabled_; }

	/// True once a downloaded pyramid has landed. False means this frame skips
	/// occlusion because the previous frame's fence is still outstanding.
	bool occlusion_ready() const { return occlusion_ready_; }

	/// Aspect (width / height) of the depth image currently in
	/// occlusion_pyramid(). Zero when no image has been adopted.
	float occlusion_aspect() const { return occlusion_aspect_; }

	/// The next end_scene_pass closes the pass without downloading a pyramid.
	/// Used when the pass only clears the window behind the editor.
	void suppress_occlusion_record() { record_occlusion_ = false; }

	uint32_t vertex_capacity() const { return capacity_; }
	/// True when the viewport texture is ready to be sampled by ImGui.
	bool viewport_ready() const { return viewport_texture_ != nullptr; }

	/// The offscreen texture the scene renders into for the editor viewport.
	/// Valid only when viewport_ready() is true.
	SDL_GPUTexture* viewport_texture() const { return viewport_texture_; }

	/// Width of the viewport texture in pixels.
	uint32_t viewport_width() const { return viewport_width_; }

	/// Height of the viewport texture in pixels.
	uint32_t viewport_height() const { return viewport_height_; }

	/// Starts a render pass into the viewport texture. Call resize_viewport
	/// first if the size changed. Returns false when the texture is not ready.
	bool begin_viewport_pass();

	/// Ends the viewport render pass.
	void end_viewport_pass();

	/// Resizes the viewport texture and its depth buffer. Returns false on
	/// failure, leaving the old texture intact.
	bool resize_viewport(uint32_t width, uint32_t height);

	/// Frames actually submitted (excludes skipped/stalled frames).
	uint64_t frames_rendered() const { return frames_rendered_; }

	LightingSettings& lighting() { return lighting_; }
	const LightingSettings& lighting() const { return lighting_; }

private:
	void create_pipeline();
	void ensure_vertex_capacity(uint32_t vertex_bytes, uint32_t uv_bytes);
	void ensure_depth_target(uint32_t width, uint32_t height);
	void destroy_buffers();

	bool create_occlusion_pipeline();
	void destroy_occlusion();
	void destroy_pyramid();
	void ensure_pyramid(uint32_t width, uint32_t height);
	void ensure_pyramid_texture(uint32_t width, uint32_t height);
	void poll_pyramid_fence();
	void record_pyramid();
	/// Creates the single-mip R32F texture the scene pass writes as colour
	/// target 1 when the reduction is unavailable. lit.frag writes location 1
	/// unconditionally, so the attachment must exist even with occlusion off.
	void ensure_pyramid_stand_in(uint32_t width, uint32_t height);
	/// Releases fences whose submission has already been superseded (a resize
	/// dropped the pyramid they were reading into) but which the GPU may still
	/// be using. Releasing one of those would hand it straight back to SDL's
	/// pool, and the next SDL_AcquireGPUCommandBuffer would vkResetFences a
	/// fence that is still in use. Each is therefore released only once
	/// SDL_QueryGPUFence reports it signalled.
	void release_retired_fences();
	void ensure_default_sampler();
	void ensure_white_texture();
	void destroy_textures();
	SDL_GPUTexture* texture_for_albedo(const std::string& albedo_id,
	                                   const AssetRegistry* assets);
	bool load_texture_from_file(const std::string& path,
	                            const std::string& cache_key);

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
	SDL_GPUBuffer* uv_buffer_{nullptr};
	SDL_GPUTransferBuffer* uv_transfer_buffer_{nullptr};
	uint32_t capacity_{0};
	uint32_t uv_capacity_{0};

	SDL_GPUTexture* depth_texture_{nullptr};
	uint32_t depth_width_{0};
	uint32_t depth_height_{0};

	SDL_GPUCommandBuffer* cmd_{nullptr};
	SDL_GPURenderPass* pass_{nullptr};
	SDL_GPUTexture* swapchain_{nullptr};
	uint64_t frames_rendered_{0};

	SDL_GPUTexture* pyramid_texture_{nullptr};
	uint32_t pyramid_width_{0};
	uint32_t pyramid_height_{0};
	/// Byte offset of each mip inside pyramid_transfer_. Only mips from
	/// kDepthPyramidMinLevel are ever written or read.
	std::vector<uint32_t> pyramid_offset_;
	uint32_t pyramid_levels_{0};
	SDL_GPUTransferBuffer* pyramid_transfer_{nullptr};
	SDL_GPUFence* pyramid_fence_{nullptr};
	bool pyramid_in_flight_{false};
	/// Fences still owned by the GPU but no longer read from, e.g. when a
	/// resize destroyed the transfer buffer they were reading into. Released
	/// by release_retired_fences() once signalled, never before.
	std::vector<SDL_GPUFence*> retired_fences_;
	/// True once SDL_CreateGPUTexture has failed for the mip-chain pyramid
	/// texture. The scene pass still needs an R32F target 1 (lit.frag writes
	/// it unconditionally), so a single-mip stand-in takes over, but the
	/// compute reduction is never attempted again: the cause is a missing
	/// usage flag or an unsupported format, not the size, so retrying would
	/// log once per frame forever.
	bool pyramid_reduction_unavailable_{false};
	/// True when record_pyramid actually recorded a download this frame, so
	/// end_frame knows the submit needs a fence.
	bool pyramid_recorded_{false};
	// --- viewport (offscreen render target for the editor) ---
	SDL_GPUTexture* viewport_texture_{nullptr};
	SDL_GPUTexture* viewport_depth_{nullptr};
	uint32_t viewport_width_{0};
	uint32_t viewport_height_{0};
	// R32F stand-in for colour target 1, sized to the panel (not the swapchain).
	// The viewport pass binds it so lit.frag's linear-depth output has a home.
	// The Hi-Z pyramid is NOT recorded from this pass; the main pass is authoritative.
	SDL_GPUTexture* viewport_pyramid_{nullptr};
	SDL_GPUComputePipeline* hiz_pipeline_{nullptr};
	bool occlusion_enabled_{false};
	bool occlusion_ready_{false};
	bool record_occlusion_{true};
	float occlusion_aspect_{0.0f};

	DepthPyramid occlusion_pyramid_;

	LightingSettings lighting_;
	float clear_r_{0.0f}, clear_g_{0.0f}, clear_b_{0.0f};

	SDL_GPUSampler* default_sampler_{nullptr};
	SDL_GPUTexture* white_texture_{nullptr};
	struct CachedAlbedo {
		SDL_GPUTexture* texture{nullptr};
	};
	std::unordered_map<std::string, CachedAlbedo> albedo_textures_;
};

} // namespace lotus

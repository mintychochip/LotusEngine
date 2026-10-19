#pragma once

// Shared state of the Vulkan backend: one `GpuRenderer::Impl` per renderer.
//
// The public header (include/lotus/gpu/renderer.hpp) is deliberately
// backend-agnostic — it holds a `std::unique_ptr<Impl>` and nothing else — so a
// second backend is a second Impl and a second set of translation units behind
// the same API. Everything here is Vulkan only.
//
// Translation units, by concern (each implements the methods its section names):
//   renderer_core.cpp      device, frame ring, targets, frame lifecycle, capture
//   renderer_geometry.cpp  mesh cache, per-frame streams, uploads, draw recording
//   renderer_passes.cpp    shadow / lit / sky / FXAA / SSAO pipelines and passes
//   renderer_occlusion.cpp Hi-Z reduction, readback, adopt, degradation
//   renderer_textures.cpp  samplers, white texture, albedo cache
//   renderer_swapchain.cpp swapchain, acquire/present (windowed)
//   renderer_ui.cpp        ImGui backend hooks

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "lotus/gpu/renderer.hpp"
#include "gpu/vk/vk_barrier.hpp"
#include "gpu/vk/vk_context.hpp"
#include "gpu/vk/vk_memory.hpp"
#include "gpu/vk/vk_pipelines.hpp"

struct SDL_Window;

namespace lotus {

/// Must match shaders/lit.vert (set 0, binding 0).
struct VertexUniforms {
	glm::mat4 view_projection;
	glm::mat4 view;
};
static_assert(sizeof(VertexUniforms) == 128, "must match lit.vert's std140 block");

/// Must match shaders/lit.frag (set 0, binding 1). Only GAINS trailing members
/// over the original five vec4s, so old uniform data stays prefix-compatible.
struct FragmentUniforms {
	glm::vec4 light_direction;
	glm::vec4 light_color;    // rgb + intensity in .a
	glm::vec4 ambient_color;
	glm::vec4 camera_position;
	glm::vec4 tone_params;    // x: exposure; yzw reserved
	glm::vec4 hemi_sky;
	glm::vec4 hemi_ground;
	glm::vec4 shadow_params;  // x: enabled; y: bias; z: normal offset; w: texel
	glm::mat4 light_vp;
	glm::vec4 fog_params;
	glm::vec4 fog_color;
	glm::vec4 bounce_color;
	glm::vec4 bounce_params;
	glm::vec4 blob_spheres[4];
	glm::vec4 blob_params;
	glm::vec4 ssao_params;
	glm::vec4 ssao_texel;
};
static_assert(sizeof(FragmentUniforms) == 368,
              "must match lit.frag's std140 block");

/// Must match shaders/ssao.frag (set 0, binding 1).
struct SsaoUniforms {
	glm::vec4 texel;
	glm::vec4 params;
};
static_assert(sizeof(SsaoUniforms) == 32, "must match ssao.frag's std140 block");

/// Must match shaders/fxaa.frag (set 0, binding 1).
struct FxaaUniforms {
	glm::vec4 texel;
};
static_assert(sizeof(FxaaUniforms) == 16, "must match fxaa.frag's std140 block");

/// Must match shaders/sky.vert (set 0, binding 0).
struct SkyVertexUniforms {
	glm::mat4 inv_view_projection;
};
static_assert(sizeof(SkyVertexUniforms) == 64, "must match sky.vert's std140 block");

/// Must match shaders/sky.frag (set 0, binding 1).
struct SkyFragmentUniforms {
	glm::vec4 camera_position;
	glm::vec4 sky_color;
	glm::vec4 ground_color;
	glm::vec4 sun_direction;
	glm::vec4 sun_color;
	glm::vec4 params;
};
static_assert(sizeof(SkyFragmentUniforms) == 96,
              "must match sky.frag's std140 block");

/// The three descriptor set layouts every pipeline draws from:
///   pass (set 0): b0 vertex UBO dynamic, b1 fragment UBO dynamic,
///                 b2 sampler (shadow / FXAA source / SSAO depth), b3 sampler (SSAO)
///   material (set 1): b0 albedo sampler2D
///   compute (set 0): b0 src storage image, b1 dst storage image
/// Uniform blocks are dynamic-offset descriptors into the slot's ring buffer;
/// there are no push constants. See the design spec for why the first version
/// of the shaders kept SDL_GPU's set numbers and why that changed.
struct DescriptorLayouts {
	VkDescriptorSetLayout pass{VK_NULL_HANDLE};
	VkDescriptorSetLayout material{VK_NULL_HANDLE};
	VkDescriptorSetLayout compute{VK_NULL_HANDLE};

	void create(const vk::Context& context);
	void destroy(const vk::Context& context);
};

/// One frame's Vulkan objects. Everything the CPU writes during a frame lives
/// here, so `kFramesInFlight` frames can be in flight without the CPU ever
/// touching memory the GPU is still reading.
struct FrameSlot {
	VkCommandPool pool{VK_NULL_HANDLE};
	VkCommandBuffer cmd{VK_NULL_HANDLE};
	VkFence fence{VK_NULL_HANDLE};
	/// Transient pass/material descriptor sets; reset when the slot is reused.
	VkDescriptorPool descriptors{VK_NULL_HANDLE};

	/// Instance records for this frame; entry 0 is the reserved identity the
	/// immediate stream draws with.
	vk::Buffer instances;
	uint32_t instance_capacity{0};
	/// World-space vertices and UVs of meshes that cannot be instanced.
	vk::Buffer immediate;
	vk::Buffer immediate_uv;
	uint32_t immediate_capacity{0};
	/// UBO ring: every uniform block the frame pushes, bump-allocated.
	vk::Buffer uniforms;
	uint64_t uniform_offset{0};
	uint64_t uniform_capacity{0};
	/// Hi-Z mip K readback (sized with the pyramid).
	vk::Buffer readback;
	uint32_t readback_pitch{0};
	bool readback_recorded{false};

	/// Resources released during this frame, destroyed when the slot is reused
	/// (its fence has been waited on, so nothing can still be reading them).
	std::vector<std::function<void()>> garbage;
};

} // namespace lotus

namespace lotus {

struct GpuRenderer::Impl {
	// --- device, frame ring -------------------------------------------------
	vk::Context context;
	vk::Memory memory;
	SDL_Window* window{nullptr};
	bool offscreen{false};
	VkFormat swapchain_format{VK_FORMAT_UNDEFINED};
	uint32_t swapchain_width{0};
	uint32_t swapchain_height{0};
	/// Windowed only (stage 2).
	VkSurfaceKHR surface{VK_NULL_HANDLE};
	VkSwapchainKHR swapchain{VK_NULL_HANDLE};
	std::vector<vk::Image> swapchain_images;
	std::vector<vk::ImageState> swapchain_states;
	uint32_t swapchain_image{0};

	std::array<FrameSlot, GpuRenderer::kFramesInFlight> slots;
	uint32_t slot_index{0};
	FrameSlot* slot{nullptr};
	VkCommandBuffer cmd{VK_NULL_HANDLE};
	uint64_t frames_rendered{0};

	vk::BarrierBatch* barriers{nullptr}; ///< per-frame scratch, owned by the slot

	// --- pipelines ----------------------------------------------------------
	DescriptorLayouts layouts;
	VkPipelineLayout lit_layout{VK_NULL_HANDLE};
	VkPipelineLayout shadow_layout{VK_NULL_HANDLE};
	VkPipelineLayout sky_layout{VK_NULL_HANDLE};
	VkPipelineLayout fxaa_layout{VK_NULL_HANDLE};
	VkPipelineLayout ssao_layout{VK_NULL_HANDLE};
	VkPipelineLayout hiz_layout{VK_NULL_HANDLE};
	VkPipeline lit_pipeline{VK_NULL_HANDLE};
	VkPipeline shadow_pipeline{VK_NULL_HANDLE};
	VkPipeline sky_pipeline{VK_NULL_HANDLE};
	VkPipeline fxaa_pipeline{VK_NULL_HANDLE};
	VkPipeline ssao_pipeline{VK_NULL_HANDLE};
	VkPipeline hiz_pipeline{VK_NULL_HANDLE};

	// --- samplers and 1x1 white fallback ------------------------------------
	VkSampler default_sampler{VK_NULL_HANDLE};
	VkSampler shadow_sampler{VK_NULL_HANDLE};
	VkSampler fxaa_sampler{VK_NULL_HANDLE};
	VkSampler ssao_sampler{VK_NULL_HANDLE};
	VkSampler ssao_depth_sampler{VK_NULL_HANDLE};
	vk::Image white_texture;
	vk::ImageState white_state;

	// --- targets ------------------------------------------------------------
	vk::Image offscreen_color;
	vk::ImageState offscreen_color_state;
	uint32_t offscreen_width{0};
	uint32_t offscreen_height{0};
	vk::Image depth;
	vk::ImageState depth_state;
	uint32_t depth_width{0};
	uint32_t depth_height{0};
	/// FXAA intermediate, at the scene/viewport target's size and format.
	vk::Image fxaa_source;
	vk::ImageState fxaa_source_state;
	uint32_t fxaa_width{0};
	uint32_t fxaa_height{0};
	bool fxaa_unavailable{false};
	/// SSAO, quarter res, sampled by the next frame's lit pass.
	vk::Image ssao_texture;
	vk::ImageState ssao_state;
	uint32_t ssao_width{0};
	uint32_t ssao_height{0};
	bool ssao_unavailable{false};
	/// 1024^2 D16 depth rendered from the light.
	vk::Image shadow_map;
	vk::ImageState shadow_map_state;
	bool shadows_supported{false};
	bool shadow_support_queried{false};
	bool shadow_rendered{false};
	glm::mat4 light_vp{1.0f};

	// --- viewport (editor) --------------------------------------------------
	vk::Image viewport_color;
	vk::ImageState viewport_color_state;
	vk::Image viewport_depth;
	vk::ImageState viewport_depth_state;
	/// Single-mip stand-in the viewport pass writes as target 1 (the Hi-Z
	/// pyramid is never recorded from that pass).
	vk::Image viewport_pyramid;
	vk::ImageState viewport_pyramid_state;
	uint32_t viewport_width{0};
	uint32_t viewport_height{0};

	// --- Hi-Z occlusion -----------------------------------------------------
	/// Mip 0 is the scene pass's colour target 1; mip K is what the CPU reads.
	vk::Image pyramid;
	std::array<vk::ImageState, 4> pyramid_states{};
	uint32_t pyramid_width{0};
	uint32_t pyramid_height{0};
	uint32_t pyramid_levels{0};
	bool occlusion_ready{false};
	bool record_occlusion{true};
	float occlusion_aspect{0.0f};
	bool pyramid_reduction_unavailable{false};
	DepthPyramid occlusion_pyramid; ///< CPU copy, adopted from the readback

	// --- per-pass state -----------------------------------------------------
	bool sky_drawn{false};
	bool pass_redirected{false};
	uint32_t pass_width{0};
	uint32_t pass_height{0};
	float clear_r{0.0f};
	float clear_g{0.0f};
	float clear_b{0.0f};
	bool has_frame{false};

	LightingSettings lighting;
	glm::vec4 blob_spheres[4]{};
	uint32_t blob_count{0};

	// --- geometry -----------------------------------------------------------
	struct CachedMesh {
		vk::Buffer vertex;
		vk::Buffer uv;
		vk::Buffer index;
		uint32_t vertex_count{0};
		uint32_t source_index_count{0};
		uint32_t index_count{0};
		uint64_t last_used{0};
		std::string key;
		bool live{false};
	};
	struct PendingMeshUpload {
		GpuMeshId id{kNoGpuMesh};
		std::vector<GpuVertex> vertices;
		std::vector<glm::vec2> uvs;
		std::vector<uint32_t> indices;
	};
	struct GeometryDraw {
		GpuMeshId mesh{kNoGpuMesh};
		uint32_t first_vertex{0};
		uint32_t vertex_count{0};
		uint32_t first_instance{0};
		uint32_t instance_count{0};
	};
	std::vector<CachedMesh> meshes;
	std::vector<GpuMeshId> free_mesh_ids;
	std::unordered_map<std::string, GpuMeshId> mesh_ids;
	std::vector<PendingMeshUpload> pending_meshes;
	std::vector<GeometryDraw> geometry_draws;
	uint32_t staged_vertices{0};
	uint32_t staged_instances{0};
	bool geometry_mapped{false};

	// --- albedo textures ----------------------------------------------------
	struct CachedAlbedo {
		vk::Image image;
		vk::ImageState state;
	};
	std::unordered_map<std::string, CachedAlbedo> albedo_textures;

	// --- per-concern helpers (defined by the file that owns the concern) -----
	// renderer_core.cpp
	void destroy_targets();
	void ensure_depth_target(uint32_t width, uint32_t height);
	void begin_frame_common();
	void end_frame_submit();
	FrameSlot& acquire_slot();
	void destroy_garbage(FrameSlot& slot);
	/// Writes \p data into the slot's UBO ring, aligned, and returns its offset.
	uint64_t push_uniforms(const void* data, uint64_t size);
	void set_viewport(VkCommandBuffer cmd, uint32_t width, uint32_t height,
	                  float max_depth);
	void set_scissor(VkCommandBuffer cmd, uint32_t width, uint32_t height);

	// renderer_geometry.cpp
	void grow_streams(FrameSlot& slot, uint32_t vertices, uint32_t instances);
	void destroy_meshes();
	void evict_idle_meshes();
	void release_mesh(GpuMeshId mesh);
	bool mesh_live(GpuMeshId mesh) const;
	void upload_pending_meshes(VkCommandBuffer cmd);
	void record_geometry_draw(VkCommandBuffer cmd, const GeometryDraw& draw);

	// renderer_passes.cpp
	void create_pipelines();
	void create_pipelines_deferred(); ///< fxaa/ssao/sky: created lazily, formats known
	void bind_lit_state(const glm::mat4& view, const glm::mat4& view_projection,
	                    const glm::vec3& camera_position);
	void render_shadow_map();
	void shadow_light_matrices(glm::mat4& view, glm::mat4& vp) const;
	bool ensure_fxaa_source(uint32_t width, uint32_t height);
	bool ensure_ssao_target(uint32_t width, uint32_t height);
	void run_fxaa_resolve(const vk::Image& destination);
	void run_ssao_pass(uint32_t width, uint32_t height);
	void draw_sky(const glm::mat4& view_projection,
	              const glm::vec3& camera_position);

	// renderer_occlusion.cpp
	void ensure_pyramid(uint32_t width, uint32_t height);
	void destroy_pyramid();
	void record_pyramid();
	void poll_pyramid();
	void ensure_pyramid_stand_in(uint32_t width, uint32_t height);

	// renderer_textures.cpp
	void create_samplers();
	vk::Image create_target_image(const char* name, VkFormat format, uint32_t width,
	                              uint32_t height, uint32_t mips,
	                              VkImageUsageFlags usage);
	bool load_texture_from_file(const std::string& path, const std::string& key);
	VkImageView texture_for_albedo(const std::string& albedo_id,
	                               const AssetRegistry* assets);
	void create_white_texture();
};

} // namespace lotus
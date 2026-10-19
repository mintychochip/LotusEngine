#pragma once

// Vulkan backend for `GpuRenderer`: offscreen core, geometry, textures and the
// shadow/lit/sky passes (first half; FXAA, SSAO, Hi-Z, the swapchain and ImGui
// belong to the second half).
//
// Behavioural reference: src/gpu/sdlgpu/renderer_sdlgpu.cpp. Mesh-cache
// semantics, pass order, uniform contents, clear values and degradation
// messages match it; the comments there explain why.
//
// Translation units, by concern:
//   renderer_core.cpp      device, frame ring, targets, frame lifecycle, capture
//   renderer_geometry.cpp  mesh cache, per-frame streams, uploads, draw recording
//   renderer_passes.cpp    descriptor layouts, pipelines, shadow/lit/sky passes
//   renderer_textures.cpp  samplers, white texture, albedo cache, image helper
//   factory.cpp            create_gpu_renderer()
//
// Descriptor sets keep the committed SPIR-V's numbers (sets 1/2/3 from SDL's
// Vulkan backend), so the shader headers stay byte-identical: set 1 is the
// vertex UBO (dynamic), set 2 the fragment samplers (albedo, shadow, SSAO),
// set 3 the fragment UBO (dynamic). The pipeline layouts carry an empty set 0
// so the indices line up. No push constants.

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

/// Must match shaders/lit.vert (set 1, binding 0).
struct VertexUniforms {
	glm::mat4 view_projection;
	glm::mat4 view;
};
static_assert(sizeof(VertexUniforms) == 128, "must match lit.vert's std140 block");

/// Must match shaders/lit.frag (set 3, binding 0). Only GAINS trailing members
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

/// Must match shaders/sky.vert (set 1, binding 0).
struct SkyVertexUniforms {
	glm::mat4 inv_view_projection;
};
static_assert(sizeof(SkyVertexUniforms) == 64, "must match sky.vert's std140 block");

/// Must match shaders/sky.frag (set 3, binding 0).
struct SkyFragmentUniforms {
	glm::vec4 camera_position;
	glm::vec4 sky_color;
	glm::vec4 ground_color;
	glm::vec4 sun_direction;
	glm::vec4 sun_color; // rgb + intensity in .a
	glm::vec4 params;    // x: exposure; y: horizon_warmth; zw reserved
};
static_assert(sizeof(SkyFragmentUniforms) == 96,
              "must match sky.frag's std140 block");

/// Descriptor set layouts matching the committed SPIR-V (see above). Every
/// graphics pipeline layout is [empty, vertex, samplers, fragment] so that
/// set indices line up with the shaders; the shadow pipeline uses only
/// [empty, vertex].
struct DescriptorLayouts {
	VkDescriptorSetLayout empty{VK_NULL_HANDLE};
	VkDescriptorSetLayout vertex{VK_NULL_HANDLE};
	VkDescriptorSetLayout samplers{VK_NULL_HANDLE};
	VkDescriptorSetLayout fragment{VK_NULL_HANDLE};
	VkDescriptorSetLayout storage{VK_NULL_HANDLE};

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
	vk::BufferState instance_state;
	/// World-space vertices and UVs of meshes that cannot be instanced.
	vk::Buffer immediate;
	vk::Buffer immediate_uv;
	uint32_t immediate_capacity{0};
	uint32_t immediate_uv_capacity{0};
	vk::BufferState immediate_state;
	vk::BufferState immediate_uv_state;
	/// UBO ring: every uniform block the frame pushes, bump-allocated.
	vk::Buffer uniforms;
	uint64_t uniform_offset{0};
	uint64_t uniform_capacity{0};
	vk::BufferState uniform_state;

	/// Resources released during this frame, destroyed when the slot is reused
	/// (its fence has been waited on, so nothing can still be reading them).
	std::vector<std::function<void()>> garbage;
	/// Submit serial of the last frame recorded into this slot (0 = none).
	uint64_t serial{0};
};

/// Raw Vulkan implementation of `GpuRenderer`: offscreen rendering with
/// shadow-mapped lit geometry and the procedural sky. Until the second half
/// lands, FXAA/SSAO/Hi-Z are unavailable, the viewport/swapchain/UI paths are
/// inert, and occlusion reports disabled.
class GpuRendererVulkan final : public GpuRenderer {
public:
	GpuRendererVulkan() = default;
	~GpuRendererVulkan() override;

	GpuRendererVulkan(const GpuRendererVulkan&)            = delete;
	GpuRendererVulkan& operator=(const GpuRendererVulkan&) = delete;

	void init(SDL_Window* window, bool debug_device = true) override;
	void shutdown() override;
	void init_offscreen(uint32_t width, uint32_t height) override;
	void begin_frame_offscreen() override;
	std::vector<uint8_t> capture_rgba(uint32_t& width, uint32_t& height) override;
	std::vector<uint8_t> capture_viewport_rgba(uint32_t& width,
	                                           uint32_t& height) override;
	bool resize_offscreen(uint32_t width, uint32_t height) override;
	bool write_ppm(const std::string& path, std::string* error = nullptr) override;
	void wait_idle() override;
	const char* device_name() const override;
	const char* device_driver() const override;
	const char* driver_name() const override;
	void begin_frame(SDL_Window* window, float clear_r, float clear_g,
	                 float clear_b) override;
	GpuMeshId acquire_mesh(const std::string& key, const GpuMeshData& data) override;
	uint32_t mesh_index_count(GpuMeshId mesh) const override;
	uint32_t cached_mesh_count() const override;
	GeometryStaging begin_geometry_upload(uint32_t immediate_vertices,
	                                      uint32_t instance_count) override;
	void commit_geometry_upload(const GpuDrawBatch* batches,
	                            uint32_t batch_count) override;
	void begin_scene_pass() override;
	void draw(const glm::mat4& view, const glm::mat4& view_projection,
	          const glm::vec3& camera_position, const GpuDrawBatch* batches,
	          uint32_t batch_count, const AssetRegistry* assets) override;
	void end_scene_pass() override;
	void end_frame() override;
	bool has_frame() const override { return cmd_ != nullptr; }
	bool init_ui_backend() override { return false; }
	void shutdown_ui_backend() override {}
	void ui_backend_new_frame() override {}
	void render_ui(ImDrawData* draw_data) override
	{
		(void)draw_data;
	}
	const DepthPyramid& occlusion_pyramid() const override
	{
		return occlusion_pyramid_;
	}
	bool occlusion_enabled() const override { return occlusion_enabled_; }
	bool occlusion_ready() const override { return occlusion_ready_; }
	float occlusion_aspect() const override { return occlusion_aspect_; }
	void suppress_occlusion_record() override { record_occlusion_ = false; }
	uint32_t vertex_capacity() const override { return vertex_capacity_; }
	bool viewport_ready() const override { return false; }
	uint64_t viewport_ui_texture() const override { return 0; }
	uint32_t viewport_width() const override { return 0; }
	uint32_t viewport_height() const override { return 0; }
	bool begin_viewport_pass() override { return false; }
	void end_viewport_pass() override {}
	bool resize_viewport(uint32_t width, uint32_t height) override
	{
		(void)width;
		(void)height;
		return false;
	}
	uint64_t frames_rendered() const override { return frames_rendered_; }
	LightingSettings& lighting() override { return lighting_; }
	const LightingSettings& lighting() const override { return lighting_; }
	void set_blob_casters(const glm::vec4* spheres, uint32_t count) override;
	void clear_blob_casters() override { blob_count_ = 0; }
	uint32_t blob_count() const override { return blob_count_; }
	const glm::vec4* blob_spheres() const override { return blob_spheres_; }

	/// Validation-layer messages reported so far (tests assert zero).
	uint32_t validation_messages() const
	{
		return context_.valid() ? context_.validation_messages() : 0u;
	}

private:
	// --- device, frame ring -------------------------------------------------
	vk::Context context_;
	vk::Memory memory_;
	SDL_Window* window_{nullptr};
	bool offscreen_{false};
	bool device_valid_{false};

	std::array<FrameSlot, GpuRenderer::kFramesInFlight> slots_;
	uint32_t slot_index_{0};
	FrameSlot* slot_{nullptr};
	VkCommandBuffer cmd_{VK_NULL_HANDLE};
	uint64_t frames_rendered_{0};

	std::unique_ptr<vk::BarrierBatch> barriers_;

	// --- pipelines ----------------------------------------------------------
	DescriptorLayouts layouts_;
	VkPipelineLayout lit_layout_{VK_NULL_HANDLE};
	VkPipelineLayout shadow_layout_{VK_NULL_HANDLE};
	VkPipelineLayout sky_layout_{VK_NULL_HANDLE};
	VkPipeline lit_pipeline_{VK_NULL_HANDLE};
	VkPipeline shadow_pipeline_{VK_NULL_HANDLE};
	VkPipeline sky_pipeline_{VK_NULL_HANDLE};
	bool sky_unavailable_{false};
	VkFormat sky_format_{VK_FORMAT_UNDEFINED};

	// --- samplers and 1x1 white fallback ------------------------------------
	VkSampler default_sampler_{VK_NULL_HANDLE};
	VkSampler shadow_sampler_{VK_NULL_HANDLE};
	vk::Image white_texture_;
	vk::ImageState white_state_;

	// --- targets ------------------------------------------------------------
	vk::Image offscreen_color_;
	vk::ImageState offscreen_color_state_;
	uint32_t offscreen_width_{0};
	uint32_t offscreen_height_{0};
	vk::Image depth_;
	vk::ImageState depth_state_;
	uint32_t depth_width_{0};
	uint32_t depth_height_{0};
	/// Mip 0 is the scene pass's colour target 1; mip K is what the CPU reads.
	/// Only mips 0..K exist: the query reads mip K and nothing coarser.
	vk::Image pyramid_;
	vk::ImageState pyramid_states_[3]{};
	VkImageView pyramid_views_[3]{VK_NULL_HANDLE, VK_NULL_HANDLE,
	                              VK_NULL_HANDLE};
	uint32_t pyramid_width_{0};
	uint32_t pyramid_height_{0};
	uint32_t pyramid_levels_{0};
	bool occlusion_enabled_{false};
	bool occlusion_ready_{false};
	bool record_occlusion_{true};
	float occlusion_aspect_{0.0f};
	bool pyramid_reduction_unavailable_{false};
	/// Mip K download, at most one in flight (a second record into the same
	/// buffer would overwrite the bytes the poll is going to map).
	vk::Buffer pyramid_readback_;
	uint32_t pyramid_pitch_{0};
	bool pyramid_recorded_{false};
	bool pyramid_in_flight_{false};
	uint64_t pyramid_serial_{0};
	uint64_t submit_serial_{0};
	uint64_t completed_serial_{0};
	VkPipelineLayout hiz_layout_{VK_NULL_HANDLE};
	VkPipeline hiz_pipeline_{VK_NULL_HANDLE};
	bool fxaa_unavailable_{true};
	bool ssao_unavailable_{true};
	/// 1024^2 D16 depth rendered from the light.
	vk::Image shadow_map_;
	vk::ImageState shadow_map_state_;
	bool shadows_supported_{false};
	bool shadow_support_queried_{false};
	bool shadow_rendered_{false};
	glm::mat4 light_vp_{1.0f};

	// --- per-pass state -----------------------------------------------------
	bool sky_drawn_{false};
	uint32_t pass_width_{0};
	uint32_t pass_height_{0};
	float clear_r_{0.0f};
	float clear_g_{0.0f};
	float clear_b_{0.0f};
	bool scene_pass_open_{false};

	LightingSettings lighting_;
	glm::vec4 blob_spheres_[4]{};
	uint32_t blob_count_{0};

	DepthPyramid occlusion_pyramid_;

	// --- geometry -----------------------------------------------------------
	struct CachedMesh {
		vk::Buffer vertex;
		vk::Buffer uv;
		vk::Buffer index;
		vk::BufferState vertex_state;
		vk::BufferState uv_state;
		vk::BufferState index_state;
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
	std::vector<CachedMesh> meshes_;
	std::vector<GpuMeshId> free_mesh_ids_;
	std::unordered_map<std::string, GpuMeshId> mesh_ids_;
	std::vector<PendingMeshUpload> pending_meshes_;
	std::vector<GeometryDraw> geometry_draws_;
	uint32_t staged_vertices_{0};
	uint32_t staged_instances_{0};
	bool geometry_mapped_{false};
	uint32_t vertex_capacity_{0};

	// --- albedo textures ----------------------------------------------------
	struct CachedAlbedo {
		vk::Image image;
		vk::ImageState state;
	};
	std::unordered_map<std::string, CachedAlbedo> albedo_textures_;

	std::string device_name_;
	std::string device_driver_;
	uint64_t uniform_alignment_{256};

	// renderer_core.cpp
	void destroy_targets();
	void ensure_depth_target(uint32_t width, uint32_t height);
	void ensure_pyramid(uint32_t width, uint32_t height);
	void ensure_pyramid_stand_in(uint32_t width, uint32_t height);
	void destroy_pyramid();
	void begin_frame_common();
	void end_frame_submit();
	FrameSlot& acquire_slot();
	void destroy_garbage(FrameSlot& slot);
	void destroy_slot(FrameSlot& slot);
	void create_slot(FrameSlot& slot);
	/// Writes \p data into the slot's UBO ring, aligned, and returns its offset.
	uint64_t push_uniforms(const void* data, uint64_t size);
	void set_viewport(VkCommandBuffer cmd, uint32_t width, uint32_t height,
	                  float max_depth);
	void set_scissor(VkCommandBuffer cmd, uint32_t width, uint32_t height);
	void begin_rendering(VkCommandBuffer cmd);
	void destroy_pipelines();

	// renderer_geometry.cpp
	void grow_streams(FrameSlot& slot, uint32_t vertices, uint32_t instances);
	void grow_uniforms(FrameSlot& slot, uint64_t size);
	void destroy_meshes();
	void evict_idle_meshes();
	void release_mesh(GpuMeshId mesh);
	bool mesh_live(GpuMeshId mesh) const;
	void upload_pending_meshes(VkCommandBuffer cmd);
	void record_geometry_draw(VkCommandBuffer cmd, const GeometryDraw& draw);
	void abandon_geometry_staging();
	void begin_geometry_frame();

	// renderer_passes.cpp
	void create_pipelines();
	void bind_lit_state(const glm::mat4& view, const glm::mat4& view_projection,
	                    const glm::vec3& camera_position);
	void render_shadow_map();
	void shadow_light_matrices(glm::mat4& view, glm::mat4& vp) const;
	bool shadows_ready();
	void ensure_shadow_resources();
	void destroy_shadows();
	bool ensure_sky_pipeline();
	void draw_sky(const glm::mat4& view_projection,
	              const glm::vec3& camera_position);
	VkDescriptorSet alloc_vertex_set(uint64_t offset);
	VkDescriptorSet alloc_fragment_set(uint64_t offset);
	VkDescriptorSet alloc_sampler_set(VkImageView albedo, VkSampler albedo_sampler,
	                                  VkImageView shadow, VkSampler shadow_sampler,
	                                  VkImageView ao, VkSampler ao_sampler);
	// renderer_occlusion.cpp
	bool create_occlusion_pipeline();
	void destroy_occlusion();
	void record_pyramid();
	void poll_pyramid();
	VkDescriptorSet alloc_storage_set(VkImageView src, VkImageView dst);
	void order_storage_write(VkCommandBuffer cmd, uint32_t mip);

	// renderer_textures.cpp
	void create_samplers();
	void destroy_samplers();
	vk::Image create_target_image(const char* name, VkFormat format, uint32_t width,
	                              uint32_t height, uint32_t mips,
	                              VkImageUsageFlags usage);
	bool load_texture_from_file(const std::string& path, const std::string& key);
	VkImageView texture_for_albedo(const std::string& albedo_id,
	                               const AssetRegistry* assets);
	void create_white_texture();
	void destroy_textures();
};

} // namespace lotus

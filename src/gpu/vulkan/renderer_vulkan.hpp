#pragma once

// Vulkan backend for `GpuRenderer`: offscreen core, geometry, textures, the
// shadow/lit/sky passes, FXAA, SSAO, Hi-Z occlusion, the editor viewport
// pass, the windowed swapchain (renderer_window.cpp) and the ImGui backend.
//
// Behavioural reference: src/gpu/sdlgpu/renderer_sdlgpu.cpp. Mesh-cache
// semantics, pass order, uniform contents, clear values and degradation
// messages match it; the comments there explain why.
//
// Translation units, by concern:
//   renderer_core.cpp      device, frame ring, targets, frame lifecycle, capture
//   renderer_geometry.cpp  mesh cache, per-frame streams, uploads, draw recording
//   renderer_passes.cpp    descriptor layouts, pipelines, shadow/lit/sky passes
//   renderer_post.cpp      FXAA, SSAO, viewport pass, capture, retirement
//   renderer_textures.cpp  samplers, white texture, albedo cache, image helper
//   renderer_occlusion.cpp Hi-Z reduction, readback, CPU pyramid adoption
//   renderer_window.cpp    swapchain, present, ImGui backend
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
#include <map>
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

/// Must match shaders/ssao.frag (set 3, binding 0).
struct SsaoUniforms {
	glm::vec4 texel;  // xy: 1/width, 1/height; zw reserved
	glm::vec4 params; // x: radius (texels); y: bias; z: strength; w reserved
};
static_assert(sizeof(SsaoUniforms) == 32,
              "must match ssao.frag's std140 block");

/// Must match shaders/fxaa.frag (set 3, binding 0).
struct FxaaUniforms {
	glm::vec4 texel; // x: 1/width; y: 1/height; zw reserved
};
static_assert(sizeof(FxaaUniforms) == 16,
              "must match fxaa.frag's std140 block");

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

/// Stage at which a windowed frame's submit waits for the swapchain acquire.
/// The image's first transition of the frame names the same stage as its
/// source: that chain is what orders the transition after the acquire (which
/// reads the image), so the two must not drift apart.
inline constexpr VkPipelineStageFlags2 kAcquireWaitStage =
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

/// One frame's Vulkan objects. Everything the CPU writes during a frame lives
/// here, so `kFramesInFlight` frames can be in flight without the CPU ever
/// touching memory the GPU is still reading.
struct FrameSlot {
	VkCommandPool pool{VK_NULL_HANDLE};
	VkCommandBuffer cmd{VK_NULL_HANDLE};
	VkFence fence{VK_NULL_HANDLE};
	/// Transient pass/material descriptor sets; reset when the slot is reused.
	/// Only the fallback path below still allocates from here (see the
	/// descriptor cache); the steady state allocates nothing per frame.
	VkDescriptorPool descriptors{VK_NULL_HANDLE};
	/// Cached uniform-buffer sets. Their update writes buffer + range only
	/// (the offset is dynamic at bind), so one set serves every uniform
	/// block and is rewritten only when the slot's uniform ring regrows.
	/// They live in the never-reset cache pool, so they survive pool resets
	/// and slot reuse; the buffer handle tracks the generation they name.
	VkDescriptorSet cached_vertex_set{VK_NULL_HANDLE};
	VkBuffer cached_vertex_buffer{VK_NULL_HANDLE};
	VkDescriptorSet cached_fragment_set{VK_NULL_HANDLE};
	VkBuffer cached_fragment_buffer{VK_NULL_HANDLE};

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
/// shadow-mapped lit geometry, the procedural sky, FXAA, SSAO, Hi-Z occlusion,
/// the editor viewport pass, the windowed swapchain and the ImGui backend.
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
	bool init_ui_backend() override;
	void shutdown_ui_backend() override;
	void ui_backend_new_frame() override;
	void render_ui(ImDrawData* draw_data) override;
	const DepthPyramid& occlusion_pyramid() const override
	{
		return occlusion_pyramid_;
	}
	bool occlusion_enabled() const override { return occlusion_enabled_; }
	bool occlusion_ready() const override { return occlusion_ready_; }
	float occlusion_aspect() const override { return occlusion_aspect_; }
	void suppress_occlusion_record() override { record_occlusion_ = false; }
	uint32_t vertex_capacity() const override { return vertex_capacity_; }
	bool viewport_ready() const override;
	uint64_t viewport_ui_texture() const override;
	uint32_t viewport_width() const override;
	uint32_t viewport_height() const override;
	bool begin_viewport_pass() override;
	void end_viewport_pass() override;
	bool resize_viewport(uint32_t width, uint32_t height) override;
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
	/// Never-reset pool for cached descriptor sets (rewritten only when
	/// their contents change). Steady-state frames allocate and update
	/// nothing; sets for retired images/samplers are dropped from the maps
	/// below and their pool space is left alone (recreates are rare).
	VkDescriptorPool cached_descriptors_{VK_NULL_HANDLE};
	/// Sampler sets by (albedo, shadow, ao) view + sampler handles.
	std::map<std::array<uint64_t, 6>, VkDescriptorSet> sampler_cache_;
	/// Hi-Z storage sets by (src, dst) view pair; at most K pairs exist.
	struct StorageCacheEntry {
		VkImageView src{VK_NULL_HANDLE};
		VkImageView dst{VK_NULL_HANDLE};
		VkDescriptorSet set{VK_NULL_HANDLE};
	};
	std::vector<StorageCacheEntry> storage_cache_;
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
	// --- FXAA -------------------------------------------------------------------
	// The scene renders into fxaa_source_; end_scene_pass()/end_viewport_pass()
	// resolve it onto the real target with a fullscreen triangle. Disabled or
	// unavailable renders straight to the target, exactly as the reference.
	vk::Image fxaa_source_;
	vk::ImageState fxaa_source_state_;
	uint32_t fxaa_width_{0};
	uint32_t fxaa_height_{0};
	VkPipeline fxaa_pipeline_{VK_NULL_HANDLE};
	VkSampler fxaa_sampler_{VK_NULL_HANDLE};
	bool fxaa_unavailable_{false};
	bool pass_redirected_{false};
	// --- SSAO -------------------------------------------------------------------
	// Quarter-res AO target, cleared to white, produced by run_ssao_pass() at
	// the end of the scene pass and sampled by the NEXT frame's lit pass
	// (one frame of lag, no circular dependency). Disabled or unavailable
	// binds the white texture (AO = 1).
	vk::Image ssao_texture_;
	vk::ImageState ssao_state_;
	uint32_t ssao_width_{0};
	uint32_t ssao_height_{0};
	uint32_t ssao_target_width_{0};
	uint32_t ssao_target_height_{0};
	VkPipeline ssao_pipeline_{VK_NULL_HANDLE};
	VkSampler ssao_sampler_{VK_NULL_HANDLE};
	VkSampler ssao_depth_sampler_{VK_NULL_HANDLE};
	bool ssao_unavailable_{false};
	// --- viewport (editor panel) ------------------------------------------------
	// Own colour, depth and single-mip linear-depth images; the Hi-Z pyramid
	// is never recorded from this pass. viewport_pass_open_ joins
	// scene_pass_open_ as a pass draw()/draw_sky() record into.
	vk::Image viewport_color_;
	vk::ImageState viewport_color_state_;
	vk::Image viewport_depth_;
	vk::ImageState viewport_depth_state_;
	vk::Image viewport_linear_;
	vk::ImageState viewport_linear_state_;
	uint32_t viewport_width_{0};
	uint32_t viewport_height_{0};
	bool viewport_pass_open_{false};
	// --- deferred destruction ---------------------------------------------------
	// Images replaced by a resize (or a lazy FXAA/SSAO recreate) while frames
	// are in flight: moved here, swept into the next acquired slot's garbage
	// list, destroyed when that slot is reused (a full ring turn later, so no
	// submitted command buffer can still reference them).
	std::vector<std::function<void()>> retired_targets_;
	/// 1024^2 D16 depth rendered from the light.
	vk::Image shadow_map_;
	vk::ImageState shadow_map_state_;
	bool shadows_supported_{false};
	bool shadow_support_queried_{false};
	bool shadow_rendered_{false};
	glm::mat4 light_vp_{1.0f};

	// --- per-pass state -----------------------------------------------------
	/// Whether BeginRendering has been recorded for the open pass. Rendering
	/// begins on the first geometry draw (which then guarantees the sky runs
	/// and covers target 1, so its clear is skipped), or with a full clear
	/// at the end of a pass that drew nothing.
	bool pass_rendering_begun_{false};
	bool sky_drawn_{false};
	uint32_t pass_width_{0};
	uint32_t pass_height_{0};
	float clear_r_{0.0f};
	float clear_g_{0.0f};
	float clear_b_{0.0f};
	bool scene_pass_open_{false};
	/// Colour attachment 0 of the open pass: the real target, or the FXAA
	/// intermediate when the pass was redirected.
	VkImageView pass_color_view_{VK_NULL_HANDLE};

	// --- windowed swapchain -------------------------------------------------
	// Only a session init()ed with a window touches this: offscreen sessions
	// never create a swapchain, so the exactness gate cannot regress. Every
	// colour target the session renders (scene, FXAA intermediate, viewport)
	// uses pipeline_color_format_, which is the swapchain format once one
	// exists and R8G8B8A8_UNORM otherwise; the depth, pyramid and SSAO
	// targets keep their own formats in both sessions.
	bool windowed_{false};
	VkSwapchainKHR swapchain_{VK_NULL_HANDLE};
	VkFormat swapchain_format_{VK_FORMAT_UNDEFINED};
	uint32_t swapchain_width_{0};
	uint32_t swapchain_height_{0};
	std::vector<VkImage> swapchain_images_;
	std::vector<VkImageView> swapchain_views_;
	/// Per-image layout state, tracked through the shared BarrierBatch like
	/// every other image: after present the image is in PRESENT_SRC_KHR,
	/// which the next acquire observes.
	std::vector<vk::ImageState> swapchain_states_;
	uint32_t swapchain_index_{0};
	/// Present or acquire returned SUBOPTIMAL or OUT_OF_DATE, or the window
	/// changed size: rebuild the swapchain in the next begin_frame, at the
	/// window's current size, before acquiring.
	bool swapchain_rebuild_{false};
	/// The last swapchain rebuild failure that was reported, so a persistent
	/// failure is said once instead of on every skipped frame.
	std::string swapchain_error_;
	VkFormat pipeline_color_format_{VK_FORMAT_R8G8B8A8_UNORM};
	/// Colour format the FXAA source image and resolve pipeline were built
	/// for (always pipeline_color_format_); UNDEFINED when neither exists.
	VkFormat fxaa_format_{VK_FORMAT_UNDEFINED};
	/// Signalled by the acquire and awaited by the submit of the same frame.
	/// One per frame slot: begin_frame waits the slot's fence before
	/// acquiring, so the frame that last waited on it has finished (an
	/// acquire needs its semaphore free of pending operations).
	std::array<VkSemaphore, GpuRenderer::kFramesInFlight> acquire_semaphores_{};
	/// Signalled by the submit and awaited by the present. One per swapchain
	/// image, indexed by the acquired image: a present's wait is known to be
	/// done only once its image is acquired again, so a semaphore shared
	/// across images could be re-signalled while still pending
	/// (VUID-vkQueueSubmit2-semaphore-03868).
	std::vector<VkSemaphore> present_semaphores_;

	// --- ImGui --------------------------------------------------------------
	bool ui_ready_{false};
	bool ui_pipeline_ready_{false};
	VkFormat ui_pipeline_format_{VK_FORMAT_UNDEFINED};
	/// Viewport texture as the ImGui backend's descriptor set (its
	/// ImTextureID); null until first use, re-registered on every resize.
	VkDescriptorSet viewport_ui_set_{VK_NULL_HANDLE};

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
	void end_frame_submit(VkSemaphore wait_semaphore,
	                      VkSemaphore signal_semaphore);
	// renderer_window.cpp (swapchain, present, ImGui)
	/// True when this frame renders into the swapchain: a windowed session
	/// with a live swapchain and an acquired image.
	bool windowed_target() const
	{
		return windowed_ && swapchain_ != VK_NULL_HANDLE &&
		       swapchain_index_ < swapchain_views_.size();
	}
	/// The acquired swapchain image as a transient wrapper for the barrier
	/// batch (ownership stays with the swapchain; layout state lives in
	/// swapchain_states_).
	vk::Image windowed_image() const;
	void destroy_swapchain();
	/// (Re)builds the swapchain at \p width x \p height, or at the size the
	/// surface dictates. Returns false, touching nothing, while the surface
	/// has no pixels to present to (a minimized window can report 0x0): that
	/// is not a failure, the caller skips the frame. Vulkan failures throw.
	bool create_swapchain(uint32_t width, uint32_t height);
	/// Acquires the next image into swapchain_index_. False when the frame
	/// must be skipped; an out-of-date swapchain is rebuilt by the next
	/// begin_frame.
	bool acquire_swapchain_image();
	void end_frame_windowed();
	void present_swapchain();
	/// Rebuilds the format-dependent pipelines (lit; sky and FXAA lazily)
	/// and retires the viewport when the swapchain format changed.
	void on_swapchain_format(VkFormat format);
	void create_ui_pipeline();
	void refresh_viewport_ui_texture();
	/// Waits for the slot's previous frame to finish (its fence) without
	/// taking the slot.
	void wait_slot(FrameSlot& slot);
	/// Takes a slot whose fence wait_slot saw signalled: resets the fence
	/// and the slot's per-frame state.
	FrameSlot& recycle_slot(FrameSlot& slot);
	/// wait_slot + recycle_slot on the next slot of the ring.
	FrameSlot& acquire_slot();
	void destroy_garbage(FrameSlot& slot);
	void destroy_slot(FrameSlot& slot);
	void create_slot(FrameSlot& slot);
	/// Writes \p data into the slot's UBO ring, aligned, and returns its offset.
	uint64_t push_uniforms(const void* data, uint64_t size);
	void set_viewport(VkCommandBuffer cmd, uint32_t width, uint32_t height,
	                  float max_depth);
	void set_scissor(VkCommandBuffer cmd, uint32_t width, uint32_t height);
	void begin_rendering(VkCommandBuffer cmd, bool skip_target1_clear);
	void destroy_pipelines();

	// renderer_post.cpp (FXAA, SSAO, viewport pass, frame-ring retirement)
	void destroy_fxaa();
	void destroy_ssao();
	void destroy_viewport();
	/// Moves \p image (with its view) into deferred destruction: destroyed
	/// when a frame-ring slot is reused, never while submitted work reads it.
	void retire_image(vk::Image image);
	void retire_pyramid_gpu();
	std::vector<uint8_t> download_image_rgba(vk::Image& image,
	                                         vk::ImageState& state,
	                                         uint32_t width, uint32_t height);

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
	/// (Re)builds the lit pipeline for pipeline_color_format_; layouts and
	/// the shadow pipeline are format-independent and survive.
	void create_lit_pipeline();
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
	/// Creates cached_descriptors_ on first use (init() also calls it
	/// directly, so mid-frame callers never branch on init order).
	void ensure_descriptor_cache();
	void create_descriptor_cache();
	void destroy_descriptor_cache();
	/// Drops the content-keyed sampler/storage entries: call after any
	/// image view or sampler they may name is destroyed. The sets themselves
	/// stay alive in the cache pool (frames in flight may still read them);
	/// only their pool space is left behind.
	void clear_descriptor_caches();
	// renderer_occlusion.cpp
	bool create_occlusion_pipeline();
	void destroy_occlusion();
	void record_pyramid();
	void poll_pyramid();
	VkDescriptorSet alloc_storage_set(VkImageView src, VkImageView dst);
	void order_storage_write(VkCommandBuffer cmd, uint32_t mip);

	// renderer_post.cpp (FXAA, SSAO, viewport pass)
	bool ensure_fxaa_source(uint32_t width, uint32_t height);
	bool ensure_fxaa_pipeline();
	void fxaa_resolve(vk::Image& destination, vk::ImageState& destination_state,
	                  uint32_t width, uint32_t height);
	bool ensure_ssao_resources(uint32_t width, uint32_t height);
	bool ensure_ssao_pipeline();
	void run_ssao_pass(uint32_t width, uint32_t height);
	/// Lit sampler set for \p albedo with the current shadow/SSAO bindings
	/// (SSAO = previous frame's texture, or white when off/unavailable).
	VkDescriptorSet alloc_lit_sampler_set(VkImageView albedo);
	bool ssao_on() const;

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

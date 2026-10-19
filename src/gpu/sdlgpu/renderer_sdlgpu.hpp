#pragma once

#include <SDL3/SDL_gpu.h>

#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "lotus/gpu/renderer.hpp"

namespace lotus {

/// SDL3 GPU backend (Vulkan on Linux). Owns the device, the lit pipeline,
/// a depth buffer, and a dynamic vertex buffer.
///
/// Frame stages (all on one command buffer):
///   begin_frame()  -> acquire command buffer + swapchain (+ depth resize)
///   acquire_mesh() per cached mesh the frame draws, then
///   begin_geometry_upload() + commit_geometry_upload()
///                  -> copy pass (outside a render pass)
///   begin_scene_pass() / draw() / end_scene_pass()
///   [UI layer may open its own pass here]
///   end_frame()    -> submit
class GpuRendererSdlGpu final : public GpuRenderer {
public:
	GpuRendererSdlGpu() = default;

	~GpuRendererSdlGpu() override;

	GpuRendererSdlGpu(const GpuRendererSdlGpu&)            = delete;

	GpuRendererSdlGpu& operator=(const GpuRendererSdlGpu&) = delete;

	void init(SDL_Window* window, bool debug_device = true) override;

	void shutdown() override;

	/// Headless device that renders into an offscreen colour target instead of
	/// a window. Used for tests and frame capture (and, later, the editor
	/// viewport).
	void init_offscreen(uint32_t width, uint32_t height) override;

	/// Starts a frame against the offscreen target. Use after init_offscreen.
	void begin_frame_offscreen() override;

	/// Downloads the offscreen colour target as tightly packed RGBA8.
	/// Returns an empty vector if this renderer is not in offscreen mode.
	std::vector<uint8_t> capture_rgba(uint32_t& width, uint32_t& height) override;

	/// Downloads the editor viewport texture as tightly packed RGBA8. Returns
	/// an empty vector when there is no viewport texture.
	std::vector<uint8_t> capture_viewport_rgba(uint32_t& width,
	                                           uint32_t& height) override;

	/// Resizes the offscreen target, and with it the depth buffer and the
	/// occlusion pyramid. Offscreen mode only. Returns false, leaving the
	/// renderer untouched, when not in offscreen mode or the new colour
	/// target could not be created. The pyramid is invalidated either way, so
	/// occlusion_ready() is false until a frame has been recorded at the new
	/// size.
	bool resize_offscreen(uint32_t width, uint32_t height) override;

	/// Offscreen capture written as a binary PPM (P6), which needs no encoder.
	bool write_ppm(const std::string& path, std::string* error = nullptr) override;

	/// Blocks until the GPU has finished every frame submitted so far.
	void wait_idle() override;

	/// Names of the GPU ("NVIDIA GeForce RTX 3080") and of its driver
	/// ("NVIDIA"); valid until shutdown(), empty before init.
	const char* device_name() const override;

	const char* device_driver() const override;

	const char* driver_name() const override;

	// --- frame ---

	void begin_frame(SDL_Window* window, float clear_r, float clear_g,
	                 float clear_b) override;

	// --- geometry ---
	//
	// A frame's geometry is a set of instanced draws of cached meshes plus,
	// for what cannot be cached, one world-space vertex stream (the
	// "immediate" stream). Cached meshes are uploaded once, in local space,
	// and drawn through an index buffer; each frame uploads only the per
	// instance records (model matrix and material), a few dozen bytes per
	// entity instead of every vertex of every entity.

	/// Makes the geometry \p data resident on the GPU under \p key and returns
	/// its handle, uploading it on first use (the copy is recorded by the next
	/// commit_geometry_upload). A mesh is identified by \p key alone: asking
	/// again with the same key and the same vertex and index counts returns
	/// the cached mesh and ignores \p data, so a key must always name the same
	/// geometry. Different counts replace the mesh. Returns kNoGpuMesh when
	/// there is nothing to draw (no vertices, or no index naming one) or no
	/// device. Handles stay valid until the mesh has been idle for
	/// kMeshIdleFrames; that happens between frames, never during one.
	GpuMeshId acquire_mesh(const std::string& key, const GpuMeshData& data) override;

	/// Indices drawn per instance of \p mesh: the source indices that name an
	/// existing vertex. 0 for a handle that is not valid.
	uint32_t mesh_index_count(GpuMeshId mesh) const override;

	/// Meshes currently resident (diagnostics and tests).
	uint32_t cached_mesh_count() const override;

	/// Maps staging memory for this frame's geometry: \p immediate_vertices
	/// world-space vertices (and as many UVs) and \p instance_count instance
	/// records, so the caller generates them in place with no intermediate
	/// copy. Write every entry, sequentially: the memory may be
	/// write-combined, so never read it back. Returns all-null when no frame
	/// is in flight or both counts are zero. Call commit_geometry_upload()
	/// before the scene pass. Also starts a new draw list: the previous
	/// frame's is dropped.
	GeometryStaging begin_geometry_upload(uint32_t immediate_vertices,
	                                      uint32_t instance_count) override;

	/// Unmaps the staging memory, records every copy the frame needs (the
	/// staged vertices and instances, and the geometry of any mesh
	/// acquire_mesh queued) and keeps \p batches as the draw list the shadow
	/// pass replays. OUTSIDE a render pass.
	void commit_geometry_upload(const GpuDrawBatch* batches,
	                            uint32_t batch_count) override;

	void begin_scene_pass() override;

	/// Draws \p batches with the lit pipeline, binding each albedo texture
	/// once per run of batches that share it. \p view is uploaded alongside
	/// \p view_projection because the vertex stage also writes view-space
	/// depth for the occlusion pyramid. The batches must refer to geometry
	/// uploaded this frame.
	///
	/// Rasterises into the lower half of the depth range and then draws the
	/// sky (when enabled) over what nothing covered, so a pass is: geometry
	/// first, sky last, and depth 1.0 means "untouched". Anything else added to
	/// this pass with a depth test has to use that range too (see
	/// set_pass_viewport).
	void draw(const glm::mat4& view, const glm::mat4& view_projection,
	          const glm::vec3& camera_position, const GpuDrawBatch* batches,
	          uint32_t batch_count, const AssetRegistry* assets) override;

	void end_scene_pass() override;

	void end_frame() override;

	/// False when no swapchain texture was available (frame should be skipped).
	bool has_frame() const override { return cmd_ != nullptr; }

	// --- UI layer (Dear ImGui) ---
	//
	// The renderer owns the ImGui renderer backend, so the editor never
	// touches a GPU handle; the platform backend (ImGui_ImplSDL3_*) stays
	// with the editor.

	/// Creates the ImGui renderer backend that draws onto the swapchain.
	/// False when it could not be created.
	bool init_ui_backend() override;

	void shutdown_ui_backend() override;

	/// Per-frame backend state; call before ImGui::NewFrame().
	void ui_backend_new_frame() override;

	/// Records \p draw_data over the swapchain image of the frame in flight,
	/// keeping what is already there. Does nothing for a frame without one.
	void render_ui(ImDrawData* draw_data) override;

	/// CPU copy of the previous frame's depth pyramid. Never null; empty when
	/// the fence has not landed yet, in which case occlusion is skipped.
	const DepthPyramid& occlusion_pyramid() const override { return occlusion_pyramid_; }

	/// False when compute shaders or R32 storage are unavailable. Culling then
	/// falls back to frustum and LOD only, with no errors.
	bool occlusion_enabled() const override { return occlusion_enabled_; }

	/// True once a downloaded pyramid has landed. False means this frame skips
	/// occlusion because the previous frame's fence is still outstanding.
	bool occlusion_ready() const override { return occlusion_ready_; }

	/// Aspect (width / height) of the depth image currently in
	/// occlusion_pyramid(). Zero when no image has been adopted.
	float occlusion_aspect() const override { return occlusion_aspect_; }

	/// The next end_scene_pass closes the pass without downloading a pyramid.
	/// Used when the pass only clears the window behind the editor.
	void suppress_occlusion_record() override { record_occlusion_ = false; }

	uint32_t vertex_capacity() const override { return capacity_; }

	/// True when the viewport texture is ready to be sampled by ImGui.
	bool viewport_ready() const override { return viewport_texture_ != nullptr; }

	/// The viewport texture as an ImGui texture id (ImGui::Image). Valid only
	/// when viewport_ready() is true.
	uint64_t viewport_ui_texture() const override;

	/// Width of the viewport texture in pixels.
	uint32_t viewport_width() const override { return viewport_width_; }

	/// Height of the viewport texture in pixels.
	uint32_t viewport_height() const override { return viewport_height_; }

	/// Starts a render pass into the viewport texture. Call resize_viewport
	/// first if the size changed. Returns false when the texture is not ready.
	bool begin_viewport_pass() override;

	/// Ends the viewport render pass.
	void end_viewport_pass() override;

	/// Resizes the viewport texture and its depth buffer. Returns false on
	/// failure, leaving the old texture intact.
	bool resize_viewport(uint32_t width, uint32_t height) override;

	/// Frames actually submitted (excludes skipped/stalled frames).
	uint64_t frames_rendered() const override { return frames_rendered_; }

	LightingSettings& lighting() override { return lighting_; }

	const LightingSettings& lighting() const override { return lighting_; }

	/// Analytic blob casters for this frame (xyz center in world space,
	/// w radius). Copies up to 4; extra entries are ignored. Entry points
	/// keep their signatures: scenes that never call this render with no
	/// blobs, exactly the legacy path.
	void set_blob_casters(const glm::vec4* spheres, uint32_t count) override;

	void clear_blob_casters() override;

	uint32_t blob_count() const override { return blob_count_; }

	const glm::vec4* blob_spheres() const override { return blob_spheres_; }

private:
	/// A mesh in the cache; see acquire_mesh().
	struct CachedMesh {
		SDL_GPUBuffer* vertex{nullptr};
		SDL_GPUBuffer* uv{nullptr};
		SDL_GPUBuffer* index{nullptr};
		uint32_t vertex_count{0};       // vertices in the source geometry
		uint32_t source_index_count{0}; // indices in the source geometry
		uint32_t index_count{0};        // indices that name a vertex: drawn
		uint64_t last_used{0};          // frames_rendered_ at the last acquire
		std::string key;
		bool live{false};
	};
	/// Geometry queued by acquire_mesh(), written to the GPU by the next
	/// commit_geometry_upload().
	struct PendingMeshUpload {
		GpuMeshId id{kNoGpuMesh};
		std::vector<GpuVertex> vertices;
		std::vector<glm::vec2> uvs;
		std::vector<uint32_t> indices;
	};
	/// One recorded draw of the frame's geometry: what the lit pass takes from
	/// a GpuDrawBatch and the shadow pass replays.
	struct GeometryDraw {
		GpuMeshId mesh{kNoGpuMesh};
		uint32_t first_vertex{0};
		uint32_t vertex_count{0};
		uint32_t first_instance{0};
		uint32_t instance_count{0};
	};

	/// Waits for the GPU, then reads \p texture (RGBA8, \p width x \p height)
	/// back as tightly packed bytes.
	std::vector<uint8_t> download_rgba(SDL_GPUTexture* texture, uint32_t width,
	                                   uint32_t height);

	void create_pipeline();
	/// Starts a frame's geometry bookkeeping: drops the previous frame's draw
	/// list and staging, and lets meshes that stopped being drawn age out.
	void begin_geometry_frame();
	void ensure_vertex_capacity(uint32_t vertex_bytes, uint32_t uv_bytes);
	void ensure_depth_target(uint32_t width, uint32_t height);
	void destroy_buffers();
	void ensure_instance_capacity(uint32_t bytes);
	void destroy_instance_buffers();
	bool mesh_live(GpuMeshId mesh) const;
	void release_mesh(GpuMeshId mesh);
	void evict_idle_meshes();
	void destroy_meshes();
	void upload_pending_meshes(SDL_GPUCopyPass* copy);
	void abandon_geometry_staging();
	/// Binds the vertex buffers of \p draw and records it on \p pass. The
	/// pipeline must already be bound.
	void record_geometry_draw(SDL_GPURenderPass* pass, const GeometryDraw& draw);
	/// Pushes the lit uniforms and binds the lit pipeline with the shadow and
	/// SSAO samplers: everything a lit draw needs except the albedo texture
	/// and the geometry.
	void bind_lit_state(const glm::mat4& view, const glm::mat4& view_projection,
	                    const glm::vec3& camera_position);

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
	/// Frame fences. Every offscreen frame, and every windowed frame that
	/// downloads a pyramid, is submitted with one; they queue in submit order.
	/// reap_frame_fences() collects the finished ones without waiting,
	/// wait_for_frame_slot() blocks until fewer than kFramesInFlight frames
	/// are outstanding, and release_frame_fences() drops them all once the GPU
	/// is idle.
	void reap_frame_fences();
	void wait_for_frame_slot();
	void release_frame_fences();
	void ensure_default_sampler();
	void ensure_white_texture();
	void destroy_textures();
	SDL_GPUTexture* texture_for_albedo(const std::string& albedo_id,
	                                   const AssetRegistry* assets);
	bool load_texture_from_file(const std::string& path,
	                            const std::string& cache_key);
	// --- directional shadow map ---
	// Rendered from the light once per frame (first scene/viewport pass wins)
	// into shadow_texture_, then sampled by lit.frag with a 3x3 PCF. All
	// entry points keep their signatures: the light VP is derived from
	// lighting_.direction over a fixed ortho box, and the pass reuses the
	// already-uploaded vertex buffers.
	bool shadows_ready();
	void ensure_shadow_resources();
	void render_shadow_map();
	void shadow_light_matrices(glm::mat4& view, glm::mat4& vp) const;
	void destroy_shadows();
	// --- Procedural sky background (gradient dome + sun disk, no textures) ---
	// Drawn bufferless (fullscreen triangle from gl_VertexIndex) inside the
	// scene pass, once per pass, as the last step of the first draw() that has
	// geometry: it shades only the pixels no geometry covered, so its fragment
	// shader never runs under a surface. That needs depth to tell "untouched"
	// from "geometry past the far plane" (the pipelines clamp depth, so such
	// geometry lands at the top of its range): geometry is rasterised into
	// [0, 0.5] through the viewport and the sky sits at 1.0 (LESS_OR_EQUAL,
	// no depth write). The pipeline declares the same two colour targets plus
	// depth as the lit pipeline, so the occlusion-pyramid location-1 contract
	// is untouched; it masks that target's write, because the pass clears it
	// to the far sentinel sky.frag would write. Entry points keep their
	// signatures: draw() draws it from the view matrices it already receives.
	// sky_enabled=false restores the legacy clear-color path.
	bool ensure_sky_pipeline();
	void draw_sky(const glm::mat4& view_projection,
	              const glm::vec3& camera_position);
	void destroy_sky();
	// Viewport of the open pass: its whole target, depth range [0, max_depth].
	void set_pass_viewport(float max_depth);
	// --- FXAA 3.11 console post pass ---
	// The scene renders into fxaa_source_; end_scene_pass()/end_viewport_pass()
	// resolve it onto the real target (swapchain, viewport, offscreen) with a
	// fullscreen-triangle pass, or skip cleanly when the target cannot host it.
	bool ensure_fxaa_source(uint32_t width, uint32_t height);
	bool ensure_fxaa_pipeline();
	void fxaa_resolve(SDL_GPUTexture* destination, uint32_t width,
	                  uint32_t height);
	void destroy_fxaa();
	// --- SSAO (fullscreen AO from the linear-depth MRT, quarter-res allowed)
	// The scene pass writes linear view depth as colour target 1; the SSAO
	// pass samples the previous frame's depth texture with 8-16 taps and
	// writes single-channel AO. lit.frag samples that AO (via gl_FragCoord)
	// and scales ONLY ambient/hemisphere/bounce, never the sun.
	// ssao_enabled=false binds white (AO=1), restoring the legacy path.
	// Entry points keep their signatures: the pass runs lazily inside
	// end_scene_pass from state draw() already receives.
	bool ensure_ssao_resources(uint32_t width, uint32_t height);
	bool ensure_ssao_pipeline();
	void run_ssao_pass(uint32_t width, uint32_t height);
	void destroy_ssao();

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
	/// Per-instance records (slot 2 of the lit and shadow pipelines). Entry 0
	/// is reserved for the identity the immediate stream draws with.
	SDL_GPUBuffer* instance_buffer_{nullptr};
	SDL_GPUTransferBuffer* instance_transfer_{nullptr};
	uint32_t instance_capacity_{0};

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
	/// Submit serial of the frame whose pyramid download is in flight; valid
	/// while pyramid_in_flight_.
	uint64_t pyramid_serial_{0};
	bool pyramid_in_flight_{false};
	/// A submitted frame's fence, in submit order. Owned here and released
	/// only once reaped, so a fence is never handed back to SDL's pool while
	/// the GPU may still be using it, whatever became of the resources the
	/// frame read.
	struct FrameFence {
		uint64_t serial;
		SDL_GPUFence* fence;
	};
	std::deque<FrameFence> frame_fences_;
	uint64_t submit_serial_{0};    // frames submitted so far
	uint64_t completed_serial_{0}; // every frame up to this one has finished
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
	// --- directional shadow map (1024px D16, depth-only pass from the light) ---
	SDL_GPUTexture* shadow_texture_{nullptr};
	SDL_GPUGraphicsPipeline* shadow_pipeline_{nullptr};
	SDL_GPUSampler* shadow_sampler_{nullptr};
	// Tri-state: queried once, then cached. False (unsupported format, failed
	// creation) means every frame silently takes the legacy unshadowed path.
	bool shadow_support_queried_{false};
	bool shadows_supported_{false};
	bool shadow_rendered_{false};
	glm::mat4 light_vp_{1.0f};
	std::vector<CachedMesh> meshes_;
	std::vector<GpuMeshId> free_mesh_ids_;
	std::unordered_map<std::string, GpuMeshId> mesh_ids_;
	std::vector<PendingMeshUpload> pending_meshes_;
	/// This frame's draw list, kept for the shadow pass. Cleared when a frame
	/// begins, so a frame that uploads nothing draws no shadows.
	std::vector<GeometryDraw> geometry_draws_;
	uint32_t staged_vertices_{0};
	uint32_t staged_instances_{0};
	bool geometry_mapped_{false};
	uint32_t swapchain_width_{0};
	uint32_t swapchain_height_{0};
	// --- FXAA 3.11 console post pass (fullscreen triangle onto the target) ---
	SDL_GPUGraphicsPipeline* fxaa_pipeline_{nullptr};
	SDL_GPUTextureFormat fxaa_format_{SDL_GPU_TEXTUREFORMAT_INVALID};
	SDL_GPUTexture* fxaa_source_{nullptr};
	uint32_t fxaa_width_{0};
	uint32_t fxaa_height_{0};
	SDL_GPUSampler* fxaa_sampler_{nullptr};
	bool fxaa_unavailable_{false};
	// --- Procedural sky background (see ensure_sky_pipeline above) ---
	SDL_GPUGraphicsPipeline* sky_pipeline_{nullptr};
	SDL_GPUTextureFormat sky_format_{SDL_GPU_TEXTUREFORMAT_INVALID};
	bool sky_unavailable_{false};
	// True once the sky has been drawn into the current pass_; reset by
	// begin_scene_pass()/begin_viewport_pass().
	bool sky_drawn_{false};
	// Size of the open pass's target (what its default viewport covers), set
	// by begin_scene_pass()/begin_viewport_pass(), for set_pass_viewport().
	uint32_t pass_width_{0};
	uint32_t pass_height_{0};
	// Set by begin_scene_pass()/begin_viewport_pass() when the scene renders
	// into fxaa_source_; the matching end_*_pass() resolves onto the target.
	bool pass_redirected_{false};
	SDL_GPUTexture* pass_destination_{nullptr};
	SDL_GPUSampler* default_sampler_{nullptr};
	SDL_GPUTexture* white_texture_{nullptr};
	// --- Analytic blob shadows (up to 4 uniform spheres, direct lobe only)
	glm::vec4 blob_spheres_[4]{};
	uint32_t blob_count_{0};
	// --- SSAO (samples previous frame's linear depth, scales fill only)
	SDL_GPUGraphicsPipeline* ssao_pipeline_{nullptr};
	SDL_GPUTexture* ssao_texture_{nullptr};
	uint32_t ssao_width_{0};
	uint32_t ssao_height_{0};
	SDL_GPUSampler* ssao_sampler_{nullptr};
	SDL_GPUSampler* ssao_depth_sampler_{nullptr};
	bool ssao_unavailable_{false};
	uint32_t ssao_target_width_{0};
	uint32_t ssao_target_height_{0};
	struct CachedAlbedo {
		SDL_GPUTexture* texture{nullptr};
	};
	std::unordered_map<std::string, CachedAlbedo> albedo_textures_;
};

} // namespace lotus

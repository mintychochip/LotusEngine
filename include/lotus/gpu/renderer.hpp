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

/// Reference gray the hemisphere colours are stored against. Sky/ground
/// default to exactly this, so the hemisphere multiplier below is 1.0 and an
/// old document renders the legacy flat floor bit-for-bit. Matches
/// HEMI_REFERENCE in shaders/lit.frag; keep the two in sync.
inline const glm::vec3 kHemiReference{0.15f, 0.15f, 0.18f};

inline glm::vec3 srgb_to_linear_vec3(const glm::vec3& c)
{
	return {srgb_to_linear(c.x), srgb_to_linear(c.y), srgb_to_linear(c.z)};
}

/// Decode-then-multiply albedo: vertex color and texel are each sRGB-encoded,
/// so each is decoded to linear first and then multiplied. Matches lit.frag.
/// (The old `srgb_to_linear(v_color * tex)` decoded the product and washed
/// mid-tones out.)
inline glm::vec3 decode_albedo(const glm::vec3& vertex_color,
                               const glm::vec3& texel)
{
	const glm::vec3 a = srgb_to_linear_vec3(vertex_color);
	const glm::vec3 b = srgb_to_linear_vec3(texel);
	return {a.x * b.x, a.y * b.y, a.z * b.z};
}

/// Linear-space exponential fog factor for a view depth (world units) and a
/// small density. Matches lit.frag: 1 - exp(-density * depth), clamped.
inline float fog_amount(float view_depth, float density)
{
	if (density <= 0.0f || view_depth <= 0.0f) {
		return 0.0f;
	}
	return clamp01(1.0f - std::exp(-density * view_depth));
}

inline glm::vec3 apply_fog(const glm::vec3& outgoing,
                           const glm::vec3& fog_color, float amount)
{
	const float t = clamp01(amount);
	return {outgoing.x + (fog_color.x - outgoing.x) * t,
	        outgoing.y + (fog_color.y - outgoing.y) * t,
	        outgoing.z + (fog_color.z - outgoing.z) * t};
}

/// Third-directional bounce fill: opposite the sun in XZ, unshadowed like the
/// hemisphere. Matches lit.frag.
inline glm::vec3 bounce_direction(const glm::vec3& sun_direction)
{
	const glm::vec3 b = {sun_direction.x * -1.0f, sun_direction.y,
	                     sun_direction.z * -1.0f};
	const float len   = std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z);
	if (len <= 0.0f) {
		return {0.0f, 1.0f, 0.0f};
	}
	return {b.x / len, b.y / len, b.z / len};
}

/// Sun angular radius (radians-ish soft-edge scale) for blob shadows.
/// Matches BLOB_SUN_RADIUS in lit.frag.
inline constexpr float kBlobSunRadius = 0.05f;
inline constexpr uint32_t kMaxBlobCasters = 4;

/// Analytic blob term for one caster: 1 lit far away, ~0.3 directly under the
/// center, with a soft edge scaled by the sun angular radius. Matches
/// lit.frag's `blob_tap`.
inline float blob_tap(const glm::vec3& world_position,
                      const glm::vec3& center, float radius)
{
	if (radius <= 0.0f) {
		return 1.0f;
	}
	const float dx = world_position.x - center.x;
	const float dy = world_position.y - center.y;
	const float dz = world_position.z - center.z;
	const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
	const float soft = radius * kBlobSunRadius + 0.05f;
	const float lo   = radius - soft;
	const float hi   = radius + soft;
	float t = hi <= lo ? (dist >= radius ? 1.0f : 0.0f)
	                   : (dist - lo) / (hi - lo);
	t = clamp01(t);
	const float smooth = t * t * (3.0f - 2.0f * t);
	// Height fade: a fragment far above the caster escapes its shadow.
	const float above = world_position.y - center.y;
	const float fade =
	        above <= 0.0f ? 1.0f : clamp01(1.0f - above / (radius * 4.0f + 1e-4f));
	return 1.0f - (1.0f - (0.3f + 0.7f * smooth)) * fade;
}

inline float blob_shadow(const glm::vec3& world_position,
                         const glm::vec4* spheres, uint32_t count)
{
	float shadow = 1.0f;
	for (uint32_t i = 0; i < count; ++i) {
		const glm::vec3 center{spheres[i].x, spheres[i].y, spheres[i].z};
		shadow *= blob_tap(world_position, center, spheres[i].w);
	}
	return shadow;
}

 /// Reference gray the hemisphere colours are stored against. Sky/ground
 /// default to exactly this, so the hemisphere multiplier below is 1.0 and an
 /// old document renders the legacy flat floor bit-for-bit. Matches
 /// HEMI_REFERENCE in shaders/lit.frag; keep the two in sync.

/// Hemisphere blend weight from world normal Y: 0 facing down, 1 facing up.
/// Matches `hemi_t` in shaders/lit.frag.
inline float hemisphere_blend(float normal_y)
{
	return clamp01(normal_y * 0.5f + 0.5f);
}

/// Hemisphere ambient floor for one unit of albedo: the legacy `ambient`
/// multiplied by the sky/ground blend, normalized by kHemiReference so the
/// defaults reproduce the legacy floor exactly. When ambient is at its
/// default the sky/ground colours apply verbatim; moving ambient scales the
/// whole hemisphere as a gain. Matches lit.frag's `hemi` term.
inline glm::vec3 hemisphere_ambient(const glm::vec3& ambient,
                                    const glm::vec3& sky,
                                    const glm::vec3& ground, float normal_y)
{
	const float t = hemisphere_blend(normal_y);
	const glm::vec3 hemi =
	        ground + (sky - ground) * t; // mix(ground, sky, t)
	return {ambient.x * hemi.x / kHemiReference.x,
	        ambient.y * hemi.y / kHemiReference.y,
	        ambient.z * hemi.z / kHemiReference.z};
}

/// One PCF tap: 1 when the surface is lit (sampled occluder at or behind the
/// surface depth minus bias), 0 when shadowed. lit.frag averages nine of
/// these over a 3x3 texel neighbourhood. Matches `shadow_factor`'s tap.
inline float shadow_tap(float surface_depth, float occluder_depth, float bias)
{
	return (surface_depth - bias) <= occluder_depth ? 1.0f : 0.0f;
}

} // namespace pbr

/// A directional light plus a hemisphere ambient floor, evaluated by the
/// Cook-Torrance dielectric BRDF in shaders/lit.frag with ACES tone mapping
/// and sRGB output. `exposure` scales linear HDR before the tone map.
///
/// Retuned for the shadowed pipeline: the sun sits a touch lower so forms
/// model and shadows read, intensity is 1.1 to compensate for light lost to
/// shadowing, exposure stays neutral (the ACES fit is already tuned), and
/// ambient is unified at the legacy renderer gray. `sky_color`/`ground_color`
/// default to that same gray (see pbr::kHemiReference), reproducing the
/// legacy flat floor exactly.
/// `shadows_enabled`/`fxaa_enabled`/`sky_enabled` default ON; each restores
/// the exact legacy path when off (for the sky, the clear-color background).
/// `horizon_warmth` scales the warm horizon glow around the sun azimuth (1.0
/// neutral, 0.0 off). `fog_enabled`/`fog_density` add linear-space exp fog
/// (off restores legacy); `bounce_*` add an unshadowed warm fill from
/// normalize(sun * vec3(-1,0,-1)) (off restores legacy); `ssao_enabled`
/// scales only ambient/hemisphere/bounce (off restores legacy). Color policy:
/// `color`/`sky_color`/`ground_color`/`bounce_color` are sRGB-authored,
/// `ambient` is a linear gain, the rest are linear non-color data. Mirrors
/// SceneSettings; application/headless copy every field across each frame.
struct LightingSettings {
	/// Unit vector pointing toward the light.
	glm::vec3 direction{0.5f, 0.8f, 0.45f};
	glm::vec3 color{1.0f, 1.0f, 1.0f};
	float intensity{1.1f};
	glm::vec3 ambient{0.15f, 0.15f, 0.18f};
	float exposure{1.0f};
	glm::vec3 sky_color{0.15f, 0.15f, 0.18f};
	glm::vec3 ground_color{0.15f, 0.15f, 0.18f};
	bool shadows_enabled{true};
	bool fxaa_enabled{true};
	bool sky_enabled{true};
	float horizon_warmth{1.0f};
	bool fog_enabled{true};
	float fog_density{0.015f};
	bool bounce_enabled{true};
	glm::vec3 bounce_color{0.40f, 0.28f, 0.20f};
	float bounce_intensity{0.5f};
	bool ssao_enabled{true};
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

	/// Staging memory for one frame's geometry; see begin_vertex_upload().
	struct VertexStaging {
		GpuVertex* vertices{nullptr};
		glm::vec2* uvs{nullptr};
	};

	/// Maps the upload staging memory for \p count vertices and UVs so the
	/// caller can generate them in place, skipping the intermediate copy that
	/// upload_vertices() makes. Write exactly \p count entries to each array,
	/// sequentially: the memory may be write-combined, so never read it back.
	/// Call commit_vertex_upload() before the scene pass. Both pointers are
	/// null when no frame is in flight or \p count is zero.
	VertexStaging begin_vertex_upload(uint32_t count);

	/// Unmaps the staging memory and records the copy to the GPU. OUTSIDE a
	/// render pass. No-op unless begin_vertex_upload() returned memory.
	void commit_vertex_upload();

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

	/// Analytic blob casters for this frame (xyz center in world space,
	/// w radius). Copies up to 4; extra entries are ignored. Entry points
	/// keep their signatures: scenes that never call this render with no
	/// blobs, exactly the legacy path.
	void set_blob_casters(const glm::vec4* spheres, uint32_t count);
	void clear_blob_casters();
	uint32_t blob_count() const { return blob_count_; }
	const glm::vec4* blob_spheres() const { return blob_spheres_; }

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
	// Drawn bufferless (fullscreen triangle from gl_VertexIndex) as the first
	// step inside the scene pass, once per pass, before any geometry. The
	// pipeline declares the same two colour targets plus depth as the lit
	// pipeline, so the occlusion-pyramid location-1 contract is untouched;
	// sky.frag writes the far sentinel there, and depth-writes 1.0 with
	// LESS_OR_EQUAL so geometry overdraws it. Entry points keep their
	// signatures: draw() draws it lazily on first use from the view matrices
	// it already receives. sky_enabled=false restores the legacy clear-color
	// path.
	bool ensure_sky_pipeline();
	void draw_sky(const glm::mat4& view_projection,
	              const glm::vec3& camera_position);
	void destroy_sky();
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
	uint32_t uploaded_vertex_count_{0};
	uint32_t staged_vertex_count_{0};
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

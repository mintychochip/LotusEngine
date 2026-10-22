#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "lotus/gpu/draw_batch.hpp"
#include "lotus/gpu/vertex.hpp"
#include "lotus/scene/depth_pyramid.hpp"

struct SDL_Window;
struct ImDrawData;

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

/// Rendering backend interface. The Vulkan backend (src/gpu/vulkan/)
/// implements this contract; callers only ever touch this header and
/// construct through create_gpu_renderer().
///
/// Frame stages (all on one command buffer):
///   begin_frame()  -> acquire command buffer + swapchain (+ depth resize)
///   acquire_mesh() per cached mesh the frame draws, then
///   begin_geometry_upload() + commit_geometry_upload()
///                  -> copy pass (outside a render pass)
///   begin_scene_pass() / draw() / end_scene_pass()
///   [UI layer may open its own pass here]
///   end_frame()    -> submit
class GpuRenderer {
public:
	GpuRenderer() = default;

	virtual ~GpuRenderer() = default;

	GpuRenderer(const GpuRenderer&)            = delete;

	GpuRenderer& operator=(const GpuRenderer&) = delete;

	virtual void init(SDL_Window* window, bool debug_device = true) = 0;

	virtual void shutdown() = 0;

	/// Headless device that renders into an offscreen colour target instead of
	/// a window. Used for tests and frame capture (and, later, the editor
	/// viewport).
	virtual void init_offscreen(uint32_t width, uint32_t height) = 0;

	/// Starts a frame against the offscreen target. Use after init_offscreen.
	virtual void begin_frame_offscreen() = 0;

	/// Downloads the offscreen colour target as tightly packed RGBA8.
	/// Returns an empty vector if this renderer is not in offscreen mode.
	virtual std::vector<uint8_t> capture_rgba(uint32_t& width, uint32_t& height) = 0;

	/// Downloads the editor viewport texture as tightly packed RGBA8. Returns
	/// an empty vector when there is no viewport texture.
	virtual std::vector<uint8_t> capture_viewport_rgba(uint32_t& width,
	                                           uint32_t& height) = 0;

	/// Resizes the offscreen target, and with it the depth buffer and the
	/// occlusion pyramid. Offscreen mode only. Returns false, leaving the
	/// renderer untouched, when not in offscreen mode or the new colour
	/// target could not be created. The pyramid is invalidated either way, so
	/// occlusion_ready() is false until a frame has been recorded at the new
	/// size.
	virtual bool resize_offscreen(uint32_t width, uint32_t height) = 0;

	/// Offscreen capture written as a binary PPM (P6), which needs no encoder.
	virtual bool write_ppm(const std::string& path, std::string* error = nullptr) = 0;

	/// Blocks until the GPU has finished every frame submitted so far.
	virtual void wait_idle() = 0;

	/// Names of the GPU ("NVIDIA GeForce RTX 3080") and of its driver
	/// ("NVIDIA"); valid until shutdown(), empty before init.
	virtual const char* device_name() const = 0;

	virtual const char* device_driver() const = 0;

	virtual const char* driver_name() const = 0;

	// --- frame ---

	virtual void begin_frame(SDL_Window* window, float clear_r, float clear_g,
	                 float clear_b) = 0;

	/// Frames an offscreen renderer lets queue up before begin_frame_offscreen()
	/// waits for the oldest to finish: enough to keep the GPU fed while the CPU
	/// records the next one. A windowed renderer is paced by its swapchain.
	static constexpr uint32_t kFramesInFlight = 2;

	// --- geometry ---
	//
	// A frame's geometry is a set of instanced draws of cached meshes plus,
	// for what cannot be cached, one world-space vertex stream (the
	// "immediate" stream). Cached meshes are uploaded once, in local space,
	// and drawn through an index buffer; each frame uploads only the per
	// instance records (model matrix and material), a few dozen bytes per
	// entity instead of every vertex of every entity.

	/// Idle frames after which a cached mesh is released (5 s at 60 fps).
	/// Re-acquiring a released mesh uploads it again.
	static constexpr uint32_t kMeshIdleFrames = 300;

	/// Makes the geometry \p data resident on the GPU under \p key and returns
	/// its handle, uploading it on first use (the copy is recorded by the next
	/// commit_geometry_upload). A mesh is identified by \p key alone: asking
	/// again with the same key and the same vertex and index counts returns
	/// the cached mesh and ignores \p data, so a key must always name the same
	/// geometry. Different counts replace the mesh. Returns kNoGpuMesh when
	/// there is nothing to draw (no vertices, or no index naming one) or no
	/// device. Handles stay valid until the mesh has been idle for
	/// kMeshIdleFrames; that happens between frames, never during one.
	virtual GpuMeshId acquire_mesh(const std::string& key, const GpuMeshData& data) = 0;

	/// Indices drawn per instance of \p mesh: the source indices that name an
	/// existing vertex. 0 for a handle that is not valid.
	virtual uint32_t mesh_index_count(GpuMeshId mesh) const = 0;

	/// Meshes currently resident (diagnostics and tests).
	virtual uint32_t cached_mesh_count() const = 0;

	/// Staging memory for one frame's geometry; see begin_geometry_upload().
	struct GeometryStaging {
		/// Immediate world-space vertices and their UVs.
		GpuVertex* vertices{nullptr};
		glm::vec2* uvs{nullptr};
		/// Instance records, indexed by GpuDrawBatch::first_instance.
		GpuInstance* instances{nullptr};
	};

	/// Maps staging memory for this frame's geometry: \p immediate_vertices
	/// world-space vertices (and as many UVs) and \p instance_count instance
	/// records, so the caller generates them in place with no intermediate
	/// copy. Write every entry, sequentially: the memory may be
	/// write-combined, so never read it back. Returns all-null when no frame
	/// is in flight or both counts are zero. Call commit_geometry_upload()
	/// before the scene pass. Also starts a new draw list: the previous
	/// frame's is dropped.
	virtual GeometryStaging begin_geometry_upload(uint32_t immediate_vertices,
	                                      uint32_t instance_count) = 0;

	/// Unmaps the staging memory, records every copy the frame needs (the
	/// staged vertices and instances, and the geometry of any mesh
	/// acquire_mesh queued) and keeps \p batches as the draw list the shadow
	/// pass replays. OUTSIDE a render pass.
	virtual void commit_geometry_upload(const GpuDrawBatch* batches,
	                            uint32_t batch_count) = 0;

	virtual void begin_scene_pass() = 0;

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
	virtual void draw(const glm::mat4& view, const glm::mat4& view_projection,
	          const glm::vec3& camera_position, const GpuDrawBatch* batches,
	          uint32_t batch_count, const AssetRegistry* assets) = 0;

	virtual void end_scene_pass() = 0;

	virtual void end_frame() = 0;

	/// False when no swapchain texture was available (frame should be skipped).
	virtual bool has_frame() const = 0;

	// --- UI layer (Dear ImGui) ---
	//
	// The renderer owns the ImGui renderer backend, so the editor never
	// touches a GPU handle; the platform backend (ImGui_ImplSDL3_*) stays
	// with the editor.

	/// Creates the ImGui renderer backend that draws onto the swapchain.
	/// False when it could not be created.
	virtual bool init_ui_backend() = 0;

	virtual void shutdown_ui_backend() = 0;

	/// Per-frame backend state; call before ImGui::NewFrame().
	virtual void ui_backend_new_frame() = 0;

	/// Records \p draw_data over the swapchain image of the frame in flight,
	/// keeping what is already there. Does nothing for a frame without one.
	virtual void render_ui(ImDrawData* draw_data) = 0;

	/// CPU copy of the previous frame's depth pyramid. Never null; empty when
	/// the fence has not landed yet, in which case occlusion is skipped.
	virtual const DepthPyramid& occlusion_pyramid() const = 0;

	/// False when compute shaders or R32 storage are unavailable. Culling then
	/// falls back to frustum and LOD only, with no errors.
	virtual bool occlusion_enabled() const = 0;

	/// True once a downloaded pyramid has landed. False means this frame skips
	/// occlusion because the previous frame's fence is still outstanding.
	virtual bool occlusion_ready() const = 0;

	/// Aspect (width / height) of the depth image currently in
	/// occlusion_pyramid(). Zero when no image has been adopted.
	virtual float occlusion_aspect() const = 0;

	/// The next end_scene_pass closes the pass without downloading a pyramid.
	/// Used when the pass only clears the window behind the editor.
	virtual void suppress_occlusion_record() = 0;

	virtual uint32_t vertex_capacity() const = 0;

	/// True when the viewport texture is ready to be sampled by ImGui.
	virtual bool viewport_ready() const = 0;

	/// The viewport texture as an ImGui texture id (ImGui::Image). Valid only
	/// when viewport_ready() is true.
	virtual uint64_t viewport_ui_texture() const = 0;

	/// Width of the viewport texture in pixels.
	virtual uint32_t viewport_width() const = 0;

	/// Height of the viewport texture in pixels.
	virtual uint32_t viewport_height() const = 0;

	/// Starts a render pass into the viewport texture. Call resize_viewport
	/// first if the size changed. Returns false when the texture is not ready.
	virtual bool begin_viewport_pass() = 0;

	/// Ends the viewport render pass.
	virtual void end_viewport_pass() = 0;

	/// Resizes the viewport texture and its depth buffer. Returns false on
	/// failure, leaving the old texture intact.
	virtual bool resize_viewport(uint32_t width, uint32_t height) = 0;

	/// Frames actually submitted (excludes skipped/stalled frames).
	virtual uint64_t frames_rendered() const = 0;

	virtual LightingSettings& lighting() = 0;

	virtual const LightingSettings& lighting() const = 0;

	/// Analytic blob casters for this frame (xyz center in world space,
	/// w radius). Copies up to 4; extra entries are ignored. Entry points
	/// keep their signatures: scenes that never call this render with no
	/// blobs, exactly the legacy path.
	virtual void set_blob_casters(const glm::vec4* spheres, uint32_t count) = 0;

	virtual void clear_blob_casters() = 0;

	virtual uint32_t blob_count() const = 0;

	virtual const glm::vec4* blob_spheres() const = 0;

};

/// The renderer this build links: the Vulkan backend. Never null.
std::unique_ptr<GpuRenderer> create_gpu_renderer();


} // namespace lotus

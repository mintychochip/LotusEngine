#pragma once

#include <string>
#include <vector>

#include "lotus/ecs.hpp"
#include "lotus/gpu/draw_batch.hpp"
#include "lotus/gpu/vertex.hpp"
#include "lotus/scene/components.hpp"

namespace lotus {

class AssetRegistry;
class GpuRenderer;
class DepthPyramid;
class PhysicsWorld;

/// A world: owns a registry and the systems that run on it.
class Scene {
public:
	explicit Scene(std::string name);

	Scene(const Scene&)            = delete;
	Scene& operator=(const Scene&) = delete;
	Scene(Scene&&)                 = default;
	Scene& operator=(Scene&&)      = default;
	~Scene()                       = default;

	const std::string& name() const { return name_; }

	Registry& registry() { return registry_; }
	const Registry& registry() const { return registry_; }

	/// Creates an entity with Transform + Hierarchy + Name.
	Entity create(std::string name = {});
	/// Destroys the entity and all of its descendants.
	void destroy(Entity entity);
	void clear();

	/// Reparents \p child under \p parent (NullEntity detaches to root).
	void set_parent(Entity child, Entity parent);
	void detach(Entity child);

	/// First entity carrying a Camera2D component, or nullptr if none.
	Camera2D* active_camera();
	const Camera2D* active_camera() const;

	/// First entity carrying a Camera3D component, or nullptr if none.
	Camera3D* active_camera3d();
	const Camera3D* active_camera3d() const;

	/// Root entities (Hierarchy::parent == NullEntity).
	std::vector<Entity> roots() const;

	/// First entity whose Name matches, or NullEntity.
	Entity find(const std::string& name) const;

	/// Nearest entity whose world bounds the ray through \p ndc_x, \p ndc_y
	/// hits, or NullEntity. Coordinates are normalised device space: x and y
	/// in [-1, 1] with y up, matching the zero-to-one depth range the renderer
	/// uses. \p aspect is the viewport aspect ratio the frame was drawn with,
	/// which is what the editor knows and the scene does not.
	enum class PickMode {
		/// Mesh/sprite bounds and optional physics raycast; closest hit wins.
		Combined,
		/// Bounding spheres only (ignores \p physics).
		BoundsOnly,
		/// Physics raycast only (requires \p physics).
		PhysicsOnly,
	};

	/// Picks an entity along the camera ray through \p ndc_x, \p ndc_y.
	Entity pick(float ndc_x, float ndc_y, float aspect,
	            const PhysicsWorld* physics = nullptr,
	            PickMode mode = PickMode::Combined) const;

	/// Persistent identity of \p entity, or a zero GUID when it has none.
	SceneGuid guid_of(Entity entity) const;

	/// First entity whose GUID matches \p guid, or NullEntity.
	Entity find_by_guid(SceneGuid guid) const;

	/// Assigns \p entity a fresh GUID. Called by create(); used on load to give
	/// a document that predates identity a stable one.
	SceneGuid assign_guid(Entity entity);

	/// Resolves every EntityRef to a handle, regenerating GUIDs for any entity
	/// that lacks one first. Unresolved references are appended to the scene's
	/// reference error list. Safe to call repeatedly.
	void resolve_refs();

	/// Destroys every entity whose SceneOrigin matches \p origin. Returns the
	/// number destroyed. This is the unload half of streaming: merge a region,
	/// then drop it by origin when it goes out of range.
	std::size_t destroy_by_origin(const std::string& origin);

	/// Tags every entity that has no origin yet with \p origin, so a loaded or
	/// merged document can be unloaded together later.
	void tag_origin(const std::string& origin);

	/// References that failed to resolve on the last resolve_refs(), one line
	/// each. Separate from last_mesh_error(): mesh and reference failures are
	/// independent and both need reporting.
	const std::vector<std::string>& reference_errors() const
	{
		return reference_errors_;
	}

	/// Advances systems (world-transform propagation, mesh resolution).
	void update(float dt);

	/// Resolves the camera, culls against the view frustum and \p occlusion,
	/// picks LOD levels, and returns this frame's vertices. Pure CPU work: no
	/// GPU needed, which is what makes culling and LOD testable. Passing a null
	/// or empty pyramid disables occlusion culling.
	std::vector<GpuVertex> build_vertices(float aspect,
	                                      const DepthPyramid* occlusion = nullptr);

	/// Builds this frame's vertex data and uploads it. OUTSIDE a render pass.
	void upload(GpuRenderer& gpu, float aspect);

	/// Records the draw for the uploaded vertices. INSIDE a render pass.
	void draw(GpuRenderer& gpu);

	/// Analytic blob spheres for this frame (xyz world center from the
	/// entity Transform, w radius from BlobCaster), up to 4. Pass to
	/// GpuRenderer::set_blob_casters; empty when no entity carries one.
	std::vector<glm::vec4> blob_spheres() const;
	uint32_t vertex_count() const { return vertex_count_; }
	uint32_t draw_calls() const { return draw_call_count_; }

	/// Entities submitted last frame.
	uint32_t drawn_entities() const { return drawn_entities_; }
	/// Entities rejected last frame by frustum, LOD distance, or occlusion.
	/// Superset of occluded_entities(): every occlusion rejection is also a cull.
	uint32_t culled_entities() const { return culled_entities_; }

	/// Entities rejected last frame by occlusion, after passing frustum and LOD.
	uint32_t occluded_entities() const { return occluded_entities_; }

	/// Last mesh-resolution error (empty when the last resolve succeeded).
	const std::string& last_mesh_error() const { return last_mesh_error_; }

	/// Per-scene environment. has_settings() is false for a scene that never
	/// specified one, which means "leave the renderer's globals alone".
	const SceneSettings& settings() const { return settings_; }
	SceneSettings& settings()
	{
		has_settings_ = true;
		return settings_;
	}
	bool has_settings() const { return has_settings_; }
	/// Replaces the settings block wholesale, e.g. when loading a document.
	void set_settings(const SceneSettings& settings)
	{
		settings_     = settings;
		has_settings_ = true;
	}

	/// Optional asset key to path table. Null means resolve MeshSource and
	/// Script paths directly (the default for unit tests). When
	/// \p assets_directory is set, embedded glTF textures are written there as
	/// PNG sidecars during mesh sync.
	void set_assets(AssetRegistry* assets,
	                const std::string& assets_directory = {})
	{
		assets_            = assets;
		assets_directory_ = assets_directory;
	}
	const AssetRegistry* assets() const { return assets_; }

private:
	std::string resolve_mesh_path(const MeshSource& source) const;

	void sync_meshes();
	void sync_lod();

	std::string name_;
	Registry registry_;
	uint32_t vertex_count_{0};
	uint32_t draw_call_count_{0};
	std::vector<GpuDrawBatch> draw_batches_;
	std::vector<glm::vec2> frame_uvs_;
	uint32_t drawn_entities_{0};
	uint32_t culled_entities_{0};
	uint32_t occluded_entities_{0};
	glm::mat4 view_{1.0f};
	glm::mat4 view_projection_{1.0f};
	glm::vec3 camera_position_{0.0f};
	std::string last_mesh_error_;
	// Starts at 1: 0 is the "unassigned" sentinel, so the first GUID handed
	// out must not be it.
	uint64_t next_guid_{1};
	std::vector<std::string> reference_errors_;
	SceneSettings settings_;
	bool has_settings_{false};
	AssetRegistry* assets_{nullptr};
	std::string assets_directory_;
};

/// Recomputes Transform::world for every entity, depth-first from the roots.
void update_world_transforms(Scene& scene);

} // namespace lotus

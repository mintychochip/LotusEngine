#pragma once

#include <string>
#include <vector>

#include "lotus/ecs.hpp"
#include "lotus/gpu/vertex.hpp"
#include "lotus/scene/components.hpp"

namespace lotus {

class GpuRenderer;
class DepthPyramid;

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

	uint32_t vertex_count() const { return vertex_count_; }
	uint32_t draw_calls() const { return vertex_count_ ? 1u : 0u; }

	/// Entities submitted last frame.
	uint32_t drawn_entities() const { return drawn_entities_; }
	/// Entities rejected last frame by frustum or LOD distance.
	uint32_t culled_entities() const { return culled_entities_; }

	/// Entities rejected last frame by occlusion, after passing frustum and LOD.
	uint32_t occluded_entities() const { return occluded_entities_; }

	/// Last mesh-resolution error (empty when the last resolve succeeded).
	const std::string& last_mesh_error() const { return last_mesh_error_; }

private:
	void sync_meshes();
	void sync_lod();

	std::string name_;
	Registry registry_;
	uint32_t vertex_count_{0};
	uint32_t drawn_entities_{0};
	uint32_t culled_entities_{0};
	uint32_t occluded_entities_{0};
	glm::mat4 view_{1.0f};
	glm::mat4 view_projection_{1.0f};
	glm::vec3 camera_position_{0.0f};
	std::string last_mesh_error_;
};

/// Recomputes Transform::world for every entity, depth-first from the roots.
void update_world_transforms(Scene& scene);

} // namespace lotus

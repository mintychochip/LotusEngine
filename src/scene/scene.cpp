#include "lotus/scene/scene.hpp"

#include <algorithm>
#include <cstdint>

#include <glm/gtc/type_ptr.hpp>

#include "lotus/gpu/renderer.hpp"
#include "lotus/scene/culling.hpp"
#include "lotus/scene/lod.hpp"
#include "lotus/scene/depth_pyramid.hpp"
#include "lotus/scene/mesh.hpp"

namespace lotus {
namespace {

void propagate(Registry& registry, Entity entity, const glm::mat4& parent_world)
{
	auto& transform = registry.get<Transform>(entity);
	transform.world = parent_world * transform.local_matrix();

	const auto& hierarchy = registry.get<Hierarchy>(entity);
	for (Entity child = hierarchy.first_child; child != NullEntity;
	     child         = registry.get<Hierarchy>(child).next_sibling) {
		propagate(registry, child, transform.world);
	}
}

/// Transforms a local-space bounding sphere into world space.
BoundingSphere world_bounds(const Transform& transform,
                            const BoundingSphere& local)
{
	BoundingSphere world;
	world.center = glm::vec3{transform.world * glm::vec4{local.center, 1.0f}};
	// Column lengths of the upper 3x3 are the axis scale factors.
	const float scale_x = glm::length(glm::vec3{transform.world[0]});
	const float scale_y = glm::length(glm::vec3{transform.world[1]});
	const float scale_z = glm::length(glm::vec3{transform.world[2]});
	world.radius = local.radius * std::max({scale_x, scale_y, scale_z});
	return world;
}

/// Emits world-space lit vertices for an indexed mesh.
void emit_mesh(std::vector<GpuVertex>& out, const Transform& transform,
               const Mesh& mesh, const glm::vec3& color)
{
	// Correct for rotation and uniform scale; non-uniform scale would need the
	// inverse transpose.
	const glm::mat3 normal_matrix{transform.world};
	for (const std::uint32_t index : mesh.indices) {
		if (index >= mesh.positions.size()) {
			continue;
		}
		const glm::vec3 position =
		        glm::vec3{transform.world * glm::vec4{mesh.positions[index], 1.0f}};
		glm::vec3 normal{0.0f, 0.0f, 1.0f};
		if (index < mesh.normals.size()) {
			const glm::vec3 transformed = normal_matrix * mesh.normals[index];
			if (glm::length(transformed) > 0.0f) {
				normal = glm::normalize(transformed);
			}
		}
		out.push_back(GpuVertex{position, normal, color});
	}
}

glm::vec3 material_color(const Registry& registry, Entity entity)
{
	if (const Material* material = registry.try_get<Material>(entity)) {
		return material->base_color;
	}
	return glm::vec3{1.0f};
}

/// True when \p pyramid reports this sphere as hidden. A null pyramid means
/// occlusion is disabled, which must never cull anything.
bool hidden(const DepthPyramid* pyramid, const glm::mat4& view,
            const glm::mat4& view_projection, const BoundingSphere& world)
{
	return pyramid != nullptr && pyramid->occluded(view, view_projection, world);
}

} // namespace

Scene::Scene(std::string name) : name_{std::move(name)} {}

Entity Scene::create(std::string name)
{
	const Entity entity = registry_.create();
	registry_.emplace<Transform>(entity);
	registry_.emplace<Hierarchy>(entity);
	registry_.emplace<Name>(entity, Name{std::move(name)});
	return entity;
}

void Scene::detach(Entity child)
{
	if (child == NullEntity || !registry_.valid(child) ||
	    !registry_.all_of<Hierarchy>(child)) {
		return;
	}
	auto& hierarchy = registry_.get<Hierarchy>(child);

	if (hierarchy.prev_sibling != NullEntity) {
		registry_.get<Hierarchy>(hierarchy.prev_sibling).next_sibling =
		        hierarchy.next_sibling;
	} else if (hierarchy.parent != NullEntity) {
		registry_.get<Hierarchy>(hierarchy.parent).first_child =
		        hierarchy.next_sibling;
	}
	if (hierarchy.next_sibling != NullEntity) {
		registry_.get<Hierarchy>(hierarchy.next_sibling).prev_sibling =
		        hierarchy.prev_sibling;
	}
	hierarchy.parent       = NullEntity;
	hierarchy.prev_sibling = NullEntity;
	hierarchy.next_sibling = NullEntity;
}

void Scene::set_parent(Entity child, Entity parent)
{
	if (child == NullEntity || child == parent || !registry_.valid(child)) {
		return;
	}
	if (parent != NullEntity && !registry_.valid(parent)) {
		return;
	}
	// Reject cycles: walk up from parent looking for child.
	for (Entity walk = parent; walk != NullEntity;) {
		if (walk == child) {
			return;
		}
		walk = registry_.get<Hierarchy>(walk).parent;
	}

	detach(child);
	auto& hierarchy  = registry_.get<Hierarchy>(child);
	hierarchy.parent = parent;
	if (parent == NullEntity) {
		return;
	}

	auto& parent_hierarchy = registry_.get<Hierarchy>(parent);
	hierarchy.next_sibling = parent_hierarchy.first_child;
	hierarchy.prev_sibling = NullEntity;
	if (parent_hierarchy.first_child != NullEntity) {
		registry_.get<Hierarchy>(parent_hierarchy.first_child).prev_sibling = child;
	}
	parent_hierarchy.first_child = child;
}

void Scene::destroy(Entity entity)
{
	if (!registry_.valid(entity)) {
		return;
	}
	Entity child = registry_.get<Hierarchy>(entity).first_child;
	while (child != NullEntity) {
		const Entity next = registry_.get<Hierarchy>(child).next_sibling;
		destroy(child);
		child = next;
	}
	detach(entity);
	registry_.destroy(entity);
}

void Scene::clear()
{
	registry_.clear();
	vertex_count_     = 0;
	drawn_entities_   = 0;
	culled_entities_  = 0;
	// Without this the scene reports the previous frame's occlusion count
	// until the next build_vertices(), which for a just-cleared scene is the
	// whole lifetime of the next read.
	occluded_entities_ = 0;
}

Camera2D* Scene::active_camera()
{
	auto view = registry_.view<Camera2D>();
	for (const Entity entity : view) {
		return &view.get<Camera2D>(entity);
	}
	return nullptr;
}

const Camera2D* Scene::active_camera() const
{
	const auto view = registry_.view<Camera2D>();
	for (const Entity entity : view) {
		return &view.get<Camera2D>(entity);
	}
	return nullptr;
}

Camera3D* Scene::active_camera3d()
{
	auto view = registry_.view<Camera3D>();
	for (const Entity entity : view) {
		return &view.get<Camera3D>(entity);
	}
	return nullptr;
}

const Camera3D* Scene::active_camera3d() const
{
	const auto view = registry_.view<Camera3D>();
	for (const Entity entity : view) {
		return &view.get<Camera3D>(entity);
	}
	return nullptr;
}

std::vector<Entity> Scene::roots() const
{
	std::vector<Entity> out;
	const auto view = registry_.view<Hierarchy>();
	for (const Entity entity : view) {
		if (view.get<Hierarchy>(entity).parent == NullEntity) {
			out.push_back(entity);
		}
	}
	return out;
}

void Scene::update(float /*dt*/)
{
	update_world_transforms(*this);
	sync_meshes();
	sync_lod();
}

Entity Scene::find(const std::string& name) const
{
	const auto view = registry_.view<Name>();
	for (const Entity entity : view) {
		if (view.get<Name>(entity).value == name) {
			return entity;
		}
	}
	return NullEntity;
}

void Scene::sync_meshes()
{
	for (const Entity entity : registry_.view<MeshSource>()) {
		// Entities using Lod get their geometry from sync_lod instead.
		if (registry_.all_of<Lod>(entity)) {
			continue;
		}
		const std::string& path = registry_.get<MeshSource>(entity).path;
		Mesh* existing          = registry_.try_get<Mesh>(entity);
		if (existing && existing->source == path) {
			continue;
		}
		Mesh resolved;
		std::string error;
		if (!mesh::resolve_mesh(path, resolved, &error)) {
			last_mesh_error_ = error;
			continue;
		}
		resolved.source = path;
		resolved.bounds = mesh::compute_mesh_bounds(resolved);
		registry_.emplace_or_replace<Mesh>(entity, std::move(resolved));
		last_mesh_error_.clear();
	}
}

void Scene::sync_lod()
{
	for (const Entity entity : registry_.view<Lod>()) {
		const Lod& lod = registry_.get<Lod>(entity);
		LodGeometry* geometry = registry_.try_get<LodGeometry>(entity);
		if (geometry && geometry->levels.size() == lod.levels.size()) {
			bool up_to_date = true;
			for (std::size_t i = 0; i < lod.levels.size(); ++i) {
				if (geometry->levels[i].source != lod.levels[i].source) {
					up_to_date = false;
					break;
				}
			}
			if (up_to_date) {
				continue;
			}
		}

		LodGeometry resolved;
		resolved.levels.reserve(lod.levels.size());
		for (const LodLevel& level : lod.levels) {
			Mesh mesh;
			std::string error;
			if (!mesh::resolve_mesh(level.source, mesh, &error)) {
				last_mesh_error_ = error;
				continue;
			}
			mesh.source = level.source;
			mesh.bounds = mesh::compute_mesh_bounds(mesh);
			resolved.levels.push_back(std::move(mesh));
		}
		registry_.emplace_or_replace<LodGeometry>(entity, std::move(resolved));
	}
}

std::vector<GpuVertex> Scene::build_vertices(float aspect,
                                              const DepthPyramid* occlusion)
{
	// Resolve the camera once, then reuse it for culling and drawing.
	if (const Camera3D* camera = active_camera3d()) {
		view_            = camera->view();
		view_projection_ = camera->view_projection(aspect);
		camera_position_ = camera->position;
	} else {
		static const Camera2D fallback{};
		const Camera2D* camera2d = active_camera();
		const Camera2D& chosen   = camera2d ? *camera2d : fallback;
		view_                   = chosen.view();
		view_projection_        = chosen.view_projection(aspect);
		camera_position_        = glm::vec3{chosen.position, 1.0f};
	}
	const Frustum frustum = Frustum::from_view_projection(view_projection_);

	std::vector<GpuVertex> vertices;
	drawn_entities_    = 0;
	culled_entities_   = 0;
	occluded_entities_ = 0;

	// 2D sprites: axis-aligned quads facing +Z.
	const auto sprite_view = registry_.view<Transform, Sprite>();
	sprite_view.each([&](const Transform& transform, const Sprite& sprite) {
		const BoundingSphere local{glm::vec3{0.0f}, glm::length(sprite.size) * 0.5f};
		const BoundingSphere world = world_bounds(transform, local);
		if (!frustum.intersects(world)) {
			++culled_entities_;
			return;
		}
		if (hidden(occlusion, view_, view_projection_, world)) {
			++occluded_entities_;
			++culled_entities_;
			return;
		}
		++drawn_entities_;

		const float hx = sprite.size.x * 0.5f;
		const float hy = sprite.size.y * 0.5f;
		const glm::vec4 corners[4] = {
		        {-hx,  hy, 0.0f, 1.0f},
		        { hx,  hy, 0.0f, 1.0f},
		        { hx, -hy, 0.0f, 1.0f},
		        {-hx, -hy, 0.0f, 1.0f},
		};
		glm::vec3 world_vertices[4];
		for (int i = 0; i < 4; ++i) {
			world_vertices[i] = glm::vec3{transform.world * corners[i]};
		}
		const glm::vec3 color{sprite.color};
		static constexpr int order[6] = {0, 1, 2, 0, 2, 3};
		for (const int index : order) {
			vertices.push_back(GpuVertex{world_vertices[index],
			                             glm::vec3{0.0f, 0.0f, 1.0f}, color});
		}
	});

	// 3D meshes (entities without Lod).
	const auto mesh_view = registry_.view<Transform, Mesh>();
	mesh_view.each([&](const Entity entity, const Transform& transform,
	                   const Mesh& mesh) {
		if (mesh.empty() || registry_.all_of<Lod>(entity)) {
			return;
		}
		const BoundingSphere world = world_bounds(transform, mesh.bounds);
		if (!frustum.intersects(world)) {
			++culled_entities_;
			return;
		}
		if (hidden(occlusion, view_, view_projection_, world)) {
			++occluded_entities_;
			++culled_entities_;
			return;
		}
		++drawn_entities_;
		emit_mesh(vertices, transform, mesh, material_color(registry_, entity));
	});

	// Lod entities: pick a level by distance, then cull that level's bounds.
	const auto lod_view = registry_.view<Transform, Lod>();
	lod_view.each([&](const Entity entity, const Transform& transform,
	                  const Lod& lod) {
		const LodGeometry* geometry = registry_.try_get<LodGeometry>(entity);
		if (!geometry || geometry->levels.empty()) {
			return;
		}
		const glm::vec3 world_position = glm::vec3{transform.world[3]};
		const float distance = glm::length(world_position - camera_position_);

		std::vector<float> max_distances;
		max_distances.reserve(lod.levels.size());
		for (const LodLevel& level : lod.levels) {
			max_distances.push_back(level.max_distance);
		}
		const int level = select_lod_level(max_distances, distance);
		if (level < 0 || static_cast<std::size_t>(level) >= geometry->levels.size()) {
			++culled_entities_; // past the last LOD
			return;
		}
		const Mesh& mesh = geometry->levels[static_cast<std::size_t>(level)];
		const BoundingSphere world = world_bounds(transform, mesh.bounds);
		if (!frustum.intersects(world)) {
			++culled_entities_;
			return;
		}
		if (hidden(occlusion, view_, view_projection_, world)) {
			++occluded_entities_;
			++culled_entities_;
			return;
		}
		++drawn_entities_;
		emit_mesh(vertices, transform, mesh, material_color(registry_, entity));
	});

	return vertices;
}

void Scene::upload(GpuRenderer& gpu, float aspect)
{
	const std::vector<GpuVertex> vertices =
	        build_vertices(aspect, &gpu.occlusion_pyramid());
	vertex_count_ = static_cast<std::uint32_t>(vertices.size());
	if (vertex_count_ != 0) {
		gpu.upload_vertices(vertices.data(), vertex_count_);
	}
}

void Scene::draw(GpuRenderer& gpu)
{
	if (vertex_count_ == 0) {
		return;
	}
	gpu.draw(view_, view_projection_, camera_position_, vertex_count_);
}

void update_world_transforms(Scene& scene)
{
	auto& registry = scene.registry();
	for (const Entity root : scene.roots()) {
		propagate(registry, root, glm::mat4{1.0f});
	}
}

} // namespace lotus

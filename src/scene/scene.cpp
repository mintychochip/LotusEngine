#include "lotus/scene/scene.hpp"

#include <cstdint>

#include <glm/gtc/type_ptr.hpp>

#include "lotus/gpu/renderer.hpp"
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
	vertex_count_ = 0;
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
}

void Scene::sync_meshes()
{
	for (const Entity entity : registry_.view<MeshSource>()) {
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
		registry_.emplace_or_replace<Mesh>(entity, std::move(resolved));
		last_mesh_error_.clear();
	}
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

void Scene::upload(GpuRenderer& gpu)
{
	std::vector<GpuVertex> vertices;

	// 2D sprites: axis-aligned quads facing +Z, lit like everything else.
	const auto sprite_view = registry_.view<Transform, Sprite>();
	sprite_view.each([&](const Transform& transform, const Sprite& sprite) {
		const float hx = sprite.size.x * 0.5f;
		const float hy = sprite.size.y * 0.5f;
		const glm::vec4 corners[4] = {
		        {-hx,  hy, 0.0f, 1.0f},
		        { hx,  hy, 0.0f, 1.0f},
		        { hx, -hy, 0.0f, 1.0f},
		        {-hx, -hy, 0.0f, 1.0f},
		};
		glm::vec3 world[4];
		for (int i = 0; i < 4; ++i) {
			world[i] = glm::vec3{transform.world * corners[i]};
		}
		const glm::vec3 color{sprite.color};
		static constexpr int order[6] = {0, 1, 2, 0, 2, 3};
		for (const int index : order) {
			vertices.push_back(
			        GpuVertex{world[index], glm::vec3{0.0f, 0.0f, 1.0f}, color});
		}
	});

	// 3D meshes: indices are expanded here because the renderer draws
	// non-indexed, and positions/normals are transformed to world space on the
	// CPU so the whole scene stays in one draw call.
	const auto mesh_view = registry_.view<Transform, Mesh>();
	mesh_view.each([&](const Entity entity, const Transform& transform,
	                   const Mesh& mesh) {
		if (mesh.empty()) {
			return;
		}
		glm::vec3 color{1.0f};
		if (const Material* material = registry_.try_get<Material>(entity)) {
			color = material->base_color;
		}
		// Correct for rotation and uniform scale; non-uniform scale would need
		// the inverse transpose.
		const glm::mat3 normal_matrix = glm::mat3{transform.world};

		for (const std::uint32_t index : mesh.indices) {
			if (index >= mesh.positions.size()) {
				continue;
			}
			const glm::vec3 position = glm::vec3{
			        transform.world * glm::vec4{mesh.positions[index], 1.0f}};
			glm::vec3 normal{0.0f, 0.0f, 1.0f};
			if (index < mesh.normals.size()) {
				const glm::vec3 transformed = normal_matrix * mesh.normals[index];
				if (glm::length(transformed) > 0.0f) {
					normal = glm::normalize(transformed);
				}
			}
			vertices.push_back(GpuVertex{position, normal, color});
		}
	});

	vertex_count_ = static_cast<std::uint32_t>(vertices.size());
	if (vertex_count_ != 0) {
		gpu.upload_vertices(vertices.data(), vertex_count_);
	}
}

void Scene::draw(GpuRenderer& gpu, float aspect)
{
	if (vertex_count_ == 0) {
		return;
	}
	glm::mat4 view_projection{1.0f};
	glm::vec3 camera_position{0.0f};

	if (const Camera3D* camera = active_camera3d()) {
		view_projection = camera->view_projection(aspect);
		camera_position = camera->position;
	} else {
		static const Camera2D fallback{};
		const Camera2D* camera2d = active_camera();
		const Camera2D& chosen   = camera2d ? *camera2d : fallback;
		view_projection = chosen.view_projection(aspect);
		camera_position = glm::vec3{chosen.position, 1.0f};
	}
	gpu.draw(view_projection, camera_position, vertex_count_);
}

void update_world_transforms(Scene& scene)
{
	auto& registry = scene.registry();
	for (const Entity root : scene.roots()) {
		propagate(registry, root, glm::mat4{1.0f});
	}
}

} // namespace lotus

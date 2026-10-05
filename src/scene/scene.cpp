#include "lotus/scene/scene.hpp"

#include <glm/gtc/type_ptr.hpp>

#include "lotus/gpu/renderer.hpp"

namespace lotus {
namespace {

struct GpuVertex {
	float x, y, z;
	float r, g, b;
};

void propagate(Registry& registry, Entity entity, const glm::mat4& parent_world)
{
	auto& transform   = registry.get<Transform>(entity);
	transform.world   = parent_world * transform.local_matrix();

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
	auto& hierarchy = registry_.get<Hierarchy>(child);
	hierarchy.parent = parent;
	if (parent == NullEntity) {
		return;
	}

	auto& parent_hierarchy   = registry_.get<Hierarchy>(parent);
	hierarchy.next_sibling   = parent_hierarchy.first_child;
	hierarchy.prev_sibling   = NullEntity;
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
}

void Scene::upload(GpuRenderer& gpu)
{
	std::vector<GpuVertex> vertices;
	const auto view = registry_.view<Transform, Sprite>();
	view.each([&](const Transform& transform, const Sprite& sprite) {
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
		static constexpr int order[6] = {0, 1, 2, 0, 2, 3};
		for (const int index : order) {
			vertices.push_back(GpuVertex{world[index].x,
			                             world[index].y,
			                             world[index].z,
			                             sprite.color.r,
			                             sprite.color.g,
			                             sprite.color.b});
		}
	});

	vertex_count_ = static_cast<uint32_t>(vertices.size());
	if (vertex_count_ != 0) {
		gpu.upload_vertices(vertices.data(),
		                    static_cast<uint32_t>(vertices.size() *
		                                          sizeof(GpuVertex)));
	}
}

void Scene::draw(GpuRenderer& gpu, float aspect)
{
	if (vertex_count_ == 0) {
		return;
	}
	const glm::mat4 mvp = camera_.view_projection(aspect);
	gpu.draw(glm::value_ptr(mvp), vertex_count_);
}

void update_world_transforms(Scene& scene)
{
	auto& registry = scene.registry();
	for (const Entity root : scene.roots()) {
		propagate(registry, root, glm::mat4{1.0f});
	}
}

} // namespace lotus

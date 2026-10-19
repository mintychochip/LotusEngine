#include "lotus/scene/scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include <glm/gtc/type_ptr.hpp>

#include "lotus/gpu/renderer.hpp"
#include "lotus/scene/culling.hpp"
#include "lotus/scene/lod.hpp"
#include "lotus/scene/depth_pyramid.hpp"
#include <filesystem>

#include "lotus/scene/assets.hpp"
#include "lotus/physics/physics_world.hpp"
#include "lotus/scene/glb.hpp"
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
	// Column lengths of the upper 3x3 are the axis scale factors. The
	// largest is the square root of the largest squared length: the root is
	// monotonic and correctly rounded, so this is bit for bit the largest of
	// the three roots, for the price of one.
	const auto length_squared = [](const glm::vec4& column) {
		const glm::vec3 axis{column};
		return glm::dot(axis, axis);
	};
	const float largest_squared =
	        std::max({length_squared(transform.world[0]),
	                  length_squared(transform.world[1]),
	                  length_squared(transform.world[2])});
	world.radius = local.radius * std::sqrt(largest_squared);
	return world;
}

/// Ray/sphere overlap for picking. \p t receives the distance along the ray
/// when it hits. Hits behind the origin count only when the origin is inside
/// the sphere, which keeps a camera parked inside a mesh pickable.
bool ray_hits_sphere(const glm::vec3& origin, const glm::vec3& direction,
                     const BoundingSphere& sphere, float& t)
{
	const glm::vec3 offset = origin - sphere.center;
	const float along      = glm::dot(offset, direction);
	const float gap        = glm::dot(offset, offset) - sphere.radius * sphere.radius;
	if (gap > 0.0f && along > 0.0f) {
		return false;
	}
	const float discriminant = along * along - gap;
	if (discriminant < 0.0f) {
		return false;
	}
	t = std::max(-along - std::sqrt(discriminant), 0.0f);
	return true;
}

/// Number of indices that address an existing vertex: exactly what
/// emit_mesh() writes. A well-formed mesh has them all.
std::uint32_t count_valid_indices(const Mesh& mesh)
{
	const std::size_t vertex_count = mesh.positions.size();
	std::uint32_t valid            = 0;
	for (const std::uint32_t index : mesh.indices) {
		valid += index < vertex_count ? 1u : 0u;
	}
	return valid;
}

/// Writes the world-space lit vertices of an indexed mesh to \p out and
/// \p uvs_out: count_valid_indices(mesh) entries each.
///
/// Each distinct vertex is transformed once and then gathered through the
/// index list: the lists repeat vertices heavily (every vertex of a UV sphere
/// is referenced about five times), and the transform (matrix product, normal
/// matrix, normalize) costs far more than the copy. Each vertex's value is a
/// pure function of (world, position, normal), so the output is bit-identical
/// to transforming per index.
void emit_mesh(GpuVertex* out, glm::vec2* uvs_out, const Transform& transform,
               const Mesh& mesh, const glm::vec3& color, float roughness)
{
	// Correct for rotation and uniform scale; non-uniform scale would need the
	// inverse transpose.
	const glm::mat3 normal_matrix{transform.world};
	const std::size_t vertex_count = mesh.positions.size();
	const bool has_uvs             = mesh.uvs.size() == vertex_count;

	thread_local std::vector<GpuVertex> transformed;
	transformed.resize(vertex_count);
	for (std::size_t i = 0; i < vertex_count; ++i) {
		GpuVertex& vertex = transformed[i];
		vertex            = GpuVertex{};
		vertex.position =
		        glm::vec3{transform.world * glm::vec4{mesh.positions[i], 1.0f}};
		glm::vec3 normal{0.0f, 0.0f, 1.0f};
		if (i < mesh.normals.size()) {
			const glm::vec3 rotated = normal_matrix * mesh.normals[i];
			if (glm::length(rotated) > 0.0f) {
				normal = glm::normalize(rotated);
			}
		}
		vertex.normal    = normal;
		vertex.color     = color;
		vertex.roughness = roughness;
	}

	for (const std::uint32_t index : mesh.indices) {
		if (index >= vertex_count) {
			continue;
		}
		*out++     = transformed[index];
		*uvs_out++ = has_uvs ? mesh.uvs[index] : glm::vec2{0.0f, 0.0f};
	}
}

/// Writes the six vertices of a 2D sprite: an axis-aligned quad facing +Z.
void emit_sprite(GpuVertex* out, glm::vec2* uvs_out, const Transform& transform,
                 const Sprite& sprite)
{
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
		GpuVertex vertex{};
		vertex.position = world_vertices[index];
		vertex.normal   = glm::vec3{0.0f, 0.0f, 1.0f};
		vertex.color    = color;
		*out++          = vertex;
		*uvs_out++      = glm::vec2{0.0f};
	}
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
	assign_guid(entity);
	return entity;
}

SceneGuid Scene::guid_of(Entity entity) const
{
	if (entity == NullEntity || !registry_.valid(entity)) {
		return SceneGuid{};
	}
	const auto* guid = registry_.try_get<SceneGuid>(entity);
	return guid ? *guid : SceneGuid{};
}

Entity Scene::find_by_guid(SceneGuid guid) const
{
	if (guid.value == 0) {
		return NullEntity;
	}
	const auto view = registry_.view<SceneGuid>();
	for (const Entity entity : view) {
		if (view.get<SceneGuid>(entity).value == guid.value) {
			return entity;
		}
	}
	return NullEntity;
}

SceneGuid Scene::assign_guid(Entity entity)
{
	if (entity == NullEntity || !registry_.valid(entity)) {
		return SceneGuid{};
	}
	const SceneGuid guid{next_guid_++};
	registry_.emplace_or_replace<SceneGuid>(entity, guid);
	return guid;
}

void Scene::resolve_refs()
{
	reference_errors_.clear();
	// A document that predates identity carries no GUIDs; give every entity one
	// before resolving, so references within a freshly loaded scene can match.
	for (const Entity entity : registry_.view<Name>()) {
		if (!registry_.all_of<SceneGuid>(entity) ||
		    registry_.get<SceneGuid>(entity).value == 0) {
			assign_guid(entity);
		}
	}
	// A document can carry GUIDs far above the counter (a hand-authored or
	// merged file). Advance past them, or a later create() collides with an
	// entity that was just loaded.
	for (const Entity entity : registry_.view<SceneGuid>()) {
		const uint64_t value = registry_.get<SceneGuid>(entity).value;
		if (value >= next_guid_) {
			next_guid_ = value + 1;
		}
	}
	for (const Entity entity : registry_.view<EntityRef>()) {
		auto& ref = registry_.get<EntityRef>(entity);
		ref.resolved = find_by_guid(ref.target);
		if (ref.resolved == NullEntity && ref.target.value != 0) {
			// try_get: an entity may carry a reference without a Name.
			const auto* name = registry_.try_get<Name>(entity);
			const std::string label =
			        name ? name->value
			             : ("entity " + std::to_string(
			                        static_cast<uint32_t>(entity)));
			reference_errors_.push_back("unresolved reference from '" +
			                            label + "' to guid " +
			                            std::to_string(ref.target.value));
		}
	}
}

void Scene::tag_origin(const std::string& origin)
{
	// Only untagged entities: a merge into a scene that already holds tagged
	// entities must not relabel the ones that were already there.
	for (const Entity entity : registry_.view<Name>()) {
		if (!registry_.all_of<SceneOrigin>(entity) ||
		    registry_.get<SceneOrigin>(entity).scene.empty()) {
			registry_.emplace_or_replace<SceneOrigin>(entity,
			                                         SceneOrigin{origin});
		}
	}
}

std::size_t Scene::destroy_by_origin(const std::string& origin)
{
	if (origin.empty()) {
		return 0; // an empty origin means "untagged": never bulk-destroy those
	}
	// Collect first: destroying mutates the view.
	std::vector<Entity> doomed;
	for (const Entity entity : registry_.view<SceneOrigin>()) {
		if (registry_.get<SceneOrigin>(entity).scene == origin) {
			doomed.push_back(entity);
		}
	}
	for (const Entity entity : doomed) {
		destroy(entity);
	}
	// References that pointed into the unloaded set are now dangling; report
	// them rather than leaving stale handles behind.
	resolve_refs();
	return doomed.size();
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
	// Identity and reference state are per-scene too: leaving the counter high
	// would let GUIDs climb across a reset, and stale reference errors would
	// describe entities that no longer exist.
	next_guid_ = 1;
	reference_errors_.clear();
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

Entity Scene::pick(float ndc_x, float ndc_y, float aspect,
                   const PhysicsWorld* physics, PickMode mode) const
{
	// The camera is rebuilt here rather than reusing view_projection_: the
	// stored one belongs to whichever pass ran last, and the viewport pass and
	// the window pass disagree on aspect whenever the panel is not the window.
	glm::mat4 view_projection{1.0f};
	if (const Camera3D* camera = active_camera3d()) {
		view_projection = camera->view_projection(aspect);
	} else {
		static const Camera2D fallback{};
		const Camera2D* camera2d = active_camera();
		const Camera2D& chosen   = camera2d ? *camera2d : fallback;
		view_projection          = chosen.view_projection(aspect);
	}

	const glm::mat4 inverse = glm::inverse(view_projection);
	glm::vec4 near_point    = inverse * glm::vec4{ndc_x, ndc_y, 0.0f, 1.0f};
	glm::vec4 far_point     = inverse * glm::vec4{ndc_x, ndc_y, 1.0f, 1.0f};
	if (near_point.w == 0.0f || far_point.w == 0.0f) {
		return NullEntity;
	}
	near_point /= near_point.w;
	far_point /= far_point.w;

	const glm::vec3 origin    = glm::vec3{near_point};
	const glm::vec3 to_far    = glm::vec3{far_point - near_point};
	if (glm::length(to_far) <= 0.0f) {
		return NullEntity;
	}
	const glm::vec3 direction = glm::normalize(to_far);

	Entity best        = NullEntity;
	float best_distance = std::numeric_limits<float>::max();

	const auto consider = [&](Entity entity, const BoundingSphere& world) {
		float distance = 0.0f;
		if (!ray_hits_sphere(origin, direction, world, distance)) {
			return;
		}
		if (distance < best_distance) {
			best_distance = distance;
			best          = entity;
		}
	};

	const bool use_bounds =
	        mode == PickMode::Combined || mode == PickMode::BoundsOnly;
	const bool use_physics =
	        physics != nullptr &&
	        (mode == PickMode::Combined || mode == PickMode::PhysicsOnly);

	if (use_bounds) {
		for (const Entity entity : registry_.view<Transform, Sprite>()) {
			const Sprite& sprite = registry_.get<Sprite>(entity);
			const BoundingSphere local{glm::vec3{0.0f},
			                           glm::length(sprite.size) * 0.5f};
			consider(entity,
			         world_bounds(registry_.get<Transform>(entity), local));
		}
		for (const Entity entity : registry_.view<Transform, Mesh>()) {
			const Mesh& mesh = registry_.get<Mesh>(entity);
			if (mesh.empty() || registry_.all_of<Lod>(entity)) {
				continue;
			}
			consider(entity, world_bounds(registry_.get<Transform>(entity),
			                              mesh.bounds));
		}
		// Lod entities are picked by their most detailed level's bounds: the level
		// actually drawn depends on the camera distance, and for selection the
		// finest bounds are the honest answer.
		for (const Entity entity : registry_.view<Transform, Lod>()) {
			const LodGeometry* geometry = registry_.try_get<LodGeometry>(entity);
			if (geometry == nullptr || geometry->levels.empty()) {
				continue;
			}
			consider(entity, world_bounds(registry_.get<Transform>(entity),
			                              geometry->levels.front().bounds));
		}
	}

	if (use_physics) {
		PhysicsRaycastHit hit;
		const float max_distance = 10000.0f;
		if (physics->raycast(origin, direction, max_distance, hit) &&
		    hit.entity != NullEntity) {
			const float physics_distance = hit.fraction * max_distance;
			if (physics_distance < best_distance) {
				best = hit.entity;
			}
		}
	}
	return best;
}

std::string Scene::resolve_mesh_path(const MeshSource& source) const
{
	if (assets_ == nullptr) {
		return source.path;
	}
	return assets_->resolve(source.id, source.path);
}

bool path_ends_with_glb(const std::string& path)
{
	if (path.size() < 4) {
		return false;
	}
	const std::string tail = path.substr(path.size() - 4);
	return tail == ".glb" || tail == ".GLB";
}

void apply_glb_material(Registry& registry, Entity entity,
                        const std::vector<GlbMaterialSlot>& slots, int index)
{
	if (index < 0 || index >= static_cast<int>(slots.size())) {
		return;
	}
	const GlbMaterialSlot& slot = slots[static_cast<std::size_t>(index)];
	if (registry.all_of<Material>(entity)) {
		Material& material = registry.get<Material>(entity);
		if (material.albedo_id.empty() && !slot.albedo_id.empty()) {
			material.albedo_id = slot.albedo_id;
		}
		material.base_color = slot.base_color;
		return;
	}
	registry.emplace<Material>(entity,
	                           Material{slot.base_color, slot.albedo_id, 0.5f});
}

void Scene::sync_meshes()
{
	for (const Entity entity : registry_.view<MeshSource>()) {
		// Entities using Lod get their geometry from sync_lod instead.
		if (registry_.all_of<Lod>(entity)) {
			continue;
		}
		const MeshSource& mesh_source = registry_.get<MeshSource>(entity);
		const std::string path        = resolve_mesh_path(mesh_source);
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

		if (assets_ != nullptr && path_ends_with_glb(path)) {
			const std::string stem =
			        std::filesystem::path(path).stem().string();
			std::vector<GlbMaterialSlot> slots;
			int material_index = -1;
			std::string import_error;
			if (import_glb_materials(path, stem, assets_directory_, *assets_,
			                         slots, material_index, &import_error)) {
				apply_glb_material(registry_, entity, slots, material_index);
			} else if (!import_error.empty()) {
				last_mesh_error_ = import_error;
			}
		}
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

void Scene::build_frame(float aspect, const DepthPyramid* occlusion)
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

	drawn_entities_    = 0;
	culled_entities_   = 0;
	occluded_entities_ = 0;
	draw_items_.clear();
	std::fill(slot_exemplar_.begin(), slot_exemplar_.end(), nullptr);

	// Frustum, then occlusion; counts the outcome either way.
	const auto visible = [&](const BoundingSphere& world) {
		if (!frustum.intersects(world)) {
			++culled_entities_;
			return false;
		}
		if (hidden(occlusion, view_, view_projection_, world)) {
			++occluded_entities_;
			++culled_entities_;
			return false;
		}
		++drawn_entities_;
		return true;
	};

	// The slot of a mesh that is drawn instanced; 0 when it must be written
	// as world-space vertices. A mesh with a source names immutable geometry
	// (see upload()), unless this frame's first mesh of that source has a
	// different size, which proves the source is being reused.
	const auto instance_slot = [&](const Mesh& mesh) -> uint32_t {
		if (mesh.source.empty()) {
			return 0;
		}
		const uint32_t slot =
		        mesh_slots_
		                .try_emplace(mesh.source,
		                             static_cast<uint32_t>(mesh_slots_.size() + 1))
		                .first->second;
		if (slot_exemplar_.size() <= slot) {
			slot_exemplar_.resize(slot + 1, nullptr);
		}
		const Mesh*& exemplar = slot_exemplar_[slot];
		if (exemplar == nullptr) {
			exemplar = &mesh;
			return slot;
		}
		const bool same = exemplar->positions.size() == mesh.positions.size() &&
		                  exemplar->normals.size() == mesh.normals.size() &&
		                  exemplar->uvs.size() == mesh.uvs.size() &&
		                  exemplar->indices.size() == mesh.indices.size();
		return same ? slot : 0;
	};

	// Albedo key of sprites and of entities without a Material.
	static const std::string no_albedo;
	const auto push_mesh = [&](Entity entity, const Transform& transform,
	                           const Mesh& mesh) {
		const Material* material = registry_.try_get<Material>(entity);
		DrawItem item;
		item.albedo    = material ? &material->albedo_id : &no_albedo;
		item.color     = material ? material->base_color : glm::vec3{1.0f};
		item.roughness = material ? material->roughness : 0.5f;
		item.transform = &transform;
		item.mesh      = &mesh;
		item.slot      = instance_slot(mesh);
		item.order     = static_cast<uint32_t>(draw_items_.size());
		draw_items_.push_back(item);
	};

	// 2D sprites: axis-aligned quads facing +Z.
	const auto sprite_view = registry_.view<Transform, Sprite>();
	sprite_view.each([&](const Transform& transform, const Sprite& sprite) {
		const BoundingSphere local{glm::vec3{0.0f}, glm::length(sprite.size) * 0.5f};
		if (!visible(world_bounds(transform, local))) {
			return;
		}
		DrawItem item;
		item.albedo    = &no_albedo;
		item.transform = &transform;
		item.sprite    = &sprite;
		item.order     = static_cast<uint32_t>(draw_items_.size());
		draw_items_.push_back(item);
	});

	// 3D meshes (entities without Lod).
	const auto mesh_view = registry_.view<Transform, Mesh>();
	mesh_view.each([&](const Entity entity, const Transform& transform,
	                   const Mesh& mesh) {
		if (mesh.empty() || registry_.all_of<Lod>(entity)) {
			return;
		}
		if (!visible(world_bounds(transform, mesh.bounds))) {
			return;
		}
		push_mesh(entity, transform, mesh);
	});

	// Lod entities: pick a level by distance, then cull that level's bounds.
	thread_local std::vector<float> max_distances;
	const auto lod_view = registry_.view<Transform, Lod>();
	lod_view.each([&](const Entity entity, const Transform& transform,
	                  const Lod& lod) {
		const LodGeometry* geometry = registry_.try_get<LodGeometry>(entity);
		if (!geometry || geometry->levels.empty()) {
			return;
		}
		const glm::vec3 world_position = glm::vec3{transform.world[3]};
		const float distance = glm::length(world_position - camera_position_);

		max_distances.clear();
		for (const LodLevel& level : lod.levels) {
			max_distances.push_back(level.max_distance);
		}
		const int level = select_lod_level(max_distances, distance);
		if (level < 0 || static_cast<std::size_t>(level) >= geometry->levels.size()) {
			++culled_entities_; // past the last LOD
			return;
		}
		const Mesh& mesh = geometry->levels[static_cast<std::size_t>(level)];
		if (!visible(world_bounds(transform, mesh.bounds))) {
			return;
		}
		push_mesh(entity, transform, mesh);
	});

	// Order by albedo texture, then by mesh, then as found: the items of one
	// draw end up adjacent, and the order is total so a frame draws the same
	// way every time (coplanar faces z-fight, and the later draw wins).
	std::sort(draw_items_.begin(), draw_items_.end(),
	          [](const DrawItem& a, const DrawItem& b) {
		          if (const int by_albedo = a.albedo->compare(*b.albedo);
		              by_albedo != 0) {
			          return by_albedo < 0;
		          }
		          if (a.slot != b.slot) {
			          return a.slot < b.slot;
		          }
		          return a.order < b.order;
	          });
}

void Scene::emit_item(const DrawItem& item, GpuVertex* vertices,
                      glm::vec2* uvs)
{
	GpuVertex* out    = vertices + item.first_vertex;
	glm::vec2* uv_out = uvs + item.first_vertex;
	if (item.mesh != nullptr) {
		emit_mesh(out, uv_out, *item.transform, *item.mesh, item.color,
		          item.roughness);
	} else {
		emit_sprite(out, uv_out, *item.transform, *item.sprite);
	}
}

std::vector<GpuVertex> Scene::build_vertices(float aspect,
                                              const DepthPyramid* occlusion)
{
	build_frame(aspect, occlusion);

	// Every item becomes world-space vertices here, instanced or not.
	uint32_t total                  = 0;
	uint32_t batches                = 0;
	const std::string* batch_albedo = nullptr;
	for (DrawItem& item : draw_items_) {
		item.vertex_count =
		        item.mesh != nullptr ? count_valid_indices(*item.mesh) : 6u;
		item.first_vertex = total;
		if (item.vertex_count == 0) {
			continue;
		}
		if (batch_albedo == nullptr || *batch_albedo != *item.albedo) {
			++batches;
			batch_albedo = item.albedo;
		}
		total += item.vertex_count;
	}
	draw_call_count_ = batches;

	std::vector<GpuVertex> vertices(total);
	std::vector<glm::vec2> uvs(total);
	for (const DrawItem& item : draw_items_) {
		if (item.vertex_count != 0) {
			emit_item(item, vertices.data(), uvs.data());
		}
	}
	return vertices;
}

void Scene::upload(GpuRenderer& gpu, float aspect)
{
	const DepthPyramid* occlusion = occlusion_for_pass(
	        gpu.occlusion_pyramid(), gpu.occlusion_ready(),
	        gpu.occlusion_aspect(), aspect);
	build_frame(aspect, occlusion);

	vertex_count_    = 0;
	draw_call_count_ = 0;
	draw_batches_.clear();
	if (!gpu.has_frame() || draw_items_.empty()) {
		return;
	}

	// Lay the frame out: one batch per run of items that share an albedo and
	// a mesh slot. A run of instanced items is one instanced draw; a run of
	// the others is one range of the world-space stream.
	uint32_t immediate_vertices = 0;
	uint32_t instances          = 0;
	uint64_t submitted          = 0;
	const std::size_t count     = draw_items_.size();
	for (std::size_t begin = 0; begin < count;) {
		const DrawItem& head = draw_items_[begin];
		std::size_t end      = begin + 1;
		while (end < count && draw_items_[end].slot == head.slot &&
		       *draw_items_[end].albedo == *head.albedo) {
			++end;
		}
		if (head.slot == 0) {
			const uint32_t first = immediate_vertices;
			for (std::size_t i = begin; i < end; ++i) {
				DrawItem& item    = draw_items_[i];
				item.vertex_count = item.mesh != nullptr
				                            ? count_valid_indices(*item.mesh)
				                            : 6u;
				item.first_vertex = immediate_vertices;
				immediate_vertices += item.vertex_count;
			}
			if (immediate_vertices != first) {
				draw_batches_.push_back(GpuDrawBatch{
				        *head.albedo, first, immediate_vertices - first});
				submitted += immediate_vertices - first;
			}
		} else {
			const Mesh& mesh = *head.mesh;
			const GpuMeshData data{
			        mesh.positions.data(), mesh.normals.data(),
			        mesh.uvs.data(),       mesh.indices.data(),
			        mesh.positions.size(), mesh.normals.size(),
			        mesh.uvs.size(),       mesh.indices.size()};
			const GpuMeshId id = gpu.acquire_mesh(mesh.source, data);
			if (id != kNoGpuMesh) {
				const uint32_t run = static_cast<uint32_t>(end - begin);
				for (std::size_t i = begin; i < end; ++i) {
					draw_items_[i].instance =
					        instances + static_cast<uint32_t>(i - begin);
				}
				const uint32_t vertices = gpu.mesh_index_count(id) * run;
				draw_batches_.push_back(GpuDrawBatch{
				        *head.albedo, 0, vertices, id, instances, run});
				instances += run;
				submitted += vertices;
			}
		}
		begin = end;
	}
	vertex_count_    = static_cast<uint32_t>(submitted);
	draw_call_count_ = static_cast<uint32_t>(draw_batches_.size());
	if (draw_batches_.empty()) {
		return;
	}

	// Generate straight into the GPU staging memory: no host-side copy of
	// the vertices, and for instanced geometry just one record per entity.
	const GpuRenderer::GeometryStaging staging =
	        gpu.begin_geometry_upload(immediate_vertices, instances);
	if (staging.instances == nullptr) {
		return; // no frame in flight
	}
	for (const DrawItem& item : draw_items_) {
		if (item.slot != 0) {
			if (item.instance != kNoInstance) {
				staging.instances[item.instance] =
				        GpuInstance{item.transform->world, item.color,
				                    item.roughness};
			}
		} else if (item.vertex_count != 0) {
			emit_item(item, staging.vertices, staging.uvs);
		}
	}
	gpu.commit_geometry_upload(draw_batches_.data(),
	                           static_cast<uint32_t>(draw_batches_.size()));
}

void Scene::draw(GpuRenderer& gpu)
{
	if (vertex_count_ == 0) {
		return;
	}
	gpu.draw(view_, view_projection_, camera_position_, draw_batches_.data(),
	         static_cast<std::uint32_t>(draw_batches_.size()), assets_);
}

std::vector<glm::vec4> Scene::blob_spheres() const
{
	std::vector<glm::vec4> spheres;
	for (const Entity entity : registry_.view<Transform, BlobCaster>()) {
		if (spheres.size() >= 4) {
			break;
		}
		const Transform& transform =
		        registry_.get<Transform>(entity);
		const BlobCaster& caster = registry_.get<BlobCaster>(entity);
		if (caster.radius <= 0.0f) {
			continue;
		}
		const glm::vec3 center =
		        glm::vec3{transform.world * glm::vec4{0.0f, 0.0f, 0.0f, 1.0f}};
		spheres.emplace_back(center, caster.radius);
	}
	return spheres;
}

void update_world_transforms(Scene& scene)
{
	auto& registry = scene.registry();
	for (const Entity root : scene.roots()) {
		propagate(registry, root, glm::mat4{1.0f});
	}
}

} // namespace lotus

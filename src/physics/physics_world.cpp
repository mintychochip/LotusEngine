#include "lotus/physics/physics_world.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

#include <memory>
#include <unordered_map>

#include "lotus/scene/components.hpp"
#include "lotus/scene/scene.hpp"

JPH_SUPPRESS_WARNINGS

namespace lotus {
namespace {

constexpr float kFixedTimestep   = 1.0f / 60.0f;
constexpr int kMaxSubstepsPerFrame = 4;

namespace Layers {
static constexpr JPH::ObjectLayer kNonMoving = 0;
static constexpr JPH::ObjectLayer kMoving    = 1;
static constexpr JPH::ObjectLayer kNumLayers = 2;
} // namespace Layers

class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
public:
	bool ShouldCollide(JPH::ObjectLayer object1,
	                   JPH::ObjectLayer object2) const override
	{
		switch (object1) {
		case Layers::kNonMoving:
			return object2 == Layers::kMoving;
		case Layers::kMoving:
			return true;
		default:
			return false;
		}
	}
};

class BroadPhaseLayerInterface final : public JPH::BroadPhaseLayerInterface {
public:
	BroadPhaseLayerInterface()
	{
		object_to_broad_phase_[Layers::kNonMoving] =
		        JPH::BroadPhaseLayer(0);
		object_to_broad_phase_[Layers::kMoving] = JPH::BroadPhaseLayer(1);
	}

	JPH::uint GetNumBroadPhaseLayers() const override { return 2; }

	JPH::BroadPhaseLayer GetBroadPhaseLayer(
	        JPH::ObjectLayer layer) const override
	{
		return object_to_broad_phase_[layer];
	}

	const char* GetBroadPhaseLayerName(
	        JPH::BroadPhaseLayer layer) const override
	{
		switch (static_cast<JPH::BroadPhaseLayer::Type>(layer)) {
		case 0:
			return "NON_MOVING";
		case 1:
			return "MOVING";
		default:
			return "INVALID";
		}
	}

private:
	JPH::BroadPhaseLayer object_to_broad_phase_[Layers::kNumLayers];
};

class ObjectVsBroadPhaseLayerFilter final
    : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
	bool ShouldCollide(JPH::ObjectLayer layer1,
	                   JPH::BroadPhaseLayer layer2) const override
	{
		switch (layer1) {
		case Layers::kNonMoving:
			return layer2 == JPH::BroadPhaseLayer(0);
		case Layers::kMoving:
			return true;
		default:
			return false;
		}
	}
};

JPH::EMotionType to_jolt_motion(PhysicsMotionType type)
{
	switch (type) {
	case PhysicsMotionType::Static:
		return JPH::EMotionType::Static;
	case PhysicsMotionType::Kinematic:
		return JPH::EMotionType::Kinematic;
	case PhysicsMotionType::Dynamic:
		return JPH::EMotionType::Dynamic;
	}
	return JPH::EMotionType::Dynamic;
}

JPH::ObjectLayer object_layer_for(PhysicsMotionType type)
{
	return type == PhysicsMotionType::Static ? Layers::kNonMoving
	                                         : Layers::kMoving;
}

glm::vec3 world_position(const Transform& transform)
{
	return glm::vec3{transform.world[3]};
}

glm::quat world_rotation(const Transform& transform)
{
	glm::vec3 scale;
	glm::quat rotation;
	glm::vec3 translation;
	glm::vec3 skew;
	glm::vec4 perspective;
	glm::decompose(transform.world, scale, rotation, translation, skew,
	               perspective);
	return rotation;
}

JPH::RVec3 to_jolt_vec3(const glm::vec3& v)
{
	return JPH::RVec3{v.x, v.y, v.z};
}

JPH::Vec3 to_jolt_vec3f(const glm::vec3& v)
{
	return JPH::Vec3{v.x, v.y, v.z};
}

JPH::Quat to_jolt_quat(const glm::quat& q)
{
	return JPH::Quat{q.x, q.y, q.z, q.w};
}

glm::vec3 to_glm_vec3(const JPH::RVec3& v)
{
	return glm::vec3{static_cast<float>(v.GetX()),
	               static_cast<float>(v.GetY()),
	               static_cast<float>(v.GetZ())};
}

glm::vec3 to_glm_vec3f(const JPH::Vec3& v)
{
	return glm::vec3{v.GetX(), v.GetY(), v.GetZ()};
}

glm::quat to_glm_quat(const JPH::Quat& q)
{
	return glm::quat{q.GetW(), q.GetX(), q.GetY(), q.GetZ()};
}

bool has_collider(const Registry& registry, Entity entity)
{
	return registry.all_of<BoxCollider>(entity) ||
	       registry.all_of<SphereCollider>(entity) ||
	       registry.all_of<MeshCollider>(entity);
}

bool scene_has_rigid_physics(const Registry& registry)
{
	for (const Entity entity : registry.view<RigidBody>()) {
		if (has_collider(registry, entity)) {
			return true;
		}
	}
	return false;
}

bool scene_has_characters(const Registry& registry)
{
	return !registry.view<CharacterController>().empty();
}

float capsule_center_y(const CharacterController& controller)
{
	return controller.half_height + controller.radius;
}

glm::vec3 feet_from_capsule_center(const glm::vec3& center,
                                   const CharacterController& controller)
{
	return center - glm::vec3{0.0f, capsule_center_y(controller), 0.0f};
}

glm::vec3 capsule_center_from_feet(const glm::vec3& feet,
                                   const CharacterController& controller)
{
	return feet + glm::vec3{0.0f, capsule_center_y(controller), 0.0f};
}

JPH::RefConst<JPH::Shape> make_shape(const Registry& registry, Entity entity)
{
	if (const auto* box = registry.try_get<BoxCollider>(entity)) {
		return new JPH::BoxShape{JPH::Vec3{box->half_extents.x,
		                                   box->half_extents.y,
		                                   box->half_extents.z}};
	}
	if (const auto* sphere = registry.try_get<SphereCollider>(entity)) {
		return new JPH::SphereShape{sphere->radius};
	}
	if (registry.all_of<MeshCollider>(entity)) {
		const Mesh* mesh = registry.try_get<Mesh>(entity);
		const Transform* transform = registry.try_get<Transform>(entity);
		if (mesh == nullptr || mesh->positions.empty() || transform == nullptr) {
			return nullptr;
		}
		constexpr std::size_t kMaxPoints = 256;
		JPH::Array<JPH::Vec3> points;
		const std::size_t step =
		        std::max<std::size_t>(1, mesh->positions.size() / kMaxPoints);
		for (std::size_t i = 0; i < mesh->positions.size(); i += step) {
			const glm::vec3 scaled = mesh->positions[i] * transform->scale;
			points.push_back(JPH::Vec3{scaled.x, scaled.y, scaled.z});
		}
		if (points.empty()) {
			return nullptr;
		}
		JPH::ConvexHullShapeSettings settings(points.data(),
		                                      static_cast<int>(points.size()));
		const JPH::Shape::ShapeResult result = settings.Create();
		if (!result.IsValid()) {
			return nullptr;
		}
		return result.Get();
	}
	return nullptr;
}

} // namespace

struct PhysicsWorld::Impl {
	enum class ColliderKind : std::uint8_t { None, Box, Sphere, Mesh };

	struct BodySyncKey {
		PhysicsMotionType motion{PhysicsMotionType::Dynamic};
		ColliderKind kind{ColliderKind::None};
		glm::vec3 box_extents{0.0f};
		float sphere_radius{0.0f};
		std::size_t mesh_vertices{0};
		glm::vec3 mesh_scale{1.0f};

		bool operator==(const BodySyncKey& other) const
		{
			return motion == other.motion && kind == other.kind &&
			       box_extents == other.box_extents &&
			       sphere_radius == other.sphere_radius &&
			       mesh_vertices == other.mesh_vertices &&
			       mesh_scale == other.mesh_scale;
		}
	};

	struct CharacterSlot {
		std::unique_ptr<JPH::CharacterVirtual> character;
		float radius{0.0f};
		float half_height{0.0f};
	};

	JPH::PhysicsSystem physics;
	JPH::JobSystemSingleThreaded job_system;
	std::unique_ptr<JPH::TempAllocatorImpl> temp_allocator;
	BroadPhaseLayerInterface broad_phase_layers;
	ObjectVsBroadPhaseLayerFilter object_vs_broadphase;
	ObjectLayerPairFilter object_vs_object;
	std::unordered_map<Entity, JPH::BodyID> bodies;
	std::unordered_map<Entity, BodySyncKey> body_keys;
	std::unordered_map<Entity, CharacterSlot> characters;
	float accumulator{0.0f};

	Impl()
	{
		static bool jolt_globals = false;
		if (!jolt_globals) {
			JPH::RegisterDefaultAllocator();
			JPH::Factory::sInstance = new JPH::Factory();
			JPH::RegisterTypes();
			jolt_globals = true;
		}
		temp_allocator = std::make_unique<JPH::TempAllocatorImpl>(
		        4 * 1024 * 1024);
		job_system.Init(2048);
		constexpr JPH::uint max_bodies              = 4096;
		constexpr JPH::uint max_body_pairs          = 4096;
		constexpr JPH::uint max_contact_constraints = 4096;
		physics.Init(max_bodies, 0, max_body_pairs, max_contact_constraints,
		             broad_phase_layers, object_vs_broadphase,
		             object_vs_object);
		physics.SetGravity(JPH::Vec3{0.0f, -9.81f, 0.0f});
	}

	~Impl()
	{
		JPH::BodyInterface& body_interface = physics.GetBodyInterface();
		for (const auto& [entity, body] : bodies) {
			if (!body.IsInvalid()) {
				body_interface.RemoveBody(body);
				body_interface.DestroyBody(body);
			}
			(void)entity;
		}
		bodies.clear();
		body_keys.clear();
		characters.clear();
	}

	BodySyncKey body_sync_key(const Registry& registry, Entity entity,
	                          const RigidBody& body) const;
	void step_characters(Scene& scene, float dt);
};

PhysicsWorld::Impl::BodySyncKey PhysicsWorld::Impl::body_sync_key(
        const Registry& registry, Entity entity, const RigidBody& body) const
{
	BodySyncKey key;
	key.motion = body.motion_type;
	if (const auto* box = registry.try_get<BoxCollider>(entity)) {
		key.kind        = ColliderKind::Box;
		key.box_extents = box->half_extents;
	} else if (const auto* sphere = registry.try_get<SphereCollider>(entity)) {
		key.kind          = ColliderKind::Sphere;
		key.sphere_radius = sphere->radius;
	} else if (registry.all_of<MeshCollider>(entity)) {
		key.kind = ColliderKind::Mesh;
		if (const Mesh* mesh = registry.try_get<Mesh>(entity)) {
			key.mesh_vertices = mesh->positions.size();
		}
		if (const Transform* transform = registry.try_get<Transform>(entity)) {
			key.mesh_scale = transform->scale;
		}
	}
	return key;
}

void PhysicsWorld::Impl::step_characters(Scene& scene, float dt)
{
	auto& registry = scene.registry();
	const JPH::Vec3 gravity = physics.GetGravity();

	for (auto it = characters.begin(); it != characters.end();) {
		const Entity entity = it->first;
		if (!registry.valid(entity) ||
		    !registry.all_of<CharacterController, Transform>(entity)) {
			it = characters.erase(it);
			continue;
		}
		++it;
	}

	const auto view = registry.view<Transform, CharacterController>();
	for (const Entity entity : view) {
		Transform& transform = view.get<Transform>(entity);
		CharacterController& controller = view.get<CharacterController>(entity);

		auto existing = characters.find(entity);
		if (existing == characters.end()) {
			JPH::Ref<JPH::CharacterVirtualSettings> settings =
			        new JPH::CharacterVirtualSettings();
			settings->mShape = new JPH::CapsuleShape(controller.half_height,
			                                         controller.radius);
			settings->mSupportingVolume =
			        JPH::Plane(JPH::Vec3::sAxisY(), -controller.radius);
			const JPH::RVec3 center =
			        to_jolt_vec3(capsule_center_from_feet(transform.position,
			                                              controller));
			CharacterSlot slot;
			slot.character = std::make_unique<JPH::CharacterVirtual>(
			        settings, center, JPH::Quat::sIdentity(),
			        static_cast<JPH::uint64>(entt::to_integral(entity)),
			        &physics);
			slot.radius      = controller.radius;
			slot.half_height = controller.half_height;
			characters.emplace(entity, std::move(slot));
			existing = characters.find(entity);
		}

		CharacterSlot& slot = existing->second;
		JPH::CharacterVirtual* character = slot.character.get();

		if (slot.radius != controller.radius ||
		    slot.half_height != controller.half_height) {
			const JPH::RefConst<JPH::Shape> shape = new JPH::CapsuleShape(
			        controller.half_height, controller.radius);
			character->SetShape(
			        shape, controller.radius * 0.5f,
			        physics.GetDefaultBroadPhaseLayerFilter(Layers::kMoving),
			        physics.GetDefaultLayerFilter(Layers::kMoving), {}, {},
			        *temp_allocator);
			slot.radius      = controller.radius;
			slot.half_height = controller.half_height;
		}

		const glm::vec3 expected_center =
		        capsule_center_from_feet(transform.position, controller);
		const glm::vec3 actual_center = to_glm_vec3(character->GetPosition());
		if (glm::distance(expected_center, actual_center) > 0.1f) {
			character->SetPosition(to_jolt_vec3(expected_center));
			character->RefreshContacts(
			        physics.GetDefaultBroadPhaseLayerFilter(Layers::kMoving),
			        physics.GetDefaultLayerFilter(Layers::kMoving), {}, {},
			        *temp_allocator);
		}

		JPH::Vec3 velocity = character->GetLinearVelocity();
		if (character->GetGroundState() ==
		    JPH::CharacterVirtual::EGroundState::OnGround) {
			const JPH::Vec3 ground = character->GetGroundVelocity();
			velocity = ground + to_jolt_vec3f(
			                           glm::vec3{controller.desired_velocity.x,
			                                     0.0f,
			                                     controller.desired_velocity.z});
		} else {
			velocity += gravity * dt;
			const JPH::Vec3 desired = to_jolt_vec3f(controller.desired_velocity);
			velocity.SetX(desired.GetX());
			velocity.SetZ(desired.GetZ());
		}
		character->SetLinearVelocity(velocity);

		JPH::CharacterVirtual::ExtendedUpdateSettings update_settings;
		character->ExtendedUpdate(
		        dt, gravity, update_settings,
		        physics.GetDefaultBroadPhaseLayerFilter(Layers::kMoving),
		        physics.GetDefaultLayerFilter(Layers::kMoving), {}, {},
		        *temp_allocator);

		transform.position =
		        feet_from_capsule_center(to_glm_vec3(character->GetPosition()),
		                                 controller);
		controller.grounded =
		        character->GetGroundState() ==
		        JPH::CharacterVirtual::EGroundState::OnGround;
	}
}

PhysicsWorld::PhysicsWorld() : impl_(new Impl()) {}

PhysicsWorld::~PhysicsWorld()
{
	delete impl_;
	impl_ = nullptr;
}

void PhysicsWorld::step(Scene& scene, float dt)
{
	if (!impl_) {
		return;
	}

	auto& registry = scene.registry();
	const bool simulate_rigid = scene_has_rigid_physics(registry);
	const bool simulate_char  = scene_has_characters(registry);
	if (!simulate_rigid && !simulate_char && impl_->bodies.empty() &&
	    impl_->characters.empty()) {
		return;
	}

	auto& physics        = impl_->physics;
	auto& body_interface = physics.GetBodyInterface();

	for (auto it = impl_->bodies.begin(); it != impl_->bodies.end();) {
		const Entity entity = it->first;
		const bool keep     = registry.valid(entity) &&
		                      registry.all_of<RigidBody>(entity) &&
		                      has_collider(registry, entity);
		if (!keep) {
			if (!it->second.IsInvalid()) {
				body_interface.RemoveBody(it->second);
				body_interface.DestroyBody(it->second);
			}
			impl_->body_keys.erase(entity);
			it = impl_->bodies.erase(it);
		} else {
			++it;
		}
	}

	if (simulate_rigid) {
	const auto view = registry.view<Transform, RigidBody>();
	for (const Entity entity : view) {
		if (!has_collider(registry, entity)) {
			continue;
		}
		const Transform& transform = view.get<Transform>(entity);
		const RigidBody& body      = view.get<RigidBody>(entity);

		const Impl::BodySyncKey key =
		        impl_->body_sync_key(registry, entity, body);
		auto existing         = impl_->bodies.find(entity);
		if (existing != impl_->bodies.end()) {
			const auto key_it = impl_->body_keys.find(entity);
			if (key_it == impl_->body_keys.end() || key_it->second != key) {
				if (!existing->second.IsInvalid()) {
					body_interface.RemoveBody(existing->second);
					body_interface.DestroyBody(existing->second);
				}
				impl_->bodies.erase(existing);
				impl_->body_keys.erase(entity);
				existing = impl_->bodies.end();
			}
		}
		if (existing == impl_->bodies.end()) {
			const JPH::RefConst<JPH::Shape> shape =
			        make_shape(registry, entity);
			if (!shape) {
				continue;
			}
			JPH::BodyCreationSettings settings(
			        shape, to_jolt_vec3(world_position(transform)),
			        to_jolt_quat(world_rotation(transform)),
			        to_jolt_motion(body.motion_type),
			        object_layer_for(body.motion_type));
			settings.mFriction    = body.friction;
			settings.mRestitution = body.restitution;
			if (body.motion_type == PhysicsMotionType::Dynamic) {
				settings.mOverrideMassProperties =
				        JPH::EOverrideMassProperties::CalculateInertia;
				settings.mMassPropertiesOverride.mMass = body.mass;
			}
			JPH::Body* created = body_interface.CreateBody(settings);
			if (!created) {
				continue;
			}
			created->SetUserData(
			        static_cast<JPH::uint64>(entt::to_integral(entity)));
			const JPH::BodyID id = created->GetID();
			body_interface.AddBody(id, JPH::EActivation::Activate);
			impl_->bodies.emplace(entity, id);
			impl_->body_keys.emplace(entity, key);
			continue;
		}

		if (body.motion_type == PhysicsMotionType::Dynamic) {
			continue;
		}
		body_interface.SetPositionAndRotation(
		        existing->second, to_jolt_vec3(world_position(transform)),
		        to_jolt_quat(world_rotation(transform)),
		        JPH::EActivation::DontActivate);
	}

	impl_->accumulator += dt;
	int substeps = 0;
	while (impl_->accumulator >= kFixedTimestep &&
	       substeps < kMaxSubstepsPerFrame) {
		physics.Update(kFixedTimestep, 1, impl_->temp_allocator.get(),
		               &impl_->job_system);
		impl_->accumulator -= kFixedTimestep;
		++substeps;
	}

	for (const auto& [entity, id] : impl_->bodies) {
		if (!registry.valid(entity)) {
			continue;
		}
		const RigidBody* body = registry.try_get<RigidBody>(entity);
		if (!body || body->motion_type != PhysicsMotionType::Dynamic) {
			continue;
		}
		Transform* transform = registry.try_get<Transform>(entity);
		if (!transform) {
			continue;
		}
		JPH::RVec3 position;
		JPH::Quat rotation;
		body_interface.GetPositionAndRotation(id, position, rotation);
		transform->position = to_glm_vec3(position);
		transform->rotation = glm::eulerAngles(to_glm_quat(rotation));
	}
	}

	if (simulate_char || !impl_->characters.empty()) {
		impl_->step_characters(scene, dt);
	}

	update_world_transforms(scene);
}

void PhysicsWorld::set_character_velocity(Scene& scene, Entity entity,
                                          const glm::vec3& velocity)
{
	if (CharacterController* controller =
	            scene.registry().try_get<CharacterController>(entity)) {
		controller->desired_velocity = velocity;
	}
}

bool PhysicsWorld::raycast(const glm::vec3& origin, const glm::vec3& direction,
                           float max_distance, PhysicsRaycastHit& hit) const
{
	if (!impl_ || max_distance <= 0.0f) {
		return false;
	}
	const float length = glm::length(direction);
	if (length <= 0.0f) {
		return false;
	}
	const glm::vec3 unit = direction / length;

	JPH::RRayCast ray{to_jolt_vec3(origin),
	                  to_jolt_vec3(unit * max_distance)};
	JPH::RayCastResult result;
	if (!impl_->physics.GetNarrowPhaseQuery().CastRay(ray, result)) {
		return false;
	}

	JPH::BodyLockRead lock(impl_->physics.GetBodyLockInterface(),
	                       result.mBodyID);
	if (!lock.Succeeded()) {
		return false;
	}
	const JPH::Body& body = lock.GetBody();
	hit.entity            = static_cast<Entity>(body.GetUserData());
	hit.fraction          = result.mFraction;
	hit.point             = to_glm_vec3(ray.GetPointOnRay(result.mFraction));
	hit.normal = to_glm_vec3f(body.GetWorldSpaceSurfaceNormal(
	        result.mSubShapeID2, ray.GetPointOnRay(result.mFraction)));
	return true;
}

} // namespace lotus

#include <gtest/gtest.h>

#include "lotus/physics/physics_world.hpp"
#include "lotus/scene/components.hpp"
#include "lotus/scene/mesh.hpp"
#include "lotus/scene/scene.hpp"

using namespace lotus;

namespace {

constexpr float kTerrainSize = 20.0f;
constexpr int kSegments      = 16;
constexpr float kPeakHeight  = 4.0f;
constexpr int kTerrainSeed   = 7;

Entity make_terrain_floor(Scene& scene, const HeightFieldCollider& collider)
{
	const Entity ground = scene.create("ground");
	scene.registry().emplace<RigidBody>(
	        ground, RigidBody{PhysicsMotionType::Static});
	scene.registry().emplace<Mesh>(
	        ground, mesh::terrain(kTerrainSize, kSegments, kPeakHeight,
	                              kTerrainSeed));
	scene.registry().emplace<HeightFieldCollider>(ground, collider);
	return ground;
}

} // namespace

TEST(HeightFieldColliderTest, RaycastHitsTerrainSurface)
{
	Scene scene{"hf_ray"};
	PhysicsWorld world;

	const Entity ground =
	        make_terrain_floor(scene, HeightFieldCollider{0.0f, kPeakHeight});
	scene.update(0.0f);
	world.step(scene, 0.0f);

	const float expected =
	        mesh::terrain_height_at(0.0f, 0.0f, kTerrainSize, kPeakHeight,
	                                kTerrainSeed);

	PhysicsRaycastHit hit;
	EXPECT_TRUE(world.raycast({0.0f, kPeakHeight + 2.0f, 0.0f},
	                          {0.0f, -1.0f, 0.0f}, 20.0f, hit));
	EXPECT_EQ(hit.entity, ground);
	EXPECT_NEAR(hit.point.y, expected, 0.05f);
}

TEST(HeightFieldColliderTest, MaxHeightClampsSurface)
{
	Scene scene{"hf_clamp"};
	PhysicsWorld world;

	constexpr float clamped_max = 0.1f;
	make_terrain_floor(scene, HeightFieldCollider{0.0f, clamped_max});
	scene.update(0.0f);
	world.step(scene, 0.0f);

	PhysicsRaycastHit hit;
	EXPECT_TRUE(world.raycast({0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 20.0f,
	                          hit));
	EXPECT_NEAR(hit.point.y, clamped_max, 0.05f);
}

TEST(HeightFieldColliderTest, NonGridMeshCreatesNoBody)
{
	Scene scene{"hf_bad_mesh"};
	PhysicsWorld world;

	const Entity ground = scene.create("ground");
	scene.registry().emplace<RigidBody>(
	        ground, RigidBody{PhysicsMotionType::Static});
	scene.registry().emplace<Mesh>(ground, mesh::cube(2.0f));
	scene.registry().emplace<HeightFieldCollider>(
	        ground, HeightFieldCollider{0.0f, 1.0f});
	scene.update(0.0f);
	world.step(scene, 0.0f);

	PhysicsRaycastHit hit;
	EXPECT_FALSE(world.raycast({0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 20.0f,
	                           hit));
}

TEST(HeightFieldColliderTest, CharacterLandsOnTerrain)
{
	Scene scene{"hf_character"};
	PhysicsWorld world;

	make_terrain_floor(scene, HeightFieldCollider{0.0f, kPeakHeight});

	const Entity player = scene.create("player");
	scene.registry().emplace<CharacterController>(player);
	scene.registry().get<Transform>(player).position = {0.0f, 4.0f, 0.0f};

	scene.update(0.0f);
	for (int i = 0; i < 180; ++i) {
		world.step(scene, 1.0f / 60.0f);
	}

	const float expected =
	        mesh::terrain_height_at(0.0f, 0.0f, kTerrainSize, kPeakHeight,
	                                kTerrainSeed);
	const float y = scene.registry().get<Transform>(player).position.y;
	EXPECT_NEAR(y, expected, 0.05f);
	EXPECT_TRUE(scene.registry().get<CharacterController>(player).grounded);
}

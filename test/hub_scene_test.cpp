#include <gtest/gtest.h>

#include "lotus/scene/components.hpp"
#include "lotus/scene/scene.hpp"
#include "lotus/scene/serialization.hpp"

using namespace lotus;

TEST(HubSceneTest, MainSceneLoadsPlayerAndFloorPhysics)
{
	Scene scene{"main"};
	std::string error;
	ASSERT_TRUE(load_scene(scene, "scenes/main.json", &error)) << error;

	const Entity player = scene.find("player");
	const Entity floor  = scene.find("floor");
	EXPECT_NE(player, NullEntity);
	EXPECT_NE(floor, NullEntity);
	EXPECT_TRUE(scene.registry().all_of<CharacterController>(player));
	EXPECT_TRUE(scene.registry().all_of<RigidBody>(floor));
	EXPECT_TRUE(scene.registry().all_of<BoxCollider>(floor));

	const Entity bob = scene.find("bob_prop");
	EXPECT_NE(bob, NullEntity);
	EXPECT_TRUE(scene.registry().all_of<Animator>(bob));
	EXPECT_EQ(scene.registry().get<Animator>(bob).clip_id, "sample_bob");
}

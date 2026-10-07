#include <gtest/gtest.h>

#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "lotus/scene/culling.hpp"
#include "lotus/scene/lod.hpp"
#include "lotus/scene/scene.hpp"

using namespace lotus;

namespace {

glm::mat4 camera_view_projection(const glm::vec3& eye)
{
	const glm::mat4 view = glm::lookAtRH(eye, glm::vec3{0.0f}, glm::vec3{0.0f, 1.0f, 0.0f});
	const glm::mat4 projection =
	        glm::perspectiveRH_ZO(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
	return projection * view;
}

} // namespace

TEST(FrustumTest, ExtractsPlanesFromZeroToOneProjection)
{
	const Frustum frustum =
	        Frustum::from_view_projection(camera_view_projection({0.0f, 0.0f, 5.0f}));

	// Planes are normalised.
	for (const glm::vec4& plane : frustum.planes) {
		EXPECT_NEAR(glm::length(glm::vec3{plane}), 1.0f, 1e-4f);
	}
}

TEST(FrustumTest, AcceptsSphereInsideAndRejectsOutside)
{
	const Frustum frustum =
	        Frustum::from_view_projection(camera_view_projection({0.0f, 0.0f, 5.0f}));

	EXPECT_TRUE(frustum.intersects_sphere({0.0f, 0.0f, 0.0f}, 1.0f));
	EXPECT_TRUE(frustum.intersects_sphere({0.0f, 0.0f, 3.0f}, 0.5f));

	// Behind the camera.
	EXPECT_FALSE(frustum.intersects_sphere({0.0f, 0.0f, 50.0f}, 1.0f));
	// Far off to the side.
	EXPECT_FALSE(frustum.intersects_sphere({100.0f, 0.0f, 0.0f}, 1.0f));
	// Beyond the far plane.
	EXPECT_FALSE(frustum.intersects_sphere({0.0f, 0.0f, -500.0f}, 1.0f));
}

TEST(FrustumTest, SphereStraddlingAPlaneIsKept)
{
	const Frustum frustum =
	        Frustum::from_view_projection(camera_view_projection({0.0f, 0.0f, 5.0f}));
	// A huge sphere centred behind the camera still overlaps the frustum.
	EXPECT_TRUE(frustum.intersects_sphere({0.0f, 0.0f, 40.0f}, 100.0f));
}

TEST(LodTest, SelectsLevelByDistance)
{
	const std::vector<float> distances{5.0f, 10.0f, 20.0f};

	EXPECT_EQ(select_lod_level(distances, 0.0f), 0);
	EXPECT_EQ(select_lod_level(distances, 5.0f), 0);  // inclusive
	EXPECT_EQ(select_lod_level(distances, 5.1f), 1);
	EXPECT_EQ(select_lod_level(distances, 10.0f), 1);
	EXPECT_EQ(select_lod_level(distances, 20.0f), 2);
	EXPECT_EQ(select_lod_level(distances, 20.1f), -1); // past the last level
}

TEST(LodTest, EmptyLevelsSelectNothing)
{
	EXPECT_EQ(select_lod_level({}, 1.0f), -1);
}

TEST(SceneCullingTest, CullsEntitiesBehindTheCamera)
{
	Scene scene{"s"};
	const Entity camera = scene.create("camera");
	scene.registry().emplace<Camera3D>(camera); // (3, 2.5, 4) looking at origin

	const Entity visible = scene.create("visible");
	scene.registry().emplace<MeshSource>(visible, MeshSource{"builtin:cube"});

	const Entity behind = scene.create("behind");
	scene.registry().emplace<MeshSource>(behind, MeshSource{"builtin:cube"});
	scene.registry().get<Transform>(behind).position = {0.0f, 0.0f, 60.0f};

	scene.update(0.0f);
	const std::vector<GpuVertex> vertices = scene.build_vertices(16.0f / 9.0f);

	EXPECT_EQ(scene.drawn_entities(), 1u);
	EXPECT_EQ(scene.culled_entities(), 1u);
	EXPECT_EQ(vertices.size(), 36u); // only the visible cube
}

TEST(SceneCullingTest, CullsSpritesBehindTheCamera)
{
	Scene scene{"s"};
	const Entity camera = scene.create("camera");
	scene.registry().emplace<Camera3D>(camera);

	const Entity sprite = scene.create("sprite");
	scene.registry().emplace<Sprite>(sprite, Sprite{});
	scene.registry().get<Transform>(sprite).position = {0.0f, 0.0f, 80.0f};

	scene.update(0.0f);
	const std::vector<GpuVertex> vertices = scene.build_vertices(16.0f / 9.0f);

	EXPECT_TRUE(vertices.empty());
	EXPECT_EQ(scene.culled_entities(), 1u);
}

TEST(SceneLodTest, NearerCameraUsesMoreDetailedLevel)
{
	Scene scene{"s"};
	const Entity camera = scene.create("camera");
	auto& camera3d        = scene.registry().emplace<Camera3D>(camera);

	const Entity ball = scene.create("ball");
	Lod lod;
	lod.levels.push_back(LodLevel{"builtin:sphere:32:24", 3.0f});
	lod.levels.push_back(LodLevel{"builtin:sphere:8:6", 50.0f});
	scene.registry().emplace<Lod>(ball, lod);

	scene.update(0.0f);

	// Camera is ~5.6 units away -> low detail level.
	const std::size_t far_vertices = scene.build_vertices(1.0f).size();
	EXPECT_EQ(scene.drawn_entities(), 1u);

	// Move the camera close -> high detail level, many more vertices.
	camera3d.position = {0.5f, 0.0f, 1.0f};
	const std::size_t near_vertices = scene.build_vertices(1.0f).size();

	EXPECT_GT(near_vertices, far_vertices);
}

TEST(SceneLodTest, CullsBeyondTheLastLevel)
{
	Scene scene{"s"};
	const Entity camera = scene.create("camera");
	scene.registry().emplace<Camera3D>(camera); // ~5.6 units from the origin

	const Entity ball = scene.create("ball");
	Lod lod;
	lod.levels.push_back(LodLevel{"builtin:sphere:8:6", 2.0f});
	scene.registry().emplace<Lod>(ball, lod);

	scene.update(0.0f);
	const std::vector<GpuVertex> vertices = scene.build_vertices(1.0f);

	EXPECT_TRUE(vertices.empty());
	EXPECT_EQ(scene.culled_entities(), 1u);
	EXPECT_EQ(scene.drawn_entities(), 0u);
}

TEST(SceneLodTest, LodTakesPrecedenceOverPlainMesh)
{
	Scene scene{"s"};
	const Entity camera = scene.create("camera");
	scene.registry().emplace<Camera3D>(camera);

	const Entity ball = scene.create("ball");
	scene.registry().emplace<MeshSource>(ball, MeshSource{"builtin:cube"});
	Lod lod;
	lod.levels.push_back(LodLevel{"builtin:sphere:8:6", 50.0f});
	scene.registry().emplace<Lod>(ball, lod);

	scene.update(0.0f);
	const std::vector<GpuVertex> vertices = scene.build_vertices(1.0f);

	// One entity drawn (not two): the Lod level wins over the plain Mesh.
	EXPECT_EQ(scene.drawn_entities(), 1u);
	EXPECT_FALSE(vertices.empty());
}

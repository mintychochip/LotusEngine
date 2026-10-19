#include <gtest/gtest.h>

#include <set>
#include <string>

#include <glm/glm.hpp>

#include "lotus/scene/mesh.hpp"
#include "lotus/scene/scene.hpp"

using namespace lotus;

TEST(MeshTest, CubeGeometryIsWellFormed)
{
	const Mesh cube = mesh::cube(2.0f);

	EXPECT_EQ(cube.positions.size(), 24u); // 6 faces x 4 vertices
	EXPECT_EQ(cube.normals.size(), cube.positions.size());
	EXPECT_EQ(cube.indices.size(), 36u);
	EXPECT_EQ(cube.triangle_count(), 12u);
	EXPECT_FALSE(cube.empty());

	for (const std::uint32_t index : cube.indices) {
		EXPECT_LT(index, cube.positions.size());
	}
	for (const glm::vec3& normal : cube.normals) {
		EXPECT_NEAR(glm::length(normal), 1.0f, 1e-5f);
	}

	// Six distinct axis-aligned face normals.
	std::set<std::tuple<int, int, int>> unique;
	for (const glm::vec3& n : cube.normals) {
		unique.emplace(static_cast<int>(n.x), static_cast<int>(n.y),
		               static_cast<int>(n.z));
	}
	EXPECT_EQ(unique.size(), 6u);

	// size 2 => half extent 1.
	for (const glm::vec3& p : cube.positions) {
		EXPECT_LE(std::abs(p.x), 1.0f + 1e-5f);
		EXPECT_LE(std::abs(p.y), 1.0f + 1e-5f);
		EXPECT_LE(std::abs(p.z), 1.0f + 1e-5f);
	}
}

TEST(MeshTest, SphereGeometryIsWellFormed)
{
	const Mesh sphere = mesh::sphere(0.5f, 8, 6);

	EXPECT_FALSE(sphere.empty());
	EXPECT_EQ(sphere.normals.size(), sphere.positions.size());
	EXPECT_EQ(sphere.indices.size() % 3, 0u);

	for (const std::uint32_t index : sphere.indices) {
		EXPECT_LT(index, sphere.positions.size());
	}
	for (std::size_t i = 0; i < sphere.positions.size(); ++i) {
		// Unit sphere: normal == position / radius, so position length == radius.
		EXPECT_NEAR(glm::length(sphere.positions[i]), 0.5f, 1e-4f);
		EXPECT_NEAR(glm::length(sphere.normals[i]), 1.0f, 1e-4f);
	}
}

TEST(MeshTest, ResolveBuiltins)
{
	Mesh out;
	std::string error;

	ASSERT_TRUE(mesh::resolve_mesh("builtin:cube", out, &error)) << error;
	EXPECT_EQ(out.indices.size(), 36u);

	ASSERT_TRUE(mesh::resolve_mesh("builtin:sphere", out, &error)) << error;
	EXPECT_FALSE(out.empty());

	EXPECT_FALSE(mesh::resolve_mesh("builtin:nope", out, &error));
	EXPECT_FALSE(error.empty());
}

TEST(MeshTest, SceneResolvesMeshSourceIntoGeometry)
{
	Scene scene{"s"};
	const Entity e = scene.create("cube");
	scene.registry().emplace<MeshSource>(e, MeshSource{"builtin:cube", ""});
	EXPECT_FALSE(scene.registry().all_of<Mesh>(e));

	scene.update(0.0f);

	ASSERT_TRUE(scene.registry().all_of<Mesh>(e));
	const Mesh& mesh = scene.registry().get<Mesh>(e);
	EXPECT_EQ(mesh.indices.size(), 36u);
	EXPECT_EQ(mesh.source, "builtin:cube");
	EXPECT_TRUE(scene.last_mesh_error().empty());
}

TEST(MeshTest, SceneReportsUnknownMeshSource)
{
	Scene scene{"s"};
	const Entity e = scene.create("bad");
	scene.registry().emplace<MeshSource>(e, MeshSource{"builtin:missing", ""});

	scene.update(0.0f);

	EXPECT_FALSE(scene.registry().all_of<Mesh>(e));
	EXPECT_FALSE(scene.last_mesh_error().empty());
}

TEST(MeshTest, SceneReResolvesWhenSourceChanges)
{
	Scene scene{"s"};
	const Entity e = scene.create("thing");
	scene.registry().emplace<MeshSource>(e, MeshSource{"builtin:cube", ""});
	scene.update(0.0f);
	const std::size_t cube_indices = scene.registry().get<Mesh>(e).indices.size();

	scene.registry().get<MeshSource>(e).path = "builtin:sphere";
	scene.update(0.0f);

	const Mesh& mesh = scene.registry().get<Mesh>(e);
	EXPECT_EQ(mesh.source, "builtin:sphere");
	EXPECT_NE(mesh.indices.size(), cube_indices);
}

TEST(MeshTest, TerrainMatchesHeightAt)
{
	// terrain() vertices and terrain_height_at() share one noise code path,
	// so sampled heights must be bit-identical.
	constexpr float kSize = 20.0f;
	constexpr int kSegments = 16;
	constexpr float kHeight = 4.0f;
	constexpr int kSeed = 7;
	const Mesh ground = mesh::terrain(kSize, kSegments, kHeight, kSeed);
	const int verts_per_side = kSegments + 1;
	ASSERT_EQ(ground.positions.size(),
	          static_cast<std::size_t>(verts_per_side * verts_per_side));
	for (int z = 0; z <= kSegments; ++z) {
		for (int x = 0; x <= kSegments; ++x) {
			const int idx = z * verts_per_side + x;
			const glm::vec3& p = ground.positions[idx];
			EXPECT_FLOAT_EQ(p.y, mesh::terrain_height_at(p.x, p.z, kSize,
			                                             kHeight, kSeed));
		}
	}
}

TEST(MeshTest, TerrainHeightsAreDeterministic)
{
	const Mesh first = mesh::terrain(20.0f, 16, 4.0f, 7);
	const Mesh second = mesh::terrain(20.0f, 16, 4.0f, 7);
	ASSERT_EQ(first.positions.size(), second.positions.size());
	for (std::size_t i = 0; i < first.positions.size(); ++i) {
		EXPECT_FLOAT_EQ(first.positions[i].y, second.positions[i].y);
	}
	EXPECT_FLOAT_EQ(mesh::terrain_height_at(1.0f, 2.0f, 20.0f, 4.0f, 7),
	                mesh::terrain_height_at(1.0f, 2.0f, 20.0f, 4.0f, 7));
	// A different seed must perturb the surface.
	EXPECT_NE(mesh::terrain_height_at(1.0f, 2.0f, 20.0f, 4.0f, 7),
	          mesh::terrain_height_at(1.0f, 2.0f, 20.0f, 4.0f, 8));
}

TEST(MeshTest, TerrainPinsExactHeights)
{
	// Quintic value-noise fbm (lacunarity 2.02, rotated domains) reference
	// values; tolerance covers float evaluation order, not noise changes.
	EXPECT_NEAR(mesh::terrain_height_at(0.0f, 0.0f, 20.0f, 4.0f, 7),
	            1.5721310f, 1e-4f);
	EXPECT_NEAR(mesh::terrain_height_at(-10.0f, -10.0f, 20.0f, 4.0f, 7),
	            0.5853509f, 1e-4f);
	EXPECT_NEAR(mesh::terrain_height_at(10.0f, 10.0f, 20.0f, 4.0f, 7),
	            1.1690160f, 1e-4f);
	EXPECT_NEAR(mesh::terrain_height_at(5.0f, 2.5f, 20.0f, 4.0f, 7),
	            1.0690796f, 1e-4f);
	EXPECT_NEAR(mesh::terrain_height_at(0.0f, 0.0f, 50.0f, 8.0f, 42),
	            3.9521205f, 1e-4f);
	EXPECT_NEAR(mesh::terrain_height_at(-25.0f, -25.0f, 50.0f, 8.0f, 42),
	            5.3330250f, 1e-4f);
	EXPECT_NEAR(mesh::terrain_height_at(12.5f, -7.5f, 50.0f, 8.0f, 42),
	            1.3376019f, 1e-4f);
}

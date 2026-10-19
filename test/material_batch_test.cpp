#include <gtest/gtest.h>

#include "lotus/scene/components.hpp"
#include "lotus/scene/scene.hpp"

using namespace lotus;

TEST(MaterialBatchTest, DistinctAlbedoIdsProduceSeparateDrawCalls)
{
	Scene scene{"test"};
	const Entity a = scene.create("a");
	const Entity b = scene.create("b");
	scene.registry().emplace<MeshSource>(a, MeshSource{"builtin:cube", ""});
	scene.registry().emplace<MeshSource>(b, MeshSource{"builtin:cube", ""});
	scene.registry().emplace<Material>(a, Material{{1.0f, 1.0f, 1.0f}, "tex_a"});
	scene.registry().emplace<Material>(b, Material{{1.0f, 1.0f, 1.0f}, "tex_b"});

	scene.update(0.0f);
	(void)scene.build_vertices(1.0f);

	EXPECT_EQ(scene.draw_calls(), 2u);
}

TEST(MaterialBatchTest, SameAlbedoIdMergesIntoOneDrawCall)
{
	Scene scene{"test"};
	const Entity a = scene.create("a");
	const Entity b = scene.create("b");
	scene.registry().emplace<MeshSource>(a, MeshSource{"builtin:cube", ""});
	scene.registry().emplace<MeshSource>(b, MeshSource{"builtin:sphere", ""});
	scene.registry().emplace<Material>(a, Material{{1.0f, 1.0f, 1.0f}, "shared"});
	scene.registry().emplace<Material>(b, Material{{1.0f, 1.0f, 1.0f}, "shared"});

	scene.update(0.0f);
	(void)scene.build_vertices(1.0f);

	EXPECT_EQ(scene.draw_calls(), 1u);
}

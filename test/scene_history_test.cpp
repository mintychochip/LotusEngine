#include <gtest/gtest.h>

#include <string>

#include "lotus/scene/history.hpp"
#include "lotus/scene/scene.hpp"

using namespace lotus;

TEST(SceneHistoryTest, UndoRedoRestoresEntityCount)
{
	Scene scene{"s"};
	SceneHistory history;
	history.reset(scene);
	ASSERT_EQ(scene.registry().view<Name>().size(), 0u);

	scene.create("a");
	history.commit(scene);
	scene.create("b");
	history.commit(scene);
	ASSERT_EQ(scene.registry().view<Name>().size(), 2u);

	ASSERT_TRUE(history.undo(scene));
	EXPECT_EQ(scene.registry().view<Name>().size(), 1u);
	ASSERT_TRUE(history.undo(scene));
	EXPECT_EQ(scene.registry().view<Name>().size(), 0u);
	EXPECT_FALSE(history.can_undo());
	EXPECT_FALSE(history.undo(scene));

	ASSERT_TRUE(history.redo(scene));
	EXPECT_EQ(scene.registry().view<Name>().size(), 1u);
	ASSERT_TRUE(history.redo(scene));
	EXPECT_EQ(scene.registry().view<Name>().size(), 2u);
	EXPECT_FALSE(history.can_redo());
	EXPECT_FALSE(history.redo(scene));
}

TEST(SceneHistoryTest, UndoRestoresComponentValues)
{
	Scene scene{"s"};
	const Entity e = scene.create("e");
	scene.registry().get<Transform>(e).position = {1.0f, 1.0f, 0.0f};

	SceneHistory history;
	history.reset(scene);

	scene.registry().get<Transform>(e).position = {9.0f, 9.0f, 0.0f};
	history.commit(scene);

	ASSERT_TRUE(history.undo(scene));
	const Entity restored = scene.find("e");
	ASSERT_NE(restored, NullEntity);
	EXPECT_FLOAT_EQ(scene.registry().get<Transform>(restored).position.x, 1.0f);

	ASSERT_TRUE(history.redo(scene));
	const Entity again = scene.find("e");
	ASSERT_NE(again, NullEntity);
	EXPECT_FLOAT_EQ(scene.registry().get<Transform>(again).position.x, 9.0f);
}

TEST(SceneHistoryTest, UndoRestoresHierarchy)
{
	Scene scene{"s"};
	const Entity parent = scene.create("parent");
	const Entity child  = scene.create("child");
	scene.set_parent(child, parent);

	SceneHistory history;
	history.reset(scene);

	scene.detach(child);
	history.commit(scene);
	ASSERT_EQ(scene.roots().size(), 2u);

	ASSERT_TRUE(history.undo(scene));
	EXPECT_EQ(scene.roots().size(), 1u);
	const Entity restored_child = scene.find("child");
	ASSERT_NE(restored_child, NullEntity);
	EXPECT_NE(scene.registry().get<Hierarchy>(restored_child).parent, NullEntity);
}

TEST(SceneHistoryTest, NewCommitDropsRedoSteps)
{
	Scene scene{"s"};
	SceneHistory history;
	history.reset(scene);

	scene.create("a");
	history.commit(scene);
	scene.create("b");
	history.commit(scene);

	ASSERT_TRUE(history.undo(scene));
	EXPECT_TRUE(history.can_redo());

	scene.create("c");
	history.commit(scene);
	EXPECT_FALSE(history.can_redo());
}

TEST(SceneHistoryTest, CapacityDropsOldestEntries)
{
	Scene scene{"s"};
	SceneHistory history{3};
	history.reset(scene);

	for (int i = 0; i < 5; ++i) {
		scene.create("e" + std::to_string(i));
		history.commit(scene);
	}
	EXPECT_EQ(history.size(), 3u);

	int steps = 0;
	while (history.undo(scene)) {
		++steps;
	}
	EXPECT_EQ(steps, 2); // capacity 3 == 3 states == 2 undo steps
}

TEST(SceneHistoryTest, UndoWithNoHistoryIsNoop)
{
	Scene scene{"s"};
	SceneHistory history;
	EXPECT_FALSE(history.can_undo());
	EXPECT_FALSE(history.can_redo());
	EXPECT_FALSE(history.undo(scene));
	EXPECT_FALSE(history.redo(scene));
}

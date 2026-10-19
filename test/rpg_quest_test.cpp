#include <gtest/gtest.h>

#include "lotus/rpg/components.hpp"
#include "lotus/rpg/systems.hpp"
#include "lotus/scene/scene.hpp"

using namespace lotus;
using namespace lotus::rpg;

namespace {

Quest make_kill_goblins_quest()
{
	Quest quest;
	quest.id   = "kill_goblins";
	quest.name = "Goblin Threat";
	quest.description = "Defeat the goblins attacking the village.";
	quest.state = QuestState::Active;
	quest.objectives.push_back(QuestObjective{"Defeat goblins", 3, 0});
	quest.xp_reward   = 100;
	quest.gold_reward = 50;
	quest.item_rewards.push_back(
	        Item{"Goblin Slayer Blade", "A sharp blade.", "weapon", 100, 1, 1,
	             10, 0, 0, 0});
	return quest;
}

Entity make_player(Scene& scene)
{
	const Entity player = scene.create("player");
	scene.registry().emplace<Stats>(player);
	scene.registry().emplace<Inventory>(player);
	scene.registry().emplace<Health>(player, Health{100, 100, true});
	scene.registry().emplace<Mana>(player, Mana{50, 50});
	scene.registry().emplace<Quest>(player, make_kill_goblins_quest());
	return player;
}

} // namespace

TEST(RpgQuestTest, GoblinKillsAdvanceQuestAndAwardRewards)
{
	Scene scene{"quest"};
	const Entity player = make_player(scene);

	const Entity goblin1 = scene.create("goblin1");
	const Entity goblin2 = scene.create("goblin2");
	const Entity goblin3 = scene.create("goblin3");

	EXPECT_TRUE(advance_kill_objectives(scene, player, goblin1));
	EXPECT_TRUE(advance_kill_objectives(scene, player, goblin2));
	EXPECT_TRUE(advance_kill_objectives(scene, player, goblin3));

	const auto& quest = scene.registry().get<Quest>(player);
	EXPECT_EQ(quest.state, QuestState::Completed);
	EXPECT_EQ(quest.objectives.size(), 1u);
	EXPECT_EQ(quest.objectives[0].current, 3);
	EXPECT_EQ(quest.objectives[0].required, 3);

	// 100 XP is exactly one level at the default threshold: the level-up path
	// converts it to level 2 with xp rolled over to 0.
	const auto& stats = scene.registry().get<Stats>(player);
	EXPECT_EQ(stats.level, 2);
	EXPECT_EQ(stats.xp, 0);

	const auto& inventory = scene.registry().get<Inventory>(player);
	EXPECT_EQ(inventory.gold, 50);
	EXPECT_NE(inventory.find("Goblin Slayer Blade"), nullptr);
}

TEST(RpgQuestTest, OrcKillsDoNotAdvanceGoblinObjective)
{
	Scene scene{"quest"};
	const Entity player = make_player(scene);
	const Entity orc     = scene.create("orc1");

	EXPECT_FALSE(advance_kill_objectives(scene, player, orc));

	const auto& quest = scene.registry().get<Quest>(player);
	EXPECT_EQ(quest.state, QuestState::Active);
	EXPECT_EQ(quest.objectives[0].current, 0);
}

TEST(RpgQuestTest, NonPlayerKillerCannotAdvanceQuest)
{
	Scene scene{"quest"};
	const Entity player = make_player(scene);
	const Entity killer = scene.create("enemy_killer");
	scene.registry().emplace<Stats>(killer);
	scene.registry().emplace<Health>(killer, Health{50, 50, true});
	const Entity goblin = scene.create("goblin1");

	EXPECT_FALSE(advance_kill_objectives(scene, killer, goblin));

	const auto& quest = scene.registry().get<Quest>(player);
	EXPECT_EQ(quest.objectives[0].current, 0);
	EXPECT_EQ(quest.state, QuestState::Active);
}

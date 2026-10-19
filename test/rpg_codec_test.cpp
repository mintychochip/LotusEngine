#include <gtest/gtest.h>

#include "lotus/rpg/codecs.hpp"
#include "lotus/rpg/components.hpp"
#include "lotus/scene/scene.hpp"
#include "lotus/scene/serialization.hpp"

using namespace lotus;

TEST(RpgCodecTest, StatsAreAbsentUntilTheGameRegistersThem)
{
	Scene scene{"rpg"};
	const Entity hero = scene.create("hero");
	auto& stats       = scene.registry().emplace<rpg::Stats>(hero);
	stats.level       = 4;
	stats.strength    = 18;

	EXPECT_EQ(default_component_codecs().reader("Stats"), nullptr);
	EXPECT_EQ(to_json_string(scene).find("Stats"), std::string::npos);

	rpg::register_component_codecs();

	const std::string saved = to_json_string(scene);
	EXPECT_NE(saved.find("\"Stats\""), std::string::npos);

	Scene loaded{"loaded"};
	std::string error;
	ASSERT_TRUE(scene_from_json(loaded, saved, &error)) << error;
	const Entity found = loaded.find("hero");
	ASSERT_NE(found, NullEntity);
	EXPECT_EQ(loaded.registry().get<rpg::Stats>(found).level, 4);
	EXPECT_EQ(loaded.registry().get<rpg::Stats>(found).strength, 18);
}

TEST(RpgCodecTest, QuestRoundTripsObjectivesAndItemRewards)
{
	rpg::register_component_codecs();

	Scene scene{"rpg"};
	const Entity hero = scene.create("hero");
	rpg::Quest quest;
	quest.id          = "kill_goblins";
	quest.name        = "Goblin Threat";
	quest.description = "Defeat goblins.";
	quest.state       = rpg::QuestState::Active;
	quest.objectives.push_back(
	        rpg::QuestObjective{"Defeat goblins", 3, 1});
	quest.xp_reward   = 100;
	quest.gold_reward = 50;
	quest.item_rewards.push_back(
	        rpg::Item{"Goblin Slayer Blade", "A sharp blade.", "weapon", 100,
	                  1, 1, 10, 0, 0, 0});
	scene.registry().emplace<rpg::Quest>(hero, quest);

	const std::string saved = to_json_string(scene);
	EXPECT_NE(saved.find("\"objectives\""), std::string::npos);
	EXPECT_NE(saved.find("\"item_rewards\""), std::string::npos);

	Scene loaded{"loaded"};
	std::string error;
	ASSERT_TRUE(scene_from_json(loaded, saved, &error)) << error;
	const Entity found = loaded.find("hero");
	ASSERT_NE(found, NullEntity);

	const auto& loaded_quest = loaded.registry().get<rpg::Quest>(found);
	EXPECT_EQ(loaded_quest.id, "kill_goblins");
	EXPECT_EQ(loaded_quest.objectives.size(), 1u);
	EXPECT_EQ(loaded_quest.objectives[0].description, "Defeat goblins");
	EXPECT_EQ(loaded_quest.objectives[0].required, 3);
	EXPECT_EQ(loaded_quest.objectives[0].current, 1);
	EXPECT_EQ(loaded_quest.item_rewards.size(), 1u);
	EXPECT_EQ(loaded_quest.item_rewards[0].name, "Goblin Slayer Blade");
	EXPECT_EQ(loaded_quest.gold_reward, 50);
}

TEST(RpgCodecTest, CombatRoundTripsCooldownTimer)
{
	rpg::register_component_codecs();

	Scene scene{"rpg"};
	const Entity fighter = scene.create("fighter");
	scene.registry().emplace<rpg::Combat>(
	        fighter, rpg::Combat{12, 1, 1.25f, 0.75f, true, 9.0f, 2.5f,
	                             NullEntity});

	const std::string saved = to_json_string(scene);
	EXPECT_NE(saved.find("\"cooldown_timer\""), std::string::npos);

	Scene loaded{"loaded"};
	std::string error;
	ASSERT_TRUE(scene_from_json(loaded, saved, &error)) << error;
	const Entity found = loaded.find("fighter");
	ASSERT_NE(found, NullEntity);

	const auto& combat = loaded.registry().get<rpg::Combat>(found);
	EXPECT_FLOAT_EQ(combat.cooldown_timer, 0.75f);
	EXPECT_FLOAT_EQ(combat.attack_cooldown, 1.25f);
	EXPECT_EQ(combat.damage, 12);
}

#include <gtest/gtest.h>

#include "lotus/rpg/components.hpp"
#include "lotus/rpg/systems.hpp"
#include "lotus/scene/scene.hpp"

using namespace lotus;
using namespace lotus::rpg;

TEST(RpgDialogueTest, UsesNpcDialogueLinesWhenPresent)
{
	Scene scene{"dialogue"};

	const Entity npc = scene.create("elder");
	scene.registry().emplace<NPC>(npc, NPC{"elder", "quest_giver", "village", true});
	auto& npc_dialogue = scene.registry().emplace<Dialogue>(npc);
	npc_dialogue.npc_name = "elder";
	npc_dialogue.lines      = {"Custom line one.", "Custom line two."};

	const Entity player = scene.create("player");
	scene.registry().emplace<Dialogue>(player);

	start_dialogue(scene, npc, player);

	const auto& player_dialogue = scene.registry().get<Dialogue>(player);
	EXPECT_TRUE(player_dialogue.is_active);
	EXPECT_EQ(player_dialogue.npc_name, "elder");
	EXPECT_EQ(player_dialogue.lines.size(), 2u);
	EXPECT_EQ(player_dialogue.lines[0], "Custom line one.");
	EXPECT_EQ(player_dialogue.lines[1], "Custom line two.");
}

TEST(RpgDialogueTest, FallsBackToGenericLinesWhenNpcDialogueEmpty)
{
	Scene scene{"dialogue"};

	const Entity npc = scene.create("stranger");
	scene.registry().emplace<NPC>(npc, NPC{"stranger", "villager", "village", true});
	scene.registry().emplace<Dialogue>(npc);

	const Entity player = scene.create("player");
	scene.registry().emplace<Dialogue>(player);

	start_dialogue(scene, npc, player);

	const auto& player_dialogue = scene.registry().get<Dialogue>(player);
	EXPECT_TRUE(player_dialogue.is_active);
	EXPECT_EQ(player_dialogue.npc_name, "stranger");
	EXPECT_EQ(player_dialogue.lines.size(), 2u);
	EXPECT_EQ(player_dialogue.lines[0], "Greetings, traveler.");
	EXPECT_EQ(player_dialogue.lines[1], "What brings you to these parts?");
}

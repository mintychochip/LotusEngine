#include <gtest/gtest.h>

#include "lotus/application/headless.hpp"
#include "lotus/console/console.hpp"
#include "lotus/scene/scene.hpp"
#include "lotus/scene/serialization.hpp"

using namespace lotus;

// split_console_commands is the shared LOTUS_CONSOLE / --exec splitter: the
// entity.* verbs run through console_.execute, which is only reachable via a
// full Application, so the units under test are the splitter, the --exec
// parse, and the entity_to_json helper that backs `entity.get`.

TEST(SplitConsoleCommands, SplitsOnSemicolons)
{
	const auto out = split_console_commands("entity.list; entity.get box");
	ASSERT_EQ(out.size(), 2u);
	EXPECT_EQ(out[0], "entity.list");
	EXPECT_EQ(out[1], "entity.get box");
}

TEST(SplitConsoleCommands, TrimsAndDropsEmpties)
{
	const auto out =
	        split_console_commands("  entity.spawn box2 ;; entity.list  ; ");
	ASSERT_EQ(out.size(), 2u);
	EXPECT_EQ(out[0], "entity.spawn box2");
	EXPECT_EQ(out[1], "entity.list");
}

TEST(SplitConsoleCommands, EmptyScriptYieldsNoCommands)
{
	EXPECT_TRUE(split_console_commands("").empty());
	EXPECT_TRUE(split_console_commands("  ; ; ").empty());
}

TEST(SplitConsoleCommands, SingleCommandPassesThrough)
{
	const auto out = split_console_commands("entity.list");
	ASSERT_EQ(out.size(), 1u);
	EXPECT_EQ(out[0], "entity.list");
}

TEST(SplitConsoleCommands, JsonWithSpacesSurvivesAsOneCommand)
{
	// Splitting is only on ';', so the JSON payload of an entity.set keeps
	// its spaces for the later whitespace tokenize + arg-join.
	const auto out = split_console_commands(
	        R"(entity.set box2 Transform {"position": [1, 2, 3]}; entity.get box2)");
	ASSERT_EQ(out.size(), 2u);
	EXPECT_EQ(out[0],
	          R"(entity.set box2 Transform {"position": [1, 2, 3]})");
	EXPECT_EQ(out[1], "entity.get box2");
}

TEST(EntityToJson, FreshEntityHasNameAndTransform)
{
	Scene scene{"test"};
	const Entity box = scene.create("box2");

	const nlohmann::json got = entity_to_json(scene.registry(), box);
	EXPECT_EQ(got["name"], "box2");
	ASSERT_TRUE(got.contains("components"));
	EXPECT_TRUE(got["components"].contains("Transform"));
}

TEST(EntityToJson, SetTransformRoundTrips)
{
	Scene scene{"test"};
	const Entity box = scene.create("box2");

	const auto* read =
	        default_component_codecs().reader("Transform");
	ASSERT_NE(read, nullptr);
	(*read)(scene.registry(), box,
	        nlohmann::json::parse(R"({"position": [1, 2, 3]})"));

	const nlohmann::json got = entity_to_json(scene.registry(), box);
	ASSERT_TRUE(got["components"].contains("Transform"));
	EXPECT_EQ(got["components"]["Transform"]["position"],
	          nlohmann::json::array({1, 2, 3}));
}

TEST(EntityToJson, UnknownComponentHasNoReader)
{
	EXPECT_EQ(default_component_codecs().reader("Frobnicate"), nullptr);
}

TEST(ParseHeadlessExec, ExecFlagParsedVerbatim)
{
	lotus::HeadlessOptions options;
	std::string error;
	EXPECT_TRUE(lotus::parse_headless_args(
	        {"--exec", "entity.spawn box2; entity.get box2"}, &options,
	        &error))
	        << error;
	EXPECT_EQ(options.exec, "entity.spawn box2; entity.get box2");
	EXPECT_EQ(split_console_commands(options.exec).size(), 2u);
}

TEST(ParseHeadlessExec, MissingExecValueFails)
{
	lotus::HeadlessOptions options;
	std::string error;
	EXPECT_FALSE(
	        lotus::parse_headless_args({"--exec"}, &options, &error));
}

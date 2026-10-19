#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lotus/application/headless.hpp"

namespace {

std::vector<std::string> args(std::initializer_list<const char*> parts)
{
	std::vector<std::string> out;
	for (const char* part : parts) {
		out.emplace_back(part);
	}
	return out;
}

} // namespace

TEST(ParseHeadlessArgs, EmptyArgsYieldDefaults)
{
	lotus::HeadlessOptions options;
	std::string error;
	EXPECT_TRUE(lotus::parse_headless_args(args({}), &options, &error))
	        << error;
	EXPECT_EQ(options.scene, "main");
	EXPECT_EQ(options.ticks, 600);
	EXPECT_TRUE(options.screenshot.empty());
	EXPECT_EQ(options.width, 1280u);
	EXPECT_EQ(options.height, 720u);
	EXPECT_EQ(options.seed, 42u);
	EXPECT_TRUE(options.dump_state.empty());
	EXPECT_EQ(options.scene_dir, "scenes");
	EXPECT_EQ(options.assets_dir, "assets");
}

TEST(ParseHeadlessArgs, FullFlagsParsed)
{
	lotus::HeadlessOptions options;
	std::string error;
	EXPECT_TRUE(lotus::parse_headless_args(
	        args({"--scene", "hub", "--ticks", "10", "--screenshot",
	              "/tmp/shot.ppm", "--width", "320", "--height", "200",
	              "--seed", "7", "--dump-state", "/tmp/dump.json",
	              "--scene-dir", "s", "--assets-dir", "a"}),
	        &options, &error))
	        << error;
	EXPECT_EQ(options.scene, "hub");
	EXPECT_EQ(options.ticks, 10);
	EXPECT_EQ(options.screenshot, "/tmp/shot.ppm");
	EXPECT_EQ(options.width, 320u);
	EXPECT_EQ(options.height, 200u);
	EXPECT_EQ(options.seed, 7u);
	EXPECT_EQ(options.dump_state, "/tmp/dump.json");
	EXPECT_EQ(options.scene_dir, "s");
	EXPECT_EQ(options.assets_dir, "a");
}

TEST(ParseHeadlessArgs, UnknownFlagFailsNamingTheFlag)
{
	lotus::HeadlessOptions options;
	std::string error;
	EXPECT_FALSE(lotus::parse_headless_args(args({"--frobnicate"}),
	                                        &options, &error));
	EXPECT_NE(error.find("--frobnicate"), std::string::npos);
}

TEST(ParseHeadlessArgs, NonNumericTicksFails)
{
	lotus::HeadlessOptions options;
	std::string error;
	EXPECT_FALSE(lotus::parse_headless_args(args({"--ticks", "abc"}),
	                                        &options, &error));
}

TEST(ParseHeadlessArgs, NegativeTicksFails)
{
	lotus::HeadlessOptions options;
	std::string error;
	EXPECT_FALSE(lotus::parse_headless_args(args({"--ticks", "-5"}),
	                                        &options, &error));
}

TEST(ParseHeadlessArgs, MissingValueFails)
{
	lotus::HeadlessOptions options;
	std::string error;
	EXPECT_FALSE(lotus::parse_headless_args(args({"--scene"}), &options,
	                                        &error));
}

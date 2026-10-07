#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "lotus/scene/assets.hpp"

namespace fs = std::filesystem;
using namespace lotus;

namespace {

std::string temp_path(const char* name)
{
	return (fs::temp_directory_path() / name).string();
}

void write_file(const std::string& path, const std::string& body)
{
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out << body;
}

} // namespace

TEST(AssetRegistryTest, ResolvePrefersRegistryOverFallback)
{
	AssetRegistry registry;
	registry.set("hero", "builtin:cube");
	EXPECT_EQ(registry.resolve("hero", "builtin:sphere"), "builtin:cube");
}

TEST(AssetRegistryTest, EmptyIdUsesFallback)
{
	AssetRegistry registry;
	registry.set("hero", "builtin:cube");
	EXPECT_EQ(registry.resolve("", "builtin:sphere"), "builtin:sphere");
}

TEST(AssetRegistryTest, UnknownIdFallsBackAndIsDangling)
{
	AssetRegistry registry;
	EXPECT_EQ(registry.resolve("missing", "fallback.glb"), "fallback.glb");
	EXPECT_TRUE(registry.dangling("missing"));
	EXPECT_FALSE(registry.dangling(""));
	EXPECT_TRUE(registry.dangling("known"));
	registry.set("known", "a.glb");
	EXPECT_FALSE(registry.dangling("known"));
}

TEST(AssetRegistryTest, RoundTripSaveLoad)
{
	const std::string path = temp_path("lotus_assets_roundtrip.json");
	AssetRegistry registry;
	registry.set("cube", "assets/cube.glb");
	registry.set("ball", "builtin:sphere");
	std::string error;
	ASSERT_TRUE(registry.save(path, &error)) << error;

	AssetRegistry loaded;
	ASSERT_TRUE(loaded.load(path, &error)) << error;
	EXPECT_EQ(loaded.size(), 2u);
	EXPECT_EQ(loaded.resolve("cube", ""), "assets/cube.glb");
	EXPECT_EQ(loaded.resolve("ball", ""), "builtin:sphere");
}

TEST(AssetRegistryTest, MalformedLoadLeavesTableIntact)
{
	const std::string path = temp_path("lotus_assets_bad.json");
	write_file(path, "{ not json");
	AssetRegistry registry;
	registry.set("keep", "path.glb");
	std::string error;
	EXPECT_FALSE(registry.load(path, &error));
	EXPECT_FALSE(error.empty());
	EXPECT_EQ(registry.size(), 1u);
	EXPECT_EQ(registry.resolve("keep", ""), "path.glb");
}

TEST(AssetRegistryTest, NamesSorted)
{
	AssetRegistry registry;
	registry.set("z", "z");
	registry.set("a", "a");
	registry.set("m", "m");
	const std::vector<std::string> names = registry.names();
	ASSERT_EQ(names.size(), 3u);
	EXPECT_EQ(names[0], "a");
	EXPECT_EQ(names[1], "m");
	EXPECT_EQ(names[2], "z");
}

TEST(AssetRegistryTest, EraseRemovesKey)
{
	AssetRegistry registry;
	registry.set("gone", "x");
	EXPECT_TRUE(registry.erase("gone"));
	EXPECT_FALSE(registry.find("gone"));
	EXPECT_FALSE(registry.erase("gone"));
}

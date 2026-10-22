#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "lotus/scene/scene.hpp"
#include "lotus/script/script.hpp"
#include "lotus/script/script_engine.hpp"

using namespace lotus;
namespace fs = std::filesystem;

namespace {

fs::path write_script(const std::string& name, const std::string& body)
{
	const fs::path path = fs::temp_directory_path() / name;
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out << body;
	return path;
}

void make_newer(const fs::path& path)
{
	fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(5));
}

} // namespace

TEST(ScriptEngineTest, UpdateMutatesComponents)
{
	Scene scene{"s"};
	const Entity e = scene.create("e");
	scene.registry().emplace<Script>(e, Script{"rotate.lua", ""});
	write_script("rotate.lua", R"(
		local s = {}
		function s.on_update(entity, dt)
			lotus.rotate(entity, dt * 2.0)
		end
		return s
	)");

	ScriptEngine engine;
	engine.set_root(fs::temp_directory_path().string());
	std::string error;
	ASSERT_TRUE(engine.sync(scene, &error)) << error;

	engine.update(scene, 0.5f);
	EXPECT_FLOAT_EQ(scene.registry().get<Transform>(e).rotation.z, 1.0f);
	engine.update(scene, 0.5f);
	EXPECT_FLOAT_EQ(scene.registry().get<Transform>(e).rotation.z, 2.0f);
}

TEST(ScriptEngineTest, ScriptCanSetFullRotation)
{
	Scene scene{"s"};
	const Entity e = scene.create("e");
	scene.registry().emplace<Script>(e, Script{"set_rotation.lua", ""});
	write_script("set_rotation.lua", R"(
		local s = {}
		function s.on_update(entity, dt)
			lotus.set_rotation(entity, 0.25, 0.5, -0.75)
		end
		return s
	)");

	ScriptEngine engine;
	engine.set_root(fs::temp_directory_path().string());
	ASSERT_TRUE(engine.sync(scene));
	engine.update(scene, 0.1f);
	const auto& rotation = scene.registry().get<Transform>(e).rotation;
	EXPECT_FLOAT_EQ(rotation.x, 0.25f);
	EXPECT_FLOAT_EQ(rotation.y, 0.5f);
	EXPECT_FLOAT_EQ(rotation.z, -0.75f);
}

TEST(ScriptEngineTest, OnStartRunsOnSync)
{
	Scene scene{"s"};
	const Entity e = scene.create("e");
	scene.registry().emplace<Script>(e, Script{"start.lua", ""});
	write_script("start.lua", R"(
		local s = {}
		function s.on_start(entity)
			lotus.set_position(entity, 9.0, 8.0, 0.0)
		end
		return s
	)");

	ScriptEngine engine;
	engine.set_root(fs::temp_directory_path().string());
	ASSERT_TRUE(engine.sync(scene));

	const auto& position = scene.registry().get<Transform>(e).position;
	EXPECT_FLOAT_EQ(position.x, 9.0f);
	EXPECT_FLOAT_EQ(position.y, 8.0f);
}

TEST(ScriptEngineTest, ScriptCanFindAndMoveOtherEntities)
{
	Scene scene{"s"};
	const Entity target = scene.create("target");
	const Entity driver = scene.create("driver");
	scene.registry().emplace<Script>(driver, Script{"driver.lua", ""});
	write_script("driver.lua", R"(
		local s = {}
		function s.on_update(entity, dt)
			local t = lotus.find("target")
			if t then
				lotus.set_position(t, 5.0, 6.0, 0.0)
				lotus.set_color(t, 1.0, 0.0, 0.0)
			end
		end
		return s
	)");
	scene.registry().emplace<Sprite>(target, Sprite{});

	ScriptEngine engine;
	engine.set_root(fs::temp_directory_path().string());
	ASSERT_TRUE(engine.sync(scene));
	engine.update(scene, 0.1f);

	EXPECT_FLOAT_EQ(scene.registry().get<Transform>(target).position.x, 5.0f);
	EXPECT_FLOAT_EQ(scene.registry().get<Sprite>(target).color.r, 1.0f);
}

TEST(ScriptEngineTest, BrokenScriptIsReportedAndNotLoaded)
{
	Scene scene{"s"};
	const Entity e = scene.create("e");
	scene.registry().emplace<Script>(e, Script{"broken.lua", ""});
	write_script("broken.lua", "this is not lua !!!");

	ScriptEngine engine;
	engine.set_root(fs::temp_directory_path().string());
	std::string error;
	EXPECT_FALSE(engine.sync(scene, &error));
	EXPECT_FALSE(error.empty());
	EXPECT_FALSE(engine.is_loaded("broken.lua"));

	engine.update(scene, 0.1f); // must not crash
}

TEST(ScriptEngineTest, RuntimeErrorIsCaughtAndReported)
{
	Scene scene{"s"};
	const Entity e = scene.create("e");
	scene.registry().emplace<Script>(e, Script{"boom.lua", ""});
	write_script("boom.lua", R"(
		local s = {}
		function s.on_update(entity, dt)
			error("intentional failure")
		end
		return s
	)");

	ScriptEngine engine;
	engine.set_root(fs::temp_directory_path().string());
	ASSERT_TRUE(engine.sync(scene));
	engine.update(scene, 0.1f);
	EXPECT_FALSE(engine.last_error().empty());
}

TEST(ScriptEngineTest, HotSwapReplacesBehaviour)
{
	Scene scene{"s"};
	const Entity e = scene.create("e");
	scene.registry().emplace<Script>(e, Script{"swap.lua", ""});
	const fs::path path = write_script("swap.lua", R"(
		local s = {}
		function s.on_update(entity, dt)
			lotus.rotate(entity, 1.0)
		end
		return s
	)");

	ScriptEngine engine;
	engine.set_root(fs::temp_directory_path().string());
	ASSERT_TRUE(engine.sync(scene));
	engine.update(scene, 0.0f);
	EXPECT_FLOAT_EQ(scene.registry().get<Transform>(e).rotation.z, 1.0f);

	// Replace the file with different behaviour and let the watcher see it.
	write_script("swap.lua", R"(
		local s = {}
		function s.on_update(entity, dt)
			lotus.rotate(entity, 10.0)
		end
		return s
	)");
	make_newer(path);
	engine.poll(scene);

	engine.update(scene, 0.0f);
	EXPECT_FLOAT_EQ(scene.registry().get<Transform>(e).rotation.z, 11.0f);
}

TEST(ScriptEngineTest, BrokenHotSwapKeepsPreviousVersion)
{
	Scene scene{"s"};
	const Entity e = scene.create("e");
	scene.registry().emplace<Script>(e, Script{"keep.lua", ""});
	const fs::path path = write_script("keep.lua", R"(
		local s = {}
		function s.on_update(entity, dt)
			lotus.rotate(entity, 1.0)
		end
		return s
	)");

	ScriptEngine engine;
	engine.set_root(fs::temp_directory_path().string());
	ASSERT_TRUE(engine.sync(scene));
	engine.update(scene, 0.0f);
	EXPECT_FLOAT_EQ(scene.registry().get<Transform>(e).rotation.z, 1.0f);

	write_script("keep.lua", "totally broken (");
	make_newer(path);
	engine.poll(scene);
	EXPECT_FALSE(engine.last_error().empty());

	// The previously loaded script is still in effect.
	engine.update(scene, 0.0f);
	EXPECT_FLOAT_EQ(scene.registry().get<Transform>(e).rotation.z, 2.0f);
}

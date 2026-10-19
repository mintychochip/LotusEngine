#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "lotus/application.hpp"
#include "lotus/gpu/renderer.hpp"
#include "lotus/scene/scene_pass.hpp"
#include "lotus/script/script.hpp"

using namespace lotus;
namespace fs = std::filesystem;

namespace {

class HeadlessApp : public Application {
public:
	using Application::Application;

protected:
	void on_ready(Window&, GpuRenderer&) override {}
	void on_update(float) override {}
	void on_shutdown() override {}
};

void write_file(const fs::path& path, const std::string& text)
{
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out << text;
}

} // namespace

TEST(ScenePassTest, EditorViewportIsTheOnlySceneDraw)
{
	EXPECT_EQ(select_scene_draw_target(true, 640, 360), SceneDrawTarget::Viewport);
	EXPECT_EQ(select_scene_draw_target(true, 0, 360), SceneDrawTarget::Window);
	EXPECT_EQ(select_scene_draw_target(false, 640, 360), SceneDrawTarget::Window);
}

TEST(FrameScheduleTest, ReloadRebindsAssetsAndDropsPhysicsBodies)
{
	const fs::path dir = fs::temp_directory_path() / "lotus_frame_reload";
	fs::remove_all(dir);
	fs::create_directories(dir / "scenes");

	const std::string scene = R"({
		"version": 3,
		"name": "hub",
		"entities": [
			{"name": "floor", "components": {
				"RigidBody": {"motion_type": "static"},
				"BoxCollider": {"half_extents": [20, 0.5, 20]},
				"Transform": {"position": [0, -0.5, 0]}
			}},
			{"name": "ball", "components": {
				"RigidBody": {"motion_type": "dynamic", "mass": 1},
				"SphereCollider": {"radius": 0.5},
				"Transform": {"position": [0, 10, 0]}
			}}
		]
	})";
	write_file(dir / "scenes" / "hub.json", scene);

	HeadlessApp app;
	app.set_scene_dir((dir / "scenes").string());
	app.set_assets_dir(dir.string());
	std::string error;
	ASSERT_TRUE(app.load_scene_by_name("hub", &error)) << error;
	Scene* loaded = app.scenes().active();
	ASSERT_NE(loaded, nullptr);
	EXPECT_EQ(loaded->assets(), &app.assets());

	loaded->update(0.0f);
	for (int i = 0; i < 90; ++i) {
		app.physics().step(*loaded, 1.0f / 60.0f);
	}

	ASSERT_TRUE(app.reload_scene_now(&error)) << error;
	Scene* again = app.scenes().active();
	ASSERT_NE(again, nullptr);
	EXPECT_EQ(again->assets(), &app.assets());

	again->update(0.0f);
	const Entity ball = again->find("ball");
	ASSERT_NE(ball, NullEntity);
	app.physics().step(*again, 1.0f / 60.0f);
	EXPECT_GT(again->registry().get<Transform>(ball).position.y, 9.0f);

	fs::remove_all(dir);
}

TEST(FrameScheduleTest, ScriptPoseIsInTheWorldMatrixThisFrame)
{
	const fs::path dir = fs::temp_directory_path() / "lotus_frame_script";
	fs::remove_all(dir);
	fs::create_directories(dir);
	{
		std::ofstream out(dir / "move.lua");
		out << R"(
			local s = {}
			function s.on_update(entity, dt)
				lotus.set_position(entity, 4.0, 0.0, 0.0)
			end
			return s
		)";
	}

	HeadlessApp app;
	app.scripts().set_root(dir.string());
	Scene& scene = app.scenes().activate("live");
	const Entity prop = scene.create("prop");
	scene.registry().emplace<Script>(prop, Script{"move.lua", ""});
	std::string error;
	ASSERT_TRUE(app.scripts().sync(scene, &error)) << error;

	app.tick_simulation(0.016f, false);

	const glm::mat4& world = scene.registry().get<Transform>(prop).world;
	EXPECT_NEAR(world[3].x, 4.0f, 1e-4f);

	fs::remove_all(dir);
}

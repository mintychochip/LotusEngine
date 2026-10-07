#include "lotus/application.hpp"
#include "lotus/gpu/renderer.hpp"
#include "lotus/scene/scene.hpp"
#include "lotus/scene/serialization.hpp"

#include <filesystem>
#include <iostream>

class DemoApplication : public lotus::Application {
public:
	using lotus::Application::Application;

protected:
	void on_ready(lotus::Window& /*window*/, lotus::GpuRenderer& gpu) override
	{
		// Directional light + ambient, applied per fragment.
		gpu.lighting().direction = {0.5f, 0.9f, 0.4f};
		gpu.lighting().intensity = 1.0f;
		gpu.lighting().ambient   = {0.12f, 0.13f, 0.17f};

		lotus::Scene& scene = scenes().activate("main");

		const lotus::Entity cube = scene.create("cube");
		// A real binary glTF file, loaded by the engine's GLB parser.
		scene.registry().emplace<lotus::MeshSource>(
		        cube, lotus::MeshSource{"../assets/cube.glb"});
		scene.registry().emplace<lotus::Material>(
		        cube, lotus::Material{{0.85f, 0.34f, 0.28f}});
		scene.registry().get<lotus::Transform>(cube).position = {-1.1f, 0.0f, 0.0f};

		const lotus::Entity ball = scene.create("ball");
		scene.registry().emplace<lotus::MeshSource>(
		        ball, lotus::MeshSource{"builtin:sphere"});
		scene.registry().emplace<lotus::Material>(
		        ball, lotus::Material{{0.28f, 0.52f, 0.90f}});
		scene.registry().get<lotus::Transform>(ball).position = {1.1f, 0.0f, 0.0f};

		// Level of detail: three tessellations of the same sphere, chosen by
		// distance from the camera (and culled past the last one).
		const lotus::Entity detailed = scene.create("lod_sphere");
		lotus::Lod lod;
		lod.levels.push_back(lotus::LodLevel{"builtin:sphere:32:24", 4.0f});
		lod.levels.push_back(lotus::LodLevel{"builtin:sphere:16:12", 9.0f});
		lod.levels.push_back(lotus::LodLevel{"builtin:sphere:8:6", 16.0f});
		scene.registry().emplace<lotus::Lod>(detailed, std::move(lod));
		scene.registry().emplace<lotus::Material>(
		        detailed, lotus::Material{{0.35f, 0.80f, 0.45f}});
		scene.registry().get<lotus::Transform>(detailed).position = {0.0f, 0.0f, -1.2f};

		// Occlusion: a panel between the camera and the LOD sphere. Its centre
		// sits on the camera->sphere ray at z = 2.2 (the sphere is at z = -1.2,
		// the default camera at {3, 2.5, 4}), and it is sized to cover the
		// sphere's whole screen footprint, so once the Hi-Z pyramid holds a
		// frame of depth the sphere is culled as hidden. The cube and ball sit
		// outside that footprint and stay visible.
		const lotus::Entity wall = scene.create("wall");
		scene.registry().emplace<lotus::MeshSource>(
		        wall, lotus::MeshSource{"builtin:cube"});
		scene.registry().emplace<lotus::Material>(
		        wall, lotus::Material{{0.55f, 0.55f, 0.58f}});
		scene.registry().get<lotus::Transform>(wall).scale = {1.5f, 1.7f, 0.25f};
		scene.registry().get<lotus::Transform>(wall).position = {1.96f, 1.63f, 2.2f};

		// Behind the camera: frustum-culled every frame.
		const lotus::Entity hidden = scene.create("behind_camera");
		scene.registry().emplace<lotus::MeshSource>(
		        hidden, lotus::MeshSource{"builtin:cube"});
		scene.registry().emplace<lotus::Material>(
		        hidden, lotus::Material{{1.0f, 0.0f, 1.0f}});
		scene.registry().get<lotus::Transform>(hidden).position = {0.0f, 0.0f, 40.0f};

		const lotus::Entity camera = scene.create("camera");
		scene.registry().emplace<lotus::Camera3D>(camera);

		// Persist and watch the scene so file edits hot-reload.
		std::error_code error;
		std::filesystem::create_directories("scenes", error);
		const std::string path = "scenes/main.json";
		std::string save_error;
		if (lotus::save_scene(scene, path, &save_error)) {
			watch_scene(path);
		} else {
			std::cerr << "could not write " << path << ": " << save_error << '\n';
		}
	}

	void on_update(float dt) override
	{
		lotus::Scene* scene = scenes().active();
		if (!scene) {
			return;
		}
		// Look entities up by name: a hot reload replaces the registry.
		if (const lotus::Entity cube = scene->find("cube");
		    cube != lotus::NullEntity) {
			scene->registry().get<lotus::Transform>(cube).rotation.y += dt * 0.7f;
		}
		if (const lotus::Entity ball = scene->find("ball");
		    ball != lotus::NullEntity) {
			scene->registry().get<lotus::Transform>(ball).rotation.x += dt * 0.4f;
		}
	}

	void on_shutdown() override {}
};

int main()
{
	try {
		DemoApplication app;
		app.run(1100, 700, "lotus");
	} catch (const std::exception& ex) {
		std::cerr << "lotus: " << ex.what() << '\n';
		return 1;
	}
	return 0;
}

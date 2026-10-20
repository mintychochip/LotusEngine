#include "lotus/application.hpp"
#include "lotus/application/headless.hpp"
#include "lotus/ecs.hpp"
#include "lotus/gpu/renderer.hpp"
#include "lotus/scene/scene.hpp"
#include "lotus/scene/serialization.hpp"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace lotus {

namespace {

bool need_value(const std::vector<std::string>& args, std::size_t& i,
                const std::string& flag, std::string& out, std::string* error)
{
	if (i + 1 >= args.size()) {
		if (error) {
			*error = "flag '" + flag + "' needs a value";
		}
		return false;
	}
	out = args[++i];
	return true;
}
bool parse_ulong(const std::string& text, const std::string& flag,
                 unsigned long& out, std::string* error)
{
	try {
	if (!text.empty() && text[0] == '-') {
		throw std::invalid_argument("negative value");
	}
	std::size_t used = 0;
		out              = std::stoul(text, &used);
		if (used != text.size()) {
			throw std::invalid_argument("trailing characters");
		}
		return true;
	} catch (const std::exception&) {
		if (error) {
			*error = "flag '" + flag + "' needs a non-negative integer, got '" +
			         text + "'";
		}
		return false;
	}
}

bool capture_screenshot(Scene& scene, const HeadlessOptions& options,
                        std::string* error)
{
	// A headless Vulkan device still needs SDL's video subsystem. The
	// offscreen driver needs no display; software Vulkan (lavapipe) is
	// enough, which is what makes this work in containers.
	SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		if (error) {
			*error = std::string{"SDL video init failed: "} + SDL_GetError();
		}
		return false;
	}
	bool ok = false;
	{
		try {
			GpuRenderer gpu;
			gpu.init_offscreen(options.width, options.height);
			if (scene.has_settings()) {
				const SceneSettings& s = scene.settings();
				gpu.lighting().direction = s.light_direction;
				gpu.lighting().color     = s.light_color;
				gpu.lighting().intensity = s.light_intensity;
				gpu.lighting().ambient   = s.ambient;
				gpu.lighting().exposure = s.exposure;
				gpu.lighting().sky_color       = s.sky_color;
				gpu.lighting().ground_color    = s.ground_color;
				gpu.lighting().shadows_enabled = s.shadows_enabled;
				gpu.lighting().fxaa_enabled    = s.fxaa_enabled;
				gpu.lighting().sky_enabled     = s.sky_enabled;
				gpu.lighting().horizon_warmth  = s.horizon_warmth;
				gpu.lighting().fog_enabled      = s.fog_enabled;
				gpu.lighting().fog_density      = s.fog_density;
				gpu.lighting().bounce_enabled   = s.bounce_enabled;
				gpu.lighting().bounce_color     = s.bounce_color;
				gpu.lighting().bounce_intensity = s.bounce_intensity;
				gpu.lighting().ssao_enabled     = s.ssao_enabled;
				gpu.lighting().environment_intensity =
				        s.environment_intensity;
			}
			const float aspect =
			        static_cast<float>(options.width) /
			        static_cast<float>(options.height);
			gpu.begin_frame_offscreen();
			scene.upload(gpu, aspect);
			gpu.begin_scene_pass();
			scene.draw(gpu);
			gpu.end_scene_pass();
			gpu.end_frame();
			SDL_WaitForGPUIdle(gpu.device());
			ok = gpu.write_ppm(options.screenshot, error);
		} catch (const std::exception& ex) {
			if (error) {
				*error = ex.what();
			}
			ok = false;
		}
	} // GPU device destroyed before SDL_Quit()
	SDL_Quit();
	return ok;
}

} // namespace

bool parse_headless_args(const std::vector<std::string>& args,
                         HeadlessOptions* options, std::string* error)
{
	if (options == nullptr) {
		return false;
	}
	HeadlessOptions parsed;
	for (std::size_t i = 0; i < args.size(); ++i) {
		const std::string& arg = args[i];
		std::string value;
		unsigned long number = 0;
		if (arg == "--scene") {
			if (!need_value(args, i, arg, value, error)) {
				return false;
			}
			parsed.scene = value;
		} else if (arg == "--ticks") {
			if (!need_value(args, i, arg, value, error) ||
			    !parse_ulong(value, arg, number, error)) {
				return false;
			}
			parsed.ticks = static_cast<int>(number);
		} else if (arg == "--screenshot") {
			if (!need_value(args, i, arg, value, error)) {
				return false;
			}
			parsed.screenshot = value;
		} else if (arg == "--width") {
			if (!need_value(args, i, arg, value, error) ||
			    !parse_ulong(value, arg, number, error) || number == 0) {
				if (error && number == 0 && !value.empty()) {
					*error = "flag '" + arg +
					         "' needs a positive integer, got '" + value +
					         "'";
				}
				return false;
			}
			parsed.width = static_cast<uint32_t>(number);
		} else if (arg == "--height") {
			if (!need_value(args, i, arg, value, error) ||
			    !parse_ulong(value, arg, number, error) || number == 0) {
				if (error && number == 0 && !value.empty()) {
					*error = "flag '" + arg +
					         "' needs a positive integer, got '" + value +
					         "'";
				}
				return false;
			}
			parsed.height = static_cast<uint32_t>(number);
		} else if (arg == "--seed") {
			if (!need_value(args, i, arg, value, error) ||
			    !parse_ulong(value, arg, number, error)) {
				return false;
			}
			parsed.seed = static_cast<unsigned>(number);
		} else if (arg == "--dump-state") {
			if (!need_value(args, i, arg, value, error)) {
				return false;
			}
			parsed.dump_state = value;
		} else if (arg == "--assets-dir") {
			if (!need_value(args, i, arg, value, error)) {
				return false;
			}
			parsed.assets_dir = value;
		} else if (arg == "--scene-dir") {
			if (!need_value(args, i, arg, value, error)) {
				return false;
			}
			parsed.scene_dir = value;
		} else if (arg == "--exec") {
			if (!need_value(args, i, arg, value, error)) {
				return false;
			}
			parsed.exec = value;
		} else {
			if (error) {
				*error = arg.rfind("--", 0) == 0
				        ? "unknown flag '" + arg + "'"
				        : "unexpected argument '" + arg + "'";
			}
			return false;
		}
	}
	*options = parsed;
	return true;
}

int Application::run_headless(const HeadlessOptions& options)
{
	console_.set_sink([](const ConsoleEntry& entry) {
		std::cout << entry.text << '\n';
	});
	register_commands();
	set_scene_dir(options.scene_dir);
	set_assets_dir(options.assets_dir);
	residency_.set_scene_directory(scene_dir_);
	{
		const std::string regions_path = scene_dir_ + "/regions.json";
		std::error_code exists;
		if (std::filesystem::exists(regions_path, exists)) {
			std::string region_error;
			if (!residency_.load_catalog(regions_path, &region_error)) {
				console_.warn("regions: " + region_error);
			}
		}
	}
	{
		std::error_code assets_dir_error;
		std::filesystem::create_directories(assets_dir_, assets_dir_error);
		std::string assets_error;
		if (!load_assets_now(&assets_error)) {
			console_.warn("assets: " + assets_error);
		}
	}

	std::string error;
	if (!load_scene_by_name(options.scene, &error)) {
		console_.error("headless: cannot load scene '" + options.scene +
		               "': " + error);
		return 1;
	}
	scripts_.set_log([this](const std::string& message) {
		console_.info(message);
	});
	if (Scene* scene = scenes_.active()) {
		if (!scripts_.sync(*scene, &error)) {
			console_.error("headless: script load: " + error);
			return 1;
		}
	}
	if (!options.exec.empty()) {
		for (const std::string& command :
		     split_console_commands(options.exec)) {
			const std::string result = console_.execute(command);
			if (result.rfind("error: ", 0) == 0) {
				console_.error("headless: exec '" + command +
				               "' failed: " + result);
				return 1;
			}
		}
	}
	std::srand(options.seed);
	constexpr float kDt = 1.0f / 60.0f;
	for (int tick = 0; tick < options.ticks; ++tick) {
		on_update(kDt);
		tick_simulation(kDt, false);
	}

	Scene* scene = scenes_.active();
	if (scene == nullptr) {
		console_.error("headless: no active scene after load");
		return 1;
	}
	if (!options.dump_state.empty()) {
		if (!save_scene(*scene, options.dump_state, &error)) {
			console_.error("headless: cannot dump state: " + error);
			return 1;
		}
	}
	if (!options.screenshot.empty()) {
		if (!capture_screenshot(*scene, options, &error)) {
			console_.error("headless: cannot capture screenshot: " + error);
			return 1;
		}
	}
	const std::size_t entities =
	        scene->registry().storage<Entity>().size();
	on_shutdown();
	console_.info("headless: scene=" + options.scene +
	              " ticks=" + std::to_string(options.ticks) +
	              " entities=" + std::to_string(entities) + " result=ok");
	return 0;
}

} // namespace lotus

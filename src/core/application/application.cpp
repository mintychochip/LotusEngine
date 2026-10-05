#include "lotus/application.hpp"
#include "lotus/global.hpp"
#include "lotus/gpu/renderer.hpp"
#include "lotus/scene/scene.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

using namespace lotus;

void Application::on_start()
{
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		throw std::runtime_error(SDL_GetError());
	}
	Global::flag(LOTUS_SDL_INITIALIZED, true);
}

void Application::run(int width, int height, const std::string& title)
{
	on_start();
	{
		Window window{width, height, title};
		GpuRenderer gpu;
		gpu.init(window.handle(), settings_.debug_device);
		editor_.init(window.handle(), gpu);
		if (const char* no_ui = std::getenv("LOTUS_NO_UI")) {
			if (std::atoi(no_ui) != 0) {
				editor_.set_visible(false);
			}
		}

		on_ready(window, gpu);

		int max_frames = 0;
		if (const char* env = std::getenv("LOTUS_MAX_FRAMES")) {
			max_frames = std::atoi(env);
		}
		int frame = 0;

		Uint64 last_counter = SDL_GetPerformanceCounter();
		const double freq   = static_cast<double>(SDL_GetPerformanceFrequency());

		while (!window.should_close()) {
			window.poll_events([this](const SDL_Event& event) {
				editor_.process_event(event);
			});

			const Uint64 now = SDL_GetPerformanceCounter();
			const float dt =
			        static_cast<float>(static_cast<double>(now - last_counter) /
			                           freq);
			last_counter = now;

			on_update(dt);
			if (Scene* scene = scenes_.active()) {
				scene->update(dt);
			}

			int pixel_w = width, pixel_h = height;
			window.pixel_size(pixel_w, pixel_h);
			const float aspect =
			        pixel_h > 0 ? static_cast<float>(pixel_w) / pixel_h : 1.0f;

			gpu.begin_frame(window.handle(), 0.05f, 0.05f, 0.08f);
			if (gpu.has_frame()) {
				if (Scene* scene = scenes_.active()) {
					scene->upload(gpu);
					gpu.begin_scene_pass();
					scene->draw(gpu, aspect);
					gpu.end_scene_pass();
				} else {
					gpu.begin_scene_pass();
					gpu.end_scene_pass();
				}

				editor_.begin_frame();
				editor_.draw(scenes_, dt);
				editor_.render(gpu);

				gpu.end_frame();
			} else {
				// Swapchain not ready: back off instead of spinning.
				SDL_Delay(1);
			}

			if (max_frames > 0 && ++frame >= max_frames) {
				break;
			}
		}

		editor_.shutdown();
		on_shutdown();
		std::cout << "frames rendered: " << gpu.frames_rendered() << '\n';
	} // GPU + window must be destroyed before SDL_Quit()

	SDL_Quit();
	Global::flag(LOTUS_SDL_INITIALIZED, false);
}

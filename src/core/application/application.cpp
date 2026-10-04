
#include "lotus/application.hpp"
#include "lotus/global.hpp"
#include "lotus/gpu/renderer.hpp"
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
	Window window{width, height, title};
	GpuRenderer gpu;
	gpu.init(window.handle(), settings_.debug_device);

	on_ready(window, gpu);

	Uint64 last_counter = SDL_GetPerformanceCounter();
	const double freq   = static_cast<double>(SDL_GetPerformanceFrequency());

	while (!window.should_close()) {
		window.poll_events();

		const Uint64 now = SDL_GetPerformanceCounter();
		const float dt =
		        static_cast<float>(static_cast<double>(now - last_counter) / freq);
		last_counter = now;

		on_update(dt);
		on_prepare_frame(gpu);
		gpu.begin_frame(window.handle(), 0.0f, 0.0f, 0.5f);
		on_render(gpu);
		gpu.end_frame();
	}

	on_shutdown();
	SDL_Quit();
	Global::flag(LOTUS_SDL_INITIALIZED, false);
}

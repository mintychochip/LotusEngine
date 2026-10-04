
#include "lotus/application.hpp"
#include "lotus/global.hpp"
#include <stdexcept>

using namespace lotus;

void GLApplication::on_start()
{
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		throw std::runtime_error(SDL_GetError());
	}
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, profile_.major);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, profile_.minor);
	if (profile_.core_profile) {
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
		                    SDL_GL_CONTEXT_PROFILE_CORE);
	}
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	Global::flag(LOTUS_SDL_INITIALIZED, true);
}

void GLApplication::run(int width, int height, const std::string& title)
{
	on_start();
	GLWindow window{width, height, title};
	if (!window.set_current()) {
		on_shutdown();
		SDL_Quit();
		Global::flag(LOTUS_SDL_INITIALIZED, false);
		throw std::runtime_error("failed to create OpenGL context");
	}

	on_ready(window);

	Uint64 last_counter = SDL_GetPerformanceCounter();
	const double freq   = static_cast<double>(SDL_GetPerformanceFrequency());

	while (!window.should_close()) {
		window.poll_events();

		const Uint64 now = SDL_GetPerformanceCounter();
		const float dt =
		        static_cast<float>(static_cast<double>(now - last_counter) / freq);
		last_counter = now;

		on_update(dt);
		on_render();
		window.swap_buffers();
	}

	on_shutdown();
	SDL_Quit();
	Global::flag(LOTUS_SDL_INITIALIZED, false);
}

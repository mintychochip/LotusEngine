#include "lotus/audio/audio_system.hpp"

#include <SDL3/SDL.h>

namespace lotus {

bool AudioSystem::init(std::string* error)
{
	if (ready_) {
		return true;
	}
	if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
		if (error) {
			*error = SDL_GetError();
		}
		return false;
	}
	ready_ = true;
	return true;
}

void AudioSystem::shutdown()
{
	if (!ready_) {
		return;
	}
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
	ready_ = false;
}

} // namespace lotus

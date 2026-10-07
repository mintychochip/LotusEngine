#pragma once

#include <string>

namespace lotus {

/// Placeholder for game audio. SDL3 audio device open/close only; sample
/// playback comes in a later pass.
class AudioSystem {
public:
	bool init(std::string* error = nullptr);
	void shutdown();
	bool ready() const { return ready_; }

private:
	bool ready_{false};
};

} // namespace lotus

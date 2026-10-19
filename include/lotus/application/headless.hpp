#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lotus {

/// Options for Application::run_headless: load a scene, step fixed-dt ticks,
/// optionally dump the scene JSON and capture a screenshot, then exit.
/// No window, editor, audio, or hot-reload. This is the agent verification
/// loop (`lotus --headless ...`).
struct HeadlessOptions {
	std::string scene{"main"};
	int ticks{600};
	/// Screenshot path (binary PPM). Empty skips all GPU work.
	std::string screenshot;
	uint32_t width{1280};
	uint32_t height{720};
	/// Seeds std::rand, which is also what Lua's math.random draws from.
	unsigned seed{42};
	/// Scene JSON dump path. Empty skips.
	std::string dump_state;
	/// `;`-separated console commands (same splitting as LOTUS_CONSOLE).
	/// Runs after scene load + script sync, before ticks. The first result
	/// starting with `error: ` aborts the run with exit 1. Empty skips.
	std::string exec;
	std::string scene_dir{"scenes"};
	std::string assets_dir{"assets"};
};

/// Parses headless flags (without the leading `--headless`). Returns false
/// and fills \p error on unknown flags, missing values, or bad numbers.
bool parse_headless_args(const std::vector<std::string>& args,
                         HeadlessOptions* options, std::string* error);

} // namespace lotus

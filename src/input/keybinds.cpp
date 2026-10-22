#include "lotus/input/keybinds.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>

namespace lotus {
namespace {

constexpr SDL_Keymod kRelevantMods =
        SDL_KMOD_CTRL | SDL_KMOD_SHIFT | SDL_KMOD_ALT | SDL_KMOD_GUI;

std::string lower(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return text;
}

std::vector<std::string> split(const std::string& text, char separator)
{
	std::vector<std::string> parts;
	std::istringstream stream{text};
	std::string part;
	while (std::getline(stream, part, separator)) {
		parts.push_back(part);
	}
	return parts;
}

} // namespace

bool KeyChord::matches(const SDL_Event& event) const
{
	if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) {
		return false;
	}
	if (event.key.key != key) {
		return false;
	}
	return (event.key.mod & kRelevantMods) == (mods & kRelevantMods);
}

std::string KeyChord::to_string() const
{
	std::string out;
	if (mods & SDL_KMOD_CTRL) {
		out += "Ctrl+";
	}
	if (mods & SDL_KMOD_SHIFT) {
		out += "Shift+";
	}
	if (mods & SDL_KMOD_ALT) {
		out += "Alt+";
	}
	if (mods & SDL_KMOD_GUI) {
		out += "Gui+";
	}
	const char* name = SDL_GetKeyName(key);
	out += (name && *name) ? name : "?";
	return out;
}

std::optional<KeyChord> parse_key_chord(const std::string& text)
{
	const std::vector<std::string> parts = split(text, '+');
	if (parts.empty() || parts.back().empty()) {
		return std::nullopt;
	}

	KeyChord chord;
	for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
		const std::string modifier = lower(parts[i]);
		if (modifier == "ctrl" || modifier == "control") {
			chord.mods |= SDL_KMOD_CTRL;
		} else if (modifier == "shift") {
			chord.mods |= SDL_KMOD_SHIFT;
		} else if (modifier == "alt") {
			chord.mods |= SDL_KMOD_ALT;
		} else if (modifier == "gui" || modifier == "super" || modifier == "cmd") {
			chord.mods |= SDL_KMOD_GUI;
		} else {
			return std::nullopt;
		}
	}

	chord.key = SDL_GetKeyFromName(parts.back().c_str());
	if (chord.key == SDLK_UNKNOWN) {
		return std::nullopt;
	}
	return chord;
}

void Keybinds::define(std::string action, KeyChord chord)
{
	defaults_[action] = chord;
	bindings_[action] = chord;
}

void Keybinds::bind(const std::string& action, KeyChord chord)
{
	bindings_[action] = chord;
}

bool Keybinds::is_bound(const std::string& action) const
{
	return bindings_.find(action) != bindings_.end();
}

std::optional<KeyChord> Keybinds::chord(const std::string& action) const
{
	const auto it = bindings_.find(action);
	if (it == bindings_.end()) {
		return std::nullopt;
	}
	return it->second;
}

std::string Keybinds::action_for(const SDL_Event& event) const
{
	for (const auto& [action, chord] : bindings_) {
		if (chord.matches(event)) {
			return action;
		}
	}
	return {};
}

void Keybinds::reset_to_defaults()
{
	bindings_ = defaults_;
}

bool Keybinds::load(const std::string& path, std::string* error)
{
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		if (error) {
			*error = "cannot open keybinds: " + path;
		}
		return false;
	}
	std::ostringstream buffer;
	buffer << in.rdbuf();

	nlohmann::json root;
	try {
		root = nlohmann::json::parse(buffer.str());
	} catch (const std::exception& ex) {
		if (error) {
			*error = std::string{"keybinds parse error: "} + ex.what();
		}
		return false;
	}
	if (!root.is_object() || !root.contains("bindings") ||
	    !root.at("bindings").is_object()) {
		if (error) {
			*error = "keybinds file must contain a 'bindings' object";
		}
		return false;
	}

	for (const auto& [action, value] : root.at("bindings").items()) {
		if (bindings_.find(action) == bindings_.end()) {
			if (error) {
				*error = "unknown action ignored: " + action;
			}
			continue;
		}
		if (!value.is_string()) {
			continue;
		}
		const auto parsed = parse_key_chord(value.get<std::string>());
		if (!parsed) {
			if (error) {
				*error = "unparsable chord ignored: " + value.get<std::string>();
			}
			continue;
		}
		bindings_[action] = *parsed;
	}
	return true;
}

bool Keybinds::save(const std::string& path, std::string* error) const
{
	nlohmann::json bindings = nlohmann::json::object();
	for (const auto& [action, chord] : bindings_) {
		bindings[action] = chord.to_string();
	}
	nlohmann::json root;
	root["version"]  = 1;
	root["bindings"] = std::move(bindings);

	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out) {
		if (error) {
			*error = "cannot write keybinds: " + path;
		}
		return false;
	}
	out << root.dump(2) << '\n';
	if (!out) {
		if (error) {
			*error = "keybinds write failed: " + path;
		}
		return false;
	}
	return true;
}

Keybinds default_keybinds()
{
	Keybinds keybinds;
	keybinds.define("editor.toggle", KeyChord{SDLK_F1, SDL_KMOD_NONE});
	keybinds.define("console.toggle", KeyChord{SDLK_F2, SDL_KMOD_NONE});
	// F5 sits with the F1/F2 view keys, clear of gameplay (WASD, Shift,
	// Space, mouse) and of the Ctrl+ editor chords.
	keybinds.define("camera.toggle_third_person",
	                KeyChord{SDLK_F5, SDL_KMOD_NONE});
	keybinds.define("undo", KeyChord{SDLK_Z, SDL_KMOD_CTRL});
	keybinds.define("redo", KeyChord{SDLK_Y, SDL_KMOD_CTRL});
	keybinds.define("scene.save", KeyChord{SDLK_S, SDL_KMOD_CTRL});
	keybinds.define("scene.reload", KeyChord{SDLK_R, SDL_KMOD_CTRL});
	keybinds.define("quit", KeyChord{SDLK_Q, SDL_KMOD_CTRL});
	return keybinds;
}

bool Keybinds::backfill(const std::string& path, std::string* error) const
{
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		return save(path, error);
	}
	std::ostringstream buffer;
	buffer << in.rdbuf();
	nlohmann::json root;
	try {
		root = nlohmann::json::parse(buffer.str());
	} catch (const std::exception& ex) {
		if (error) {
			*error = std::string{"keybinds parse error: "} + ex.what();
		}
		return false;
	}
	if (!root.is_object() || !root.contains("bindings") ||
	    !root.at("bindings").is_object()) {
		return save(path, error);
	}
	const nlohmann::json& bindings = root.at("bindings");
	for (const auto& entry : defaults_) {
		if (!bindings.contains(entry.first)) {
			return save(path, error);
		}
	}
	return false;
}

} // namespace lotus

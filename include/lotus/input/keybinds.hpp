#pragma once

#include <SDL3/SDL.h>

#include <map>
#include <optional>
#include <string>

namespace lotus {

/// A key plus modifier chord, e.g. Ctrl+Shift+Z.
struct KeyChord {
	SDL_Keycode key{SDLK_UNKNOWN};
	SDL_Keymod mods{SDL_KMOD_NONE};

	bool valid() const { return key != SDLK_UNKNOWN; }
	/// True for a non-repeat key-down event matching key and modifiers.
	bool matches(const SDL_Event& event) const;
	/// Human-readable form, e.g. "Ctrl+Shift+Z".
	std::string to_string() const;

	friend bool operator==(const KeyChord& a, const KeyChord& b)
	{
		return a.key == b.key && (a.mods & SDL_KMOD_CTRL) == (b.mods & SDL_KMOD_CTRL) &&
		       (a.mods & SDL_KMOD_SHIFT) == (b.mods & SDL_KMOD_SHIFT) &&
		       (a.mods & SDL_KMOD_ALT) == (b.mods & SDL_KMOD_ALT) &&
		       (a.mods & SDL_KMOD_GUI) == (b.mods & SDL_KMOD_GUI);
	}
};

/// Parses "Ctrl+Shift+Z" (case-insensitive modifiers). Returns nullopt when
/// the key name is unknown.
std::optional<KeyChord> parse_key_chord(const std::string& text);

/// Named actions bound to chords, loadable/savable as JSON so keybinds are
/// editable by hand or by an agent, like scene files.
class Keybinds {
public:
	/// Registers an action and its default chord (also the current binding).
	void define(std::string action, KeyChord chord);

	void bind(const std::string& action, KeyChord chord);
	bool is_bound(const std::string& action) const;
	std::optional<KeyChord> chord(const std::string& action) const;

	/// The action triggered by this event, or empty.
	std::string action_for(const SDL_Event& event) const;

	const std::map<std::string, KeyChord>& bindings() const { return bindings_; }
	const std::map<std::string, KeyChord>& defaults() const { return defaults_; }

	/// Restores every action to its default chord.
	void reset_to_defaults();

	/// Loads bindings, overriding defaults for known actions. Define actions
	/// first (see default_keybinds()); actions that were never defined are
	/// ignored and reported through \p error, as are unparsable chords.
	bool load(const std::string& path, std::string* error = nullptr);
	bool save(const std::string& path, std::string* error = nullptr) const;

private:
	std::map<std::string, KeyChord> defaults_;
	std::map<std::string, KeyChord> bindings_;
};

/// The engine's standard actions, with their default chords.
Keybinds default_keybinds();

} // namespace lotus

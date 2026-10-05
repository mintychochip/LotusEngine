#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "lotus/input/keybinds.hpp"

using namespace lotus;
namespace fs = std::filesystem;

namespace {

SDL_Event key_event(SDL_Keycode key, SDL_Keymod mods, bool repeat = false)
{
	SDL_Event event{};
	event.type            = SDL_EVENT_KEY_DOWN;
	event.key.key         = key;
	event.key.mod         = mods;
	event.key.repeat      = repeat;
	return event;
}

} // namespace

TEST(KeyChordTest, ParsesModifiersAndKey)
{
	const auto chord = parse_key_chord("Ctrl+Shift+Z");
	ASSERT_TRUE(chord.has_value());
	EXPECT_EQ(chord->key, SDLK_Z);
	EXPECT_EQ(chord->mods & SDL_KMOD_CTRL, SDL_KMOD_CTRL);
	EXPECT_EQ(chord->mods & SDL_KMOD_SHIFT, SDL_KMOD_SHIFT);
}

TEST(KeyChordTest, ParsesBareKey)
{
	const auto chord = parse_key_chord("F1");
	ASSERT_TRUE(chord.has_value());
	EXPECT_EQ(chord->key, SDLK_F1);
	EXPECT_EQ(chord->mods, SDL_KMOD_NONE);
}

TEST(KeyChordTest, RejectsUnknownNames)
{
	EXPECT_FALSE(parse_key_chord("Ctrl+NotAKey").has_value());
	EXPECT_FALSE(parse_key_chord("").has_value());
}

TEST(KeyChordTest, RoundTripsThroughString)
{
	const auto chord = parse_key_chord("Ctrl+S");
	ASSERT_TRUE(chord.has_value());
	EXPECT_EQ(chord->to_string(), "Ctrl+S");
	const auto again = parse_key_chord(chord->to_string());
	ASSERT_TRUE(again.has_value());
	EXPECT_TRUE(*again == *chord);
}

TEST(KeyChordTest, MatchesOnlyExactModifiers)
{
	const KeyChord chord{SDLK_Z, SDL_KMOD_CTRL};
	EXPECT_TRUE(chord.matches(key_event(SDLK_Z, SDL_KMOD_CTRL)));
	EXPECT_FALSE(chord.matches(key_event(SDLK_Z, SDL_KMOD_NONE)));
	EXPECT_FALSE(chord.matches(key_event(SDLK_X, SDL_KMOD_CTRL)));
	// Caps lock / num lock must not defeat a binding.
	EXPECT_TRUE(chord.matches(key_event(SDLK_Z, SDL_KMOD_CTRL | SDL_KMOD_CAPS)));
	// Key repeats must not re-trigger.
	EXPECT_FALSE(chord.matches(key_event(SDLK_Z, SDL_KMOD_CTRL, true)));
}

TEST(KeybindsTest, DefaultsAreDefined)
{
	Keybinds keybinds = default_keybinds();
	EXPECT_TRUE(keybinds.is_bound("undo"));
	EXPECT_TRUE(keybinds.is_bound("scene.save"));
	EXPECT_EQ(keybinds.action_for(key_event(SDLK_F1, SDL_KMOD_NONE)), "editor.toggle");
	EXPECT_EQ(keybinds.action_for(key_event(SDLK_Z, SDL_KMOD_CTRL)), "undo");
	EXPECT_EQ(keybinds.action_for(key_event(SDLK_S, SDL_KMOD_CTRL)), "scene.save");
	EXPECT_TRUE(keybinds.action_for(key_event(SDLK_F9, SDL_KMOD_NONE)).empty());
}

TEST(KeybindsTest, RebindChangesDispatch)
{
	Keybinds keybinds = default_keybinds();
	keybinds.bind("undo", KeyChord{SDLK_U, SDL_KMOD_CTRL});
	EXPECT_EQ(keybinds.action_for(key_event(SDLK_U, SDL_KMOD_CTRL)), "undo");
	EXPECT_TRUE(keybinds.action_for(key_event(SDLK_Z, SDL_KMOD_CTRL)).empty());
}

TEST(KeybindsTest, ResetRestoresDefaults)
{
	Keybinds keybinds = default_keybinds();
	keybinds.bind("undo", KeyChord{SDLK_U, SDL_KMOD_CTRL});
	keybinds.reset_to_defaults();
	EXPECT_EQ(keybinds.action_for(key_event(SDLK_Z, SDL_KMOD_CTRL)), "undo");
}

TEST(KeybindsTest, FileRoundTrip)
{
	const std::string path = (fs::temp_directory_path() / "lotus_keybinds.json").string();
	Keybinds keybinds = default_keybinds();
	keybinds.bind("undo", KeyChord{SDLK_U, SDL_KMOD_CTRL | SDL_KMOD_SHIFT});

	std::string error;
	ASSERT_TRUE(keybinds.save(path, &error)) << error;

	Keybinds loaded = default_keybinds();
	ASSERT_TRUE(loaded.load(path, &error)) << error;
	const auto chord = loaded.chord("undo");
	ASSERT_TRUE(chord.has_value());
	EXPECT_EQ(chord->key, SDLK_U);
	EXPECT_EQ(chord->mods & SDL_KMOD_SHIFT, SDL_KMOD_SHIFT);

	std::remove(path.c_str());
}

TEST(KeybindsTest, UnknownActionInFileIsIgnored)
{
	const std::string path =
	        (fs::temp_directory_path() / "lotus_keybinds_unknown.json").string();
	{
		std::ofstream out(path);
		out << R"({"version":1,"bindings":{"not.an.action":"Ctrl+K","undo":"Ctrl+U"}})";
	}

	Keybinds keybinds = default_keybinds();
	std::string error;
	EXPECT_TRUE(keybinds.load(path, &error));
	EXPECT_FALSE(error.empty());
	EXPECT_FALSE(keybinds.is_bound("not.an.action"));
	EXPECT_EQ(keybinds.action_for(key_event(SDLK_U, SDL_KMOD_CTRL)), "undo");

	std::remove(path.c_str());
}

TEST(KeybindsTest, LoadAfterDefaultsOverridesWithoutWarnings)
{
	// The engine's sequence: defaults first, then the file overrides them.
	const std::string path =
	        (fs::temp_directory_path() / "lotus_keybinds_order.json").string();
	{
		std::ofstream out(path);
		out << R"({"version":1,"bindings":{"undo":"Ctrl+U"}})";
	}

	Keybinds keybinds = default_keybinds();
	std::string error;
	EXPECT_TRUE(keybinds.load(path, &error));
	EXPECT_TRUE(error.empty()) << error;
	EXPECT_EQ(keybinds.action_for(key_event(SDLK_U, SDL_KMOD_CTRL)), "undo");
	// Unlisted actions keep their defaults.
	EXPECT_EQ(keybinds.action_for(key_event(SDLK_S, SDL_KMOD_CTRL)), "scene.save");

	std::remove(path.c_str());
}

TEST(KeybindsTest, LoadWithoutDefinitionsIgnoresEveryAction)
{
	// Documents the contract: load() only overrides actions that exist, so a
	// caller that forgets to define defaults ends up with no bindings.
	const std::string path =
	        (fs::temp_directory_path() / "lotus_keybinds_undefined.json").string();
	{
		std::ofstream out(path);
		out << R"({"version":1,"bindings":{"undo":"Ctrl+U"}})";
	}

	Keybinds undefined;
	std::string error;
	EXPECT_TRUE(undefined.load(path, &error));
	EXPECT_FALSE(error.empty());
	EXPECT_TRUE(undefined.bindings().empty());

	std::remove(path.c_str());
}

TEST(KeybindsTest, MissingFileReportsError)
{
	Keybinds keybinds = default_keybinds();
	std::string error;
	EXPECT_FALSE(keybinds.load("definitely_missing_keybinds.json", &error));
	EXPECT_FALSE(error.empty());
}

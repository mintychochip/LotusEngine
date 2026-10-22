#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "lotus/input/keybinds.hpp"

using namespace lotus;
namespace fs = std::filesystem;

namespace {

KeyEvent key_event(Key key, Mod mods, bool repeat = false)
{
	return KeyEvent{key, mods, true, repeat};
}

} // namespace

TEST(KeyChordTest, ParsesModifiersAndKey)
{
	const auto chord = parse_key_chord("Ctrl+Shift+Z");
	ASSERT_TRUE(chord.has_value());
	EXPECT_EQ(chord->key, Key::Z);
	EXPECT_EQ(chord->mods & Mod::Ctrl, Mod::Ctrl);
	EXPECT_EQ(chord->mods & Mod::Shift, Mod::Shift);
}

TEST(KeyChordTest, ParsesBareKey)
{
	const auto chord = parse_key_chord("F1");
	ASSERT_TRUE(chord.has_value());
	EXPECT_EQ(chord->key, Key::F1);
	EXPECT_EQ(chord->mods, Mod::None);
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
	const KeyChord chord{Key::Z, Mod::Ctrl};
	EXPECT_TRUE(chord.matches(key_event(Key::Z, Mod::Ctrl)));
	EXPECT_FALSE(chord.matches(key_event(Key::Z, Mod::None)));
	EXPECT_FALSE(chord.matches(key_event(Key::X, Mod::Ctrl)));
	// An extra modifier makes it a different chord.
	EXPECT_FALSE(chord.matches(key_event(Key::Z, Mod::Ctrl | Mod::Shift)));
	// Key repeats must not re-trigger.
	EXPECT_FALSE(chord.matches(key_event(Key::Z, Mod::Ctrl, true)));
	// Letting go of the key is not a trigger either.
	EXPECT_FALSE(chord.matches(KeyEvent{Key::Z, Mod::Ctrl, false, false}));
}

TEST(KeybindsTest, DefaultsAreDefined)
{
	Keybinds keybinds = default_keybinds();
	EXPECT_TRUE(keybinds.is_bound("undo"));
	EXPECT_TRUE(keybinds.is_bound("scene.save"));
	EXPECT_EQ(keybinds.action_for(key_event(Key::F1, Mod::None)), "editor.toggle");
	EXPECT_EQ(keybinds.action_for(key_event(Key::Z, Mod::Ctrl)), "undo");
	EXPECT_EQ(keybinds.action_for(key_event(Key::S, Mod::Ctrl)), "scene.save");
	EXPECT_TRUE(keybinds.action_for(key_event(Key::F9, Mod::None)).empty());
}

TEST(KeybindsTest, RebindChangesDispatch)
{
	Keybinds keybinds = default_keybinds();
	keybinds.bind("undo", KeyChord{Key::U, Mod::Ctrl});
	EXPECT_EQ(keybinds.action_for(key_event(Key::U, Mod::Ctrl)), "undo");
	EXPECT_TRUE(keybinds.action_for(key_event(Key::Z, Mod::Ctrl)).empty());
}

TEST(KeybindsTest, ResetRestoresDefaults)
{
	Keybinds keybinds = default_keybinds();
	keybinds.bind("undo", KeyChord{Key::U, Mod::Ctrl});
	keybinds.reset_to_defaults();
	EXPECT_EQ(keybinds.action_for(key_event(Key::Z, Mod::Ctrl)), "undo");
}

TEST(KeybindsTest, FileRoundTrip)
{
	const std::string path = (fs::temp_directory_path() / "lotus_keybinds.json").string();
	Keybinds keybinds = default_keybinds();
	keybinds.bind("undo", KeyChord{Key::U, Mod::Ctrl | Mod::Shift});

	std::string error;
	ASSERT_TRUE(keybinds.save(path, &error)) << error;

	Keybinds loaded = default_keybinds();
	ASSERT_TRUE(loaded.load(path, &error)) << error;
	const auto chord = loaded.chord("undo");
	ASSERT_TRUE(chord.has_value());
	EXPECT_EQ(chord->key, Key::U);
	EXPECT_EQ(chord->mods & Mod::Shift, Mod::Shift);

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
	EXPECT_EQ(keybinds.action_for(key_event(Key::U, Mod::Ctrl)), "undo");

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
	EXPECT_EQ(keybinds.action_for(key_event(Key::U, Mod::Ctrl)), "undo");
	// Unlisted actions keep their defaults.
	EXPECT_EQ(keybinds.action_for(key_event(Key::S, Mod::Ctrl)), "scene.save");

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

TEST(KeybindsTest, ThirdPersonToggleDefaultIsF5)
{
	Keybinds keybinds = default_keybinds();
	EXPECT_TRUE(keybinds.is_bound("camera.toggle_third_person"));
	EXPECT_EQ(keybinds.action_for(key_event(Key::F5, Mod::None)),
	          "camera.toggle_third_person");
	// The new default leaves the established chords alone.
	EXPECT_EQ(keybinds.action_for(key_event(Key::F1, Mod::None)),
	          "editor.toggle");
	EXPECT_EQ(keybinds.action_for(key_event(Key::F2, Mod::None)),
	          "console.toggle");
	EXPECT_TRUE(
	        keybinds.action_for(key_event(Key::W, Mod::None)).empty());
	EXPECT_TRUE(keybinds.action_for(key_event(Key::Space, Mod::None))
	                    .empty());
}

TEST(KeybindsTest, BackfillCompletesStaleFileKeepingRebinds)
{
	const std::string path =
	        (fs::temp_directory_path() / "lotus_keybinds_backfill.json").string();
	{
		std::ofstream out(path);
		out << R"({"version":1,"bindings":{"undo":"Ctrl+U"}})";
	}

	Keybinds keybinds = default_keybinds();
	std::string error;
	ASSERT_TRUE(keybinds.load(path, &error)) << error;
	EXPECT_TRUE(keybinds.backfill(path, &error)) << error;

	Keybinds reloaded = default_keybinds();
	ASSERT_TRUE(reloaded.load(path, &error)) << error;
	EXPECT_EQ(reloaded.action_for(key_event(Key::U, Mod::Ctrl)), "undo");
	EXPECT_EQ(reloaded.action_for(key_event(Key::F5, Mod::None)),
	          "camera.toggle_third_person");
	// A complete file is left alone.
	EXPECT_FALSE(reloaded.backfill(path, &error)) << error;

	std::remove(path.c_str());
}

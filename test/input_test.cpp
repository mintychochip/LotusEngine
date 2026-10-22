#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <string_view>

#include "lotus/input/input.hpp"
#include "lotus/input/key.hpp"

using namespace lotus;

namespace {

constexpr int kFirstKey = 1;
constexpr int kKeyCount = static_cast<int>(Key::Count);

std::string lowered(std::string_view text)
{
	std::string out{text};
	std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return out;
}

KeyEvent key_event(Key key, bool pressed, bool repeat = false)
{
	return KeyEvent{key, Mod::None, pressed, repeat};
}

} // namespace

// Names are what keybinds.json holds, so every key needs one, no two may
// collide (parsing is case-insensitive), and each must parse back to itself.
TEST(KeyNamesTest, EveryKeyHasAUniqueNameThatParsesBack)
{
	std::set<std::string> seen;
	for (int i = kFirstKey; i < kKeyCount; ++i) {
		const Key key                   = static_cast<Key>(i);
		const std::string_view name = key_name(key);
		ASSERT_FALSE(name.empty()) << "key " << i << " has no name";
		EXPECT_TRUE(seen.insert(lowered(name)).second)
		        << "duplicate name " << name;
		EXPECT_EQ(key_from_name(name), key) << name;
	}
}

// The keys a keybinds.json written by the SDL build can hold must keep their
// names, or an existing file would stop parsing.
TEST(KeyNamesTest, KeepsTheNamesExistingKeybindFilesUse)
{
	EXPECT_EQ(key_name(Key::F1), "F1");
	EXPECT_EQ(key_name(Key::F2), "F2");
	EXPECT_EQ(key_name(Key::F5), "F5");
	EXPECT_EQ(key_name(Key::Q), "Q");
	EXPECT_EQ(key_name(Key::Y), "Y");
	EXPECT_EQ(key_name(Key::R), "R");
	EXPECT_EQ(key_name(Key::S), "S");
	EXPECT_EQ(key_name(Key::Z), "Z");
	EXPECT_EQ(key_name(Key::Space), "Space");
	EXPECT_EQ(key_name(Key::Enter), "Return");
	EXPECT_EQ(key_name(Key::Escape), "Escape");
	EXPECT_EQ(key_name(Key::LeftShift), "Left Shift");
}

TEST(KeyNamesTest, ParsingIgnoresCase)
{
	EXPECT_EQ(key_from_name("z"), Key::Z);
	EXPECT_EQ(key_from_name("f1"), Key::F1);
	EXPECT_EQ(key_from_name("SPACE"), Key::Space);
	EXPECT_EQ(key_from_name("left shift"), Key::LeftShift);
}

TEST(KeyNamesTest, AcceptsEnterAndEscAsAliases)
{
	EXPECT_EQ(key_from_name("Enter"), Key::Enter);
	EXPECT_EQ(key_from_name("Esc"), Key::Escape);
}

TEST(KeyNamesTest, UnknownNamesAreUnknown)
{
	EXPECT_EQ(key_from_name(""), Key::Unknown);
	EXPECT_EQ(key_from_name("nope"), Key::Unknown);
	// A modifier word is not a key, and F25 is past the last named one.
	EXPECT_EQ(key_from_name("Ctrl"), Key::Unknown);
	EXPECT_EQ(key_from_name("F25"), Key::Unknown);
	EXPECT_TRUE(key_name(Key::Unknown).empty());
	EXPECT_TRUE(key_name(Key::Count).empty());
}

TEST(ModTest, CombinesAndTestsBits)
{
	const Mod chord = Mod::Ctrl | Mod::Shift;
	EXPECT_EQ(chord & Mod::Ctrl, Mod::Ctrl);
	EXPECT_EQ(chord & Mod::Shift, Mod::Shift);
	EXPECT_EQ(chord & Mod::Alt, Mod::None);
	EXPECT_TRUE(any(chord));
	EXPECT_FALSE(any(Mod::None));
	Mod grown = Mod::None;
	grown |= Mod::Gui;
	EXPECT_EQ(grown, Mod::Gui);
}

TEST(InputTest, KeyFollowsPressAndRelease)
{
	Input input;
	EXPECT_FALSE(input.key_down(Key::W));
	input.apply(key_event(Key::W, true));
	EXPECT_TRUE(input.key_down(Key::W));
	input.apply(key_event(Key::W, false));
	EXPECT_FALSE(input.key_down(Key::W));
}

TEST(InputTest, KeysAreIndependent)
{
	Input input;
	input.apply(key_event(Key::W, true));
	input.apply(key_event(Key::LeftShift, true));
	input.apply(key_event(Key::W, false));
	EXPECT_FALSE(input.key_down(Key::W));
	EXPECT_TRUE(input.key_down(Key::LeftShift));
}

// An auto-repeat arrives as another press while the key is held: it must not
// release the key, and must not be the thing that presses it either.
TEST(InputTest, RepeatKeepsTheKeyDown)
{
	Input input;
	input.apply(key_event(Key::A, true));
	input.apply(key_event(Key::A, true, true));
	EXPECT_TRUE(input.key_down(Key::A));
	input.apply(key_event(Key::A, false));
	EXPECT_FALSE(input.key_down(Key::A));
}

TEST(InputTest, UnknownKeysAreIgnored)
{
	Input input;
	input.apply(key_event(Key::Unknown, true));
	EXPECT_TRUE(input.keys().none());
}

// Callers that detect a fresh press keep the previous snapshot; it has to be a
// copy that later events do not change.
TEST(InputTest, KeySnapshotIsACopy)
{
	Input input;
	input.apply(key_event(Key::E, true));
	const KeySet before = input.keys();
	input.apply(key_event(Key::E, false));
	EXPECT_TRUE(before[static_cast<std::size_t>(Key::E)]);
	EXPECT_FALSE(input.key_down(Key::E));
}

TEST(InputTest, MouseButtonsFollowEvents)
{
	Input input;
	EXPECT_FALSE(input.mouse_down(MouseButton::Right));
	input.apply(MouseButtonEvent{MouseButton::Right, true, Mod::None});
	EXPECT_TRUE(input.mouse_down(MouseButton::Right));
	EXPECT_FALSE(input.mouse_down(MouseButton::Left));
	EXPECT_EQ(input.mouse_buttons(), mouse_bit(MouseButton::Right));
	input.apply(MouseButtonEvent{MouseButton::Right, false, Mod::None});
	EXPECT_FALSE(input.mouse_down(MouseButton::Right));
}

TEST(InputTest, MousePositionFollowsMoves)
{
	Input input;
	input.apply(MouseMoveEvent{12.5f, 40.0f, 1.0f, -2.0f, 0});
	EXPECT_FLOAT_EQ(input.mouse_x(), 12.5f);
	EXPECT_FLOAT_EQ(input.mouse_y(), 40.0f);
}

TEST(MouseMoveEventTest, ReportsWhichButtonsWereHeld)
{
	const MouseMoveEvent drag{0.0f, 0.0f, 3.0f, 4.0f,
	                          mouse_bit(MouseButton::Right)};
	EXPECT_TRUE(drag.down(MouseButton::Right));
	EXPECT_FALSE(drag.down(MouseButton::Left));
}

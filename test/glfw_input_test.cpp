#include <gtest/gtest.h>

#include <GLFW/glfw3.h>

#include <cstddef>
#include <set>

#include "core/application/glfw_input.hpp"

using namespace lotus;

// Every engine key must be reachable from exactly one GLFW key. A key missing
// from the table is a key the player can press that does nothing; two GLFW
// keys on one engine key would make them indistinguishable.
TEST(GlfwInputTest, EveryEngineKeyIsReachableFromExactlyOneGlfwKey)
{
	std::set<Key> seen;
	for (int glfw_key = GLFW_KEY_SPACE; glfw_key <= GLFW_KEY_LAST; ++glfw_key) {
		const Key key = key_from_glfw(glfw_key);
		if (key == Key::Unknown) {
			continue;
		}
		EXPECT_TRUE(seen.insert(key).second)
		        << "two GLFW keys map to " << key_name(key);
	}
	EXPECT_EQ(seen.size(), static_cast<std::size_t>(Key::Count) - 1);
}

TEST(GlfwInputTest, TheKeysTheGameReadsMapToTheirNamesakes)
{
	EXPECT_EQ(key_from_glfw(GLFW_KEY_W), Key::W);
	EXPECT_EQ(key_from_glfw(GLFW_KEY_SPACE), Key::Space);
	EXPECT_EQ(key_from_glfw(GLFW_KEY_LEFT_SHIFT), Key::LeftShift);
	EXPECT_EQ(key_from_glfw(GLFW_KEY_RIGHT_SHIFT), Key::RightShift);
	EXPECT_EQ(key_from_glfw(GLFW_KEY_UP), Key::Up);
	EXPECT_EQ(key_from_glfw(GLFW_KEY_F5), Key::F5);
	EXPECT_EQ(key_from_glfw(GLFW_KEY_ESCAPE), Key::Escape);
}

TEST(GlfwInputTest, KeysWithoutAnEngineKeyAreUnknown)
{
	EXPECT_EQ(key_from_glfw(GLFW_KEY_UNKNOWN), Key::Unknown);
	EXPECT_EQ(key_from_glfw(GLFW_KEY_WORLD_1), Key::Unknown);
	EXPECT_EQ(key_from_glfw(GLFW_KEY_F25), Key::Unknown);
	EXPECT_EQ(key_from_glfw(-5), Key::Unknown);
}

TEST(GlfwInputTest, ModifiersTranslateAndLocksAreDropped)
{
	EXPECT_EQ(mods_from_glfw(0), Mod::None);
	EXPECT_EQ(mods_from_glfw(GLFW_MOD_CONTROL), Mod::Ctrl);
	EXPECT_EQ(mods_from_glfw(GLFW_MOD_CONTROL | GLFW_MOD_SHIFT),
	          Mod::Ctrl | Mod::Shift);
	EXPECT_EQ(mods_from_glfw(GLFW_MOD_ALT | GLFW_MOD_SUPER),
	          Mod::Alt | Mod::Gui);
	// Caps lock and num lock must not defeat a binding.
	EXPECT_EQ(mods_from_glfw(GLFW_MOD_CONTROL | GLFW_MOD_CAPS_LOCK |
	                         GLFW_MOD_NUM_LOCK),
	          Mod::Ctrl);
}

TEST(GlfwInputTest, OnlyTheThreeMainMouseButtonsAreTracked)
{
	EXPECT_EQ(mouse_button_from_glfw(GLFW_MOUSE_BUTTON_LEFT),
	          MouseButton::Left);
	EXPECT_EQ(mouse_button_from_glfw(GLFW_MOUSE_BUTTON_RIGHT),
	          MouseButton::Right);
	EXPECT_EQ(mouse_button_from_glfw(GLFW_MOUSE_BUTTON_MIDDLE),
	          MouseButton::Middle);
	EXPECT_EQ(mouse_button_from_glfw(GLFW_MOUSE_BUTTON_4), MouseButton::Count);
}

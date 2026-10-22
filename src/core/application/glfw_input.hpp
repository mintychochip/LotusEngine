#pragma once

#include "lotus/input/input.hpp"

namespace lotus {

/// The engine key for a GLFW key token; Unknown for tokens the engine has no
/// key for (GLFW_KEY_UNKNOWN, the non-US "world" keys, F25).
Key key_from_glfw(int glfw_key);

/// The engine modifiers for GLFW modifier bits. Caps lock and num lock are
/// dropped: they are not modifiers as far as a binding is concerned.
Mod mods_from_glfw(int glfw_mods);

/// The engine button for a GLFW mouse button; MouseButton::Count for the extra
/// buttons the engine does not track.
MouseButton mouse_button_from_glfw(int glfw_button);

} // namespace lotus

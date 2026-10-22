#pragma once

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <variant>

#include "lotus/input/key.hpp"

namespace lotus {

enum class MouseButton : std::uint8_t {
	Left,
	Right,
	Middle,
	Count,
};

/// The bit a button occupies in a button mask.
constexpr std::uint8_t mouse_bit(MouseButton button)
{
	return static_cast<std::uint8_t>(1u << static_cast<unsigned>(button));
}

/// A key went down, auto-repeated while down, or came up.
struct KeyEvent {
	Key key{Key::Unknown};
	Mod mods{Mod::None};
	bool pressed{false};
	/// A press that repeats a key already held (the OS auto-repeat).
	bool repeat{false};
};

struct MouseButtonEvent {
	MouseButton button{MouseButton::Left};
	bool pressed{false};
	Mod mods{Mod::None};
};

/// The cursor moved. Positions and deltas are in window coordinates, +x right
/// and +y down.
struct MouseMoveEvent {
	float x{0.0f};
	float y{0.0f};
	/// Movement since the previous position; zero for the first one seen.
	float dx{0.0f};
	float dy{0.0f};
	/// Buttons held during the move, one bit per MouseButton.
	std::uint8_t buttons{0};

	bool down(MouseButton button) const
	{
		return (buttons & mouse_bit(button)) != 0;
	}
};

using Event = std::variant<KeyEvent, MouseButtonEvent, MouseMoveEvent>;

/// Which keys are held, by Key index.
using KeySet = std::bitset<static_cast<std::size_t>(Key::Count)>;

/// What is held right now, kept up to date by applying events in order. A
/// window owns one and feeds it every event it delivers, so code that polls
/// ("is W down?") and code that reacts to events see the same input.
class Input {
public:
	void apply(const Event& event);

	bool key_down(Key key) const;
	/// The whole key state, for callers that detect a fresh press by comparing
	/// with the previous frame's copy.
	const KeySet& keys() const { return keys_; }

	bool mouse_down(MouseButton button) const
	{
		return (buttons_ & mouse_bit(button)) != 0;
	}
	std::uint8_t mouse_buttons() const { return buttons_; }
	float mouse_x() const { return mouse_x_; }
	float mouse_y() const { return mouse_y_; }

private:
	KeySet keys_;
	std::uint8_t buttons_{0};
	float mouse_x_{0.0f};
	float mouse_y_{0.0f};
};

} // namespace lotus

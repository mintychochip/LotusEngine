#include "lotus/input/input.hpp"

namespace lotus {

namespace {

template <class... Handlers>
struct Overloaded : Handlers... {
	using Handlers::operator()...;
};

} // namespace

void Input::apply(const Event& event)
{
	std::visit(
	        Overloaded{
	                [this](const KeyEvent& key) {
		                const auto index = static_cast<std::size_t>(key.key);
		                if (key.key == Key::Unknown || index >= keys_.size()) {
			                return;
		                }
		                keys_.set(index, key.pressed);
	                },
	                [this](const MouseButtonEvent& button) {
		                if (button.pressed) {
			                buttons_ |= mouse_bit(button.button);
		                } else {
			                buttons_ &= static_cast<std::uint8_t>(
			                        ~mouse_bit(button.button));
		                }
	                },
	                [this](const MouseMoveEvent& move) {
		                mouse_x_ = move.x;
		                mouse_y_ = move.y;
	                },
	        },
	        event);
}

bool Input::key_down(Key key) const
{
	const auto index = static_cast<std::size_t>(key);
	return index < keys_.size() && keys_.test(index);
}

} // namespace lotus

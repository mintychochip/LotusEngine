#pragma once

#include <cstdint>
#include <string_view>

namespace lotus {

/// Every key the engine names, with the text a keybinds file spells it with.
/// The spellings are SDL's (the SDL build wrote them), so existing
/// keybinds.json files keep parsing. Enum order is this order.
#define LOTUS_KEY_LIST(KEY)                                                    \
	KEY(Space, "Space")                                                        \
	KEY(Apostrophe, "'")                                                       \
	KEY(Comma, ",")                                                            \
	KEY(Minus, "-")                                                            \
	KEY(Period, ".")                                                           \
	KEY(Slash, "/")                                                            \
	KEY(Num0, "0")                                                             \
	KEY(Num1, "1")                                                             \
	KEY(Num2, "2")                                                             \
	KEY(Num3, "3")                                                             \
	KEY(Num4, "4")                                                             \
	KEY(Num5, "5")                                                             \
	KEY(Num6, "6")                                                             \
	KEY(Num7, "7")                                                             \
	KEY(Num8, "8")                                                             \
	KEY(Num9, "9")                                                             \
	KEY(Semicolon, ";")                                                        \
	KEY(Equal, "=")                                                            \
	KEY(A, "A")                                                                \
	KEY(B, "B")                                                                \
	KEY(C, "C")                                                                \
	KEY(D, "D")                                                                \
	KEY(E, "E")                                                                \
	KEY(F, "F")                                                                \
	KEY(G, "G")                                                                \
	KEY(H, "H")                                                                \
	KEY(I, "I")                                                                \
	KEY(J, "J")                                                                \
	KEY(K, "K")                                                                \
	KEY(L, "L")                                                                \
	KEY(M, "M")                                                                \
	KEY(N, "N")                                                                \
	KEY(O, "O")                                                                \
	KEY(P, "P")                                                                \
	KEY(Q, "Q")                                                                \
	KEY(R, "R")                                                                \
	KEY(S, "S")                                                                \
	KEY(T, "T")                                                                \
	KEY(U, "U")                                                                \
	KEY(V, "V")                                                                \
	KEY(W, "W")                                                                \
	KEY(X, "X")                                                                \
	KEY(Y, "Y")                                                                \
	KEY(Z, "Z")                                                                \
	KEY(LeftBracket, "[")                                                      \
	KEY(Backslash, "\\")                                                       \
	KEY(RightBracket, "]")                                                     \
	KEY(GraveAccent, "`")                                                      \
	KEY(Escape, "Escape")                                                      \
	KEY(Enter, "Return")                                                       \
	KEY(Tab, "Tab")                                                            \
	KEY(Backspace, "Backspace")                                                \
	KEY(Insert, "Insert")                                                      \
	KEY(Delete, "Delete")                                                      \
	KEY(Right, "Right")                                                        \
	KEY(Left, "Left")                                                          \
	KEY(Down, "Down")                                                          \
	KEY(Up, "Up")                                                              \
	KEY(PageUp, "PageUp")                                                      \
	KEY(PageDown, "PageDown")                                                  \
	KEY(Home, "Home")                                                          \
	KEY(End, "End")                                                            \
	KEY(CapsLock, "CapsLock")                                                  \
	KEY(ScrollLock, "ScrollLock")                                              \
	KEY(NumLock, "NumLock")                                                    \
	KEY(PrintScreen, "PrintScreen")                                            \
	KEY(Pause, "Pause")                                                        \
	KEY(F1, "F1")                                                              \
	KEY(F2, "F2")                                                              \
	KEY(F3, "F3")                                                              \
	KEY(F4, "F4")                                                              \
	KEY(F5, "F5")                                                              \
	KEY(F6, "F6")                                                              \
	KEY(F7, "F7")                                                              \
	KEY(F8, "F8")                                                              \
	KEY(F9, "F9")                                                              \
	KEY(F10, "F10")                                                            \
	KEY(F11, "F11")                                                            \
	KEY(F12, "F12")                                                            \
	KEY(F13, "F13")                                                            \
	KEY(F14, "F14")                                                            \
	KEY(F15, "F15")                                                            \
	KEY(F16, "F16")                                                            \
	KEY(F17, "F17")                                                            \
	KEY(F18, "F18")                                                            \
	KEY(F19, "F19")                                                            \
	KEY(F20, "F20")                                                            \
	KEY(F21, "F21")                                                            \
	KEY(F22, "F22")                                                            \
	KEY(F23, "F23")                                                            \
	KEY(F24, "F24")                                                            \
	KEY(Keypad0, "Keypad 0")                                                   \
	KEY(Keypad1, "Keypad 1")                                                   \
	KEY(Keypad2, "Keypad 2")                                                   \
	KEY(Keypad3, "Keypad 3")                                                   \
	KEY(Keypad4, "Keypad 4")                                                   \
	KEY(Keypad5, "Keypad 5")                                                   \
	KEY(Keypad6, "Keypad 6")                                                   \
	KEY(Keypad7, "Keypad 7")                                                   \
	KEY(Keypad8, "Keypad 8")                                                   \
	KEY(Keypad9, "Keypad 9")                                                   \
	KEY(KeypadDecimal, "Keypad .")                                             \
	KEY(KeypadDivide, "Keypad /")                                              \
	KEY(KeypadMultiply, "Keypad *")                                            \
	KEY(KeypadSubtract, "Keypad -")                                            \
	KEY(KeypadAdd, "Keypad +")                                                 \
	KEY(KeypadEnter, "Keypad Enter")                                           \
	KEY(KeypadEqual, "Keypad =")                                               \
	KEY(LeftShift, "Left Shift")                                               \
	KEY(LeftControl, "Left Ctrl")                                              \
	KEY(LeftAlt, "Left Alt")                                                   \
	KEY(LeftSuper, "Left GUI")                                                 \
	KEY(RightShift, "Right Shift")                                             \
	KEY(RightControl, "Right Ctrl")                                            \
	KEY(RightAlt, "Right Alt")                                                 \
	KEY(RightSuper, "Right GUI")                                               \
	KEY(Menu, "Menu")

/// A physical keyboard key, identified by where it sits on a US-layout
/// keyboard rather than by what it prints, so WASD stays under the left hand
/// on every layout. Unknown stands for a key the engine has no name for;
/// Count is not a key.
enum class Key : std::uint16_t {
	Unknown = 0,
#define LOTUS_KEY_ENUM(id, name) id,
	LOTUS_KEY_LIST(LOTUS_KEY_ENUM)
#undef LOTUS_KEY_ENUM
	Count,
};

/// Modifier keys held during an event, or required by a key chord. Caps lock
/// and num lock are deliberately not here, so they cannot defeat a binding.
enum class Mod : std::uint8_t {
	None  = 0,
	Shift = 1,
	Ctrl  = 2,
	Alt   = 4,
	Gui   = 8,
};

constexpr Mod operator|(Mod a, Mod b)
{
	return static_cast<Mod>(static_cast<std::uint8_t>(a) |
	                        static_cast<std::uint8_t>(b));
}

constexpr Mod operator&(Mod a, Mod b)
{
	return static_cast<Mod>(static_cast<std::uint8_t>(a) &
	                        static_cast<std::uint8_t>(b));
}

constexpr Mod& operator|=(Mod& a, Mod b)
{
	return a = a | b;
}

/// True when at least one modifier is set.
constexpr bool any(Mod mods)
{
	return mods != Mod::None;
}

/// The name a key is saved under in keybinds files ("Z", "F1", "Left Shift");
/// empty for Unknown and Count.
std::string_view key_name(Key key);

/// The key a name spells, ignoring case ("enter" and "esc" are accepted too),
/// or Unknown.
Key key_from_name(std::string_view name);

} // namespace lotus

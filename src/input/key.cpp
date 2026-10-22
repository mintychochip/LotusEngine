#include "lotus/input/key.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <string>
#include <unordered_map>

namespace lotus {

namespace {

// Index 0 is Unknown, which has no name; the rest follow LOTUS_KEY_LIST.
constexpr std::array<std::string_view, static_cast<std::size_t>(Key::Count)>
        kKeyNames = {
                "",
#define LOTUS_KEY_NAME(id, name) name,
                LOTUS_KEY_LIST(LOTUS_KEY_NAME)
#undef LOTUS_KEY_NAME
};

std::string lowered(std::string_view text)
{
	std::string out{text};
	std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return out;
}

} // namespace

std::string_view key_name(Key key)
{
	const auto index = static_cast<std::size_t>(key);
	return index < kKeyNames.size() ? kKeyNames[index] : std::string_view{};
}

Key key_from_name(std::string_view name)
{
	static const std::unordered_map<std::string, Key> lookup = [] {
		std::unordered_map<std::string, Key> map;
		for (std::size_t i = 1; i < kKeyNames.size(); ++i) {
			map.emplace(lowered(kKeyNames[i]), static_cast<Key>(i));
		}
		map.emplace("enter", Key::Enter);
		map.emplace("esc", Key::Escape);
		return map;
	}();
	const auto found = lookup.find(lowered(name));
	return found != lookup.end() ? found->second : Key::Unknown;
}

} // namespace lotus

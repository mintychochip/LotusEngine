#include "lotus/net/protocol.hpp"

namespace lotus {

std::uint32_t fnv1a32(const char* data, std::size_t len)
{
	std::uint32_t hash = 2166136261u;
	for (std::size_t i = 0; i < len; ++i) {
		hash ^= static_cast<std::uint8_t>(data[i]);
		hash *= 16777619u;
	}
	return hash;
}

std::uint32_t fnv1a32(const std::string& text)
{
	return fnv1a32(text.data(), text.size());
}

} // namespace lotus

#pragma once

#include <cstdint>
#include <string>

namespace lotus {

inline constexpr std::uint8_t kProtocolVersion = 1;
inline constexpr std::size_t kMtu              = 1200;
inline constexpr std::size_t kMaxFragment      = 1024;
inline constexpr float kResendInterval         = 0.1f;
inline constexpr int kMaxResends               = 10;
inline constexpr float kConnectionTimeout      = 5.0f;
inline constexpr float kFragmentTimeout        = 5.0f;
inline constexpr std::size_t kMaxClients       = 16;

inline constexpr std::uint8_t kMsgFragment        = 0x00;
inline constexpr std::uint8_t kMsgConnectRequest  = 0x01;
inline constexpr std::uint8_t kMsgConnectAccept   = 0x02;
inline constexpr std::uint8_t kMsgConnectComplete = 0x03;
inline constexpr std::uint8_t kMsgDisconnect      = 0x04;

enum class Channel : std::uint8_t {
	Unreliable = 0,
	Reliable   = 1,
};

enum class SessionState { Disconnected, Connecting, Connected, Disconnecting };

enum class DisconnectReason : std::uint8_t {
	None             = 0,
	VersionMismatch  = 1,
	ServerFull       = 2,
	Timeout          = 3,
	DuplicateHandshake = 4,
	BadToken         = 5,
};

std::uint32_t fnv1a32(const char* data, std::size_t len);
std::uint32_t fnv1a32(const std::string& text);

} // namespace lotus

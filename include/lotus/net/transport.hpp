#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lotus {

struct Endpoint {
	std::string address;
	std::uint16_t port{0};
	bool operator==(const Endpoint& other) const;
};

struct Datagram {
	Endpoint from;
	std::vector<std::uint8_t> bytes;
};

class Transport {
public:
	virtual ~Transport() = default;

	virtual bool send(const Endpoint& to,
	                  std::span<const std::uint8_t> bytes) = 0;

	virtual std::vector<Datagram> poll() = 0;

	virtual std::optional<Endpoint> local_endpoint() const = 0;

	virtual const char* name() const = 0;
};

} // namespace lotus

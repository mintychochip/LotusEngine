#pragma once

#include "lotus/net/transport.hpp"

namespace lotus {

/// Non-blocking IPv4 UDP socket. Poll once per frame; nothing here blocks.
class UdpTransport final : public Transport {
public:
	UdpTransport();
	~UdpTransport() override;

	UdpTransport(const UdpTransport&)            = delete;
	UdpTransport& operator=(const UdpTransport&) = delete;
	UdpTransport(UdpTransport&&)                 = delete;
	UdpTransport& operator=(UdpTransport&&)      = delete;

	/// Bind 0.0.0.0. Port 0 asks the OS for an ephemeral port.
	bool bind(std::uint16_t port);
	void close();

	bool send(const Endpoint& to,
	          std::span<const std::uint8_t> bytes) override;
	std::vector<Datagram> poll() override;
	std::optional<Endpoint> local_endpoint() const override;
	const char* name() const override { return "udp"; }

private:
	int fd_{-1};
	Endpoint local_{};
	bool bound_{false};
};

} // namespace lotus

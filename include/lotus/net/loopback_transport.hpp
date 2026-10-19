#pragma once

#include "lotus/net/transport.hpp"

#include <memory>
#include <random>

namespace lotus {

struct LoopbackPolicy {
	float loss{0.0f};
	float duplicate{0.0f};
	float reorder{0.0f};
	std::uint32_t seed{1};
};

struct LoopbackPair {
	std::unique_ptr<class LoopbackTransport> a;
	std::unique_ptr<class LoopbackTransport> b;
};

LoopbackPair make_loopback_pair(const LoopbackPolicy& policy = {});

class LoopbackTransport final : public Transport {
public:
	LoopbackTransport() = default;
	~LoopbackTransport() override = default;

	void deliver();
	const char* name() const override { return "loopback"; }
	bool send(const Endpoint& to,
	          std::span<const std::uint8_t> bytes) override;
	std::vector<Datagram> poll() override;
	std::optional<Endpoint> local_endpoint() const override;
	void set_peer(LoopbackTransport* p, const Endpoint& local) {
		peer_  = p;
		local_ = local;
	}

	// Public for make_loopback_pair setup.
	// pending_ holds datagrams addressed here until deliver() applies loss,
	// duplicate, and reorder and moves the survivors into inbox_.
	std::vector<Datagram> pending_;
	std::vector<Datagram> inbox_;
	LoopbackTransport* peer_{nullptr};
	std::optional<Endpoint> local_;
	LoopbackPolicy policy_;
	std::mt19937 rng_;
};

} // namespace lotus

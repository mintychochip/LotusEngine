#include "lotus/net/loopback_transport.hpp"

#include <algorithm>

using namespace lotus;

namespace lotus {

LoopbackPair make_loopback_pair(const LoopbackPolicy& policy) {
	auto a = std::make_unique<LoopbackTransport>();
	auto b = std::make_unique<LoopbackTransport>();
	a->set_peer(b.get(), Endpoint{"127.0.0.1", 7777});
	b->set_peer(a.get(), Endpoint{"127.0.0.1", 7778});
	a->policy_ = policy;
	b->policy_ = policy;
	a->rng_.seed(policy.seed);
	b->rng_.seed(policy.seed);
	return {std::move(a), std::move(b)};
}

} // namespace lotus

bool LoopbackTransport::send(const Endpoint& to,
                             std::span<const std::uint8_t> bytes) {
	if (!peer_ || !peer_->local_ || to != *peer_->local_) return false;
	if (!local_) return false;
	Datagram d;
	d.from = *local_;
	d.bytes.assign(bytes.begin(), bytes.end());
	peer_->pending_.push_back(std::move(d));
	return true;
}

std::vector<Datagram> LoopbackTransport::poll() {
	std::vector<Datagram> out;
	out.swap(inbox_);
	return out;
}

std::optional<Endpoint> LoopbackTransport::local_endpoint() const {
	return local_;
}

void LoopbackTransport::deliver() {
	std::vector<Datagram> to_deliver;
	to_deliver.swap(pending_);

	std::vector<Datagram> processed;
	for (auto& d : to_deliver) {
		std::uniform_real_distribution<float> dist(0.0f, 1.0f);
		float roll = dist(rng_);

		if (roll < policy_.loss) continue;

		processed.push_back(std::move(d));

		if (dist(rng_) < policy_.duplicate) {
			processed.push_back(processed.back());
		}
	}

	if (policy_.reorder > 0.0f) {
		std::uniform_real_distribution<float> dist(0.0f, 1.0f);
		for (size_t i = 0; i + 1 < processed.size(); i += 2) {
			if (dist(rng_) < policy_.reorder) {
				std::swap(processed[i], processed[i + 1]);
			}
		}
	}

	for (auto& d : processed) {
		inbox_.push_back(std::move(d));
	}
}

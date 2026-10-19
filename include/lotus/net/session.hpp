#pragma once

#include "lotus/net/byte_buffer.hpp"
#include "lotus/net/protocol.hpp"
#include "lotus/net/transport.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace lotus {

/// One peer connection. A server Session accepts a single client; a client
/// Session dials one server. Poll-driven: call update() once per frame.
class Session {
public:
	enum class Role { Server, Client };

	Session(std::unique_ptr<Transport> transport, Role role);

	Session(const Session&)            = delete;
	Session& operator=(const Session&) = delete;

	bool connect(const std::string& host, std::uint16_t port);
	void disconnect();
	void accept_client(const Endpoint& endpoint);

	void update(float dt);
	void send_reliable(Channel channel, std::span<const std::uint8_t> payload);
	void send_unreliable(Channel channel, std::span<const std::uint8_t> payload);
	std::vector<std::vector<std::uint8_t>> received_reliable(Channel channel);
	std::vector<std::vector<std::uint8_t>> received_unreliable(Channel channel);

	SessionState state() const { return state_; }
	std::uint32_t local_client_id() const { return client_id_; }
	std::uint32_t remote_client_id() const { return remote_client_id_; }
	Transport* transport() { return transport_.get(); }
	void set_protocol_version(std::uint8_t version) { protocol_version_ = version; }

private:
	struct ReliableMsg {
		Channel channel{Channel::Reliable};
		std::uint16_t channel_seq{0};
		std::vector<std::uint8_t> payload;
		float since_sent{0.0f};
		int times_sent{0};
		bool acked{false};
		std::vector<std::uint16_t> in_datagrams;
	};

	struct UnreliableMsg {
		Channel channel{Channel::Unreliable};
		std::uint16_t channel_seq{0};
		std::vector<std::uint8_t> payload;
	};

	struct FragmentSet {
		std::uint32_t count{0};
		std::uint32_t total{0};
		std::vector<std::vector<std::uint8_t>> parts;
		std::vector<char> got;
		float age{0.0f};
	};

	static constexpr int kChannels = 2;

	std::unique_ptr<Transport> transport_;
	Role role_;
	SessionState state_{SessionState::Disconnected};
	std::uint8_t protocol_version_{kProtocolVersion};
	std::uint32_t client_id_{0};
	std::uint32_t remote_client_id_{0};
	std::uint32_t client_nonce_{0};
	std::uint32_t server_nonce_{0};
	std::uint16_t next_datagram_seq_{1};
	std::uint16_t next_reliable_seq_[kChannels]{1, 1};
	std::uint16_t next_unreliable_seq_[kChannels]{1, 1};
	std::uint16_t expect_reliable_[kChannels]{1, 1};
	float time_since_last_received_{0.0f};
	bool need_ack_{false};
	bool client_assigned_{false};
	bool complete_sent_{false};
	std::uint32_t next_fragment_id_{1};
	std::optional<Endpoint> peer_;

	std::vector<ReliableMsg> reliable_;
	std::vector<UnreliableMsg> unreliable_out_;
	std::map<std::uint16_t, std::vector<std::uint8_t>> reorder_[kChannels];
	std::map<std::uint32_t, FragmentSet> fragments_;
	std::vector<std::vector<std::uint8_t>> received_reliable_[kChannels];
	std::vector<std::vector<std::uint8_t>> received_unreliable_[kChannels];
	std::vector<std::uint16_t> received_datagrams_;

	static int channel_index(Channel channel);
	std::uint16_t allocate_datagram_seq();
	void enqueue_reliable(Channel channel, std::vector<std::uint8_t> payload);
	void fail(DisconnectReason reason);
	void poll_transport();
	void handle_datagram(const Datagram& datagram);
	void apply_ack(std::uint32_t ack, std::uint32_t ack_bits);
	void note_received_datagram(std::uint16_t sequence);
	void write_ack(ByteWriter& writer) const;
	void on_reliable(Channel channel,
	                 std::uint16_t sequence,
	                 std::vector<std::uint8_t> payload);
	void dispatch(Channel channel, std::vector<std::uint8_t> payload);
	bool try_fragment(Channel channel, const std::vector<std::uint8_t>& payload);
	bool try_handshake(const std::vector<std::uint8_t>& payload);
	void queue_accept();
	void queue_complete();
	void send_raw_disconnect(const Endpoint& to, DisconnectReason reason);
	void flush();
	void send_datagram(std::uint8_t flags,
	                   Channel channel,
	                   std::uint16_t channel_seq,
	                   std::span<const std::uint8_t> payload,
	                   ReliableMsg* owner);
};

} // namespace lotus

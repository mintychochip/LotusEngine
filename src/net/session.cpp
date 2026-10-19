#include "lotus/net/session.hpp"

#include <algorithm>

namespace lotus {
namespace {

constexpr std::size_t kMaxTrackedDatagrams = 128;
constexpr std::uint16_t kMaxReorderAhead   = 64;

bool more_recent(std::uint16_t a, std::uint16_t b)
{
	if (a == b) {
		return false;
	}
	const std::uint16_t diff = static_cast<std::uint16_t>(a - b);
	return diff < 32768;
}

bool is_before(std::uint16_t seq, std::uint16_t expect)
{
	const std::uint16_t diff = static_cast<std::uint16_t>(expect - seq);
	return diff != 0 && diff < 32768;
}

bool sequence_in(const std::vector<std::uint16_t>& seqs, std::uint16_t seq)
{
	return std::find(seqs.begin(), seqs.end(), seq) != seqs.end();
}

std::size_t varint_size(std::uint64_t value)
{
	std::size_t n = 1;
	while (value >= 128) {
		value >>= 7;
		++n;
	}
	return n;
}

std::size_t framed_size(std::size_t payload)
{
	return 1 + 2 + varint_size(payload) + payload;
}

} // namespace

int Session::channel_index(Channel channel)
{
	return channel == Channel::Unreliable ? 0 : 1;
}

Session::Session(std::unique_ptr<Transport> transport, Role role)
        : transport_(std::move(transport))
        , role_(role)
{
}

bool Session::connect(const std::string& host, std::uint16_t port)
{
	if (role_ != Role::Client || !transport_ || state_ != SessionState::Disconnected) {
		return false;
	}
	peer_                     = Endpoint{host, port};
	state_                    = SessionState::Connecting;
	time_since_last_received_ = 0.0f;
	client_nonce_             = 0xC0FFEE01u;
	ByteWriter payload;
	payload.u8(kMsgConnectRequest);
	payload.u8(protocol_version_);
	payload.u32(client_nonce_);
	payload.string("lotus");
	enqueue_reliable(Channel::Reliable, std::move(payload.data()));
	return true;
}

void Session::disconnect()
{
	if (state_ == SessionState::Disconnected) {
		return;
	}
	if (peer_) {
		send_raw_disconnect(*peer_, DisconnectReason::None);
	}
	fail(DisconnectReason::None);
}

void Session::accept_client(const Endpoint& endpoint)
{
	peer_ = endpoint;
}

void Session::update(float dt)
{
	if (!transport_) {
		return;
	}
	poll_transport();
	if (state_ == SessionState::Disconnected) {
		return;
	}

	time_since_last_received_ += dt;
	if (time_since_last_received_ >= kConnectionTimeout) {
		fail(DisconnectReason::Timeout);
		return;
	}

	for (auto& message : reliable_) {
		message.since_sent += dt;
	}
	for (auto it = fragments_.begin(); it != fragments_.end();) {
		it->second.age += dt;
		if (it->second.age >= kFragmentTimeout) {
			it = fragments_.erase(it);
		} else {
			++it;
		}
	}
	for (const auto& message : reliable_) {
		if (message.times_sent > kMaxResends) {
			fail(DisconnectReason::Timeout);
			return;
		}
	}
	flush();
}

void Session::send_reliable(Channel channel, std::span<const std::uint8_t> payload)
{
	if (state_ == SessionState::Disconnected) {
		return;
	}
	if (payload.size() <= kMaxFragment) {
		enqueue_reliable(channel,
		                 std::vector<std::uint8_t>(payload.begin(), payload.end()));
		return;
	}
	const std::uint32_t id    = next_fragment_id_++;
	const std::uint32_t count = static_cast<std::uint32_t>(
	        (payload.size() + kMaxFragment - 1) / kMaxFragment);
	for (std::uint32_t index = 0; index < count; ++index) {
		const std::size_t offset = static_cast<std::size_t>(index) * kMaxFragment;
		const std::size_t count_bytes =
		        std::min(kMaxFragment, payload.size() - offset);
		ByteWriter fragment;
		fragment.u8(kMsgFragment);
		fragment.varint(id);
		fragment.varint(index);
		fragment.varint(count);
		fragment.varint(payload.size());
		fragment.bytes(std::span<const std::uint8_t>(payload.data() + offset,
		                                             count_bytes));
		enqueue_reliable(channel, std::move(fragment.data()));
	}
}

void Session::send_unreliable(Channel channel, std::span<const std::uint8_t> payload)
{
	if (state_ == SessionState::Disconnected || payload.size() > kMaxFragment) {
		return;
	}
	UnreliableMsg message;
	message.channel = channel;
	const int index = channel_index(channel);
	message.channel_seq = next_unreliable_seq_[index]++;
	if (next_unreliable_seq_[index] == 0) {
		next_unreliable_seq_[index] = 1;
	}
	message.payload.assign(payload.begin(), payload.end());
	unreliable_out_.push_back(std::move(message));
}

std::vector<std::vector<std::uint8_t>> Session::received_reliable(Channel channel)
{
	auto& queue = received_reliable_[channel_index(channel)];
	std::vector<std::vector<std::uint8_t>> out;
	out.swap(queue);
	return out;
}

std::vector<std::vector<std::uint8_t>> Session::received_unreliable(Channel channel)
{
	auto& queue = received_unreliable_[channel_index(channel)];
	std::vector<std::vector<std::uint8_t>> out;
	out.swap(queue);
	return out;
}

std::uint16_t Session::allocate_datagram_seq()
{
	const std::uint16_t sequence = next_datagram_seq_;
	next_datagram_seq_ = static_cast<std::uint16_t>(next_datagram_seq_ + 1);
	if (next_datagram_seq_ == 0) {
		next_datagram_seq_ = 1;
	}
	return sequence;
}

void Session::enqueue_reliable(Channel channel, std::vector<std::uint8_t> payload)
{
	ReliableMsg message;
	message.channel     = channel;
	const int index     = channel_index(channel);
	message.channel_seq = next_reliable_seq_[index]++;
	if (next_reliable_seq_[index] == 0) {
		next_reliable_seq_[index] = 1;
	}
	message.payload    = std::move(payload);
	message.since_sent = kResendInterval;
	reliable_.push_back(std::move(message));
}

void Session::fail(DisconnectReason /*reason*/)
{
	state_ = SessionState::Disconnected;
	reliable_.clear();
	unreliable_out_.clear();
	need_ack_ = false;
}

void Session::poll_transport()
{
	const std::vector<Datagram> inbound = transport_->poll();
	for (const Datagram& datagram : inbound) {
		handle_datagram(datagram);
	}
}

void Session::handle_datagram(const Datagram& datagram)
{
	ByteReader reader(datagram.bytes);
	std::uint8_t version = 0;
	std::uint8_t flags   = 0;
	std::uint16_t sequence = 0;
	std::uint32_t ack      = 0;
	std::uint32_t ack_bits = 0;
	if (!reader.u8(version) || !reader.u8(flags) || !reader.u16(sequence) ||
	    !reader.u32(ack) || !reader.u32(ack_bits)) {
		return;
	}
	if (version != kProtocolVersion) {
		if (role_ == Role::Server) {
			send_raw_disconnect(datagram.from, DisconnectReason::VersionMismatch);
		}
		return;
	}
	if (peer_ && datagram.from != *peer_) {
		return;
	}
	if (!peer_) {
		if (role_ != Role::Server) {
			return;
		}
		peer_ = datagram.from;
	}

	apply_ack(ack, ack_bits);
	time_since_last_received_ = 0.0f;
	need_ack_                 = true;
	if (sequence_in(received_datagrams_, sequence)) {
		return;
	}
	note_received_datagram(sequence);
	if ((flags & 0x01) != 0) {
		return;
	}

	std::uint64_t count = 0;
	if (!reader.varint(count) || count > 32) {
		return;
	}
	for (std::uint64_t i = 0; i < count; ++i) {
		std::uint8_t channel_raw = 0;
		std::uint16_t channel_seq = 0;
		std::uint64_t length      = 0;
		if (!reader.u8(channel_raw) || !reader.u16(channel_seq) ||
		    !reader.varint(length)) {
			return;
		}
		if (length > kMtu) {
			return;
		}
		std::span<const std::uint8_t> bytes;
		if (!reader.bytes(static_cast<std::size_t>(length), bytes)) {
			return;
		}
		if (channel_raw > static_cast<std::uint8_t>(Channel::Reliable)) {
			continue;
		}
		const Channel channel = static_cast<Channel>(channel_raw);
		std::vector<std::uint8_t> payload(bytes.begin(), bytes.end());
		if (channel == Channel::Reliable) {
			on_reliable(channel, channel_seq, std::move(payload));
		} else {
			received_unreliable_[channel_index(channel)].push_back(
			        std::move(payload));
		}
	}
}

void Session::apply_ack(std::uint32_t ack, std::uint32_t ack_bits)
{
	if (ack == 0 && ack_bits == 0) {
		return;
	}
	const auto hit = [this](std::uint16_t sequence) {
		for (auto& message : reliable_) {
			if (sequence_in(message.in_datagrams, sequence)) {
				message.acked = true;
			}
		}
	};
	hit(static_cast<std::uint16_t>(ack));
	for (int bit = 0; bit < 32; ++bit) {
		if ((ack_bits & (1u << bit)) != 0) {
			hit(static_cast<std::uint16_t>(static_cast<std::uint16_t>(ack) -
			                               static_cast<std::uint16_t>(bit + 1)));
		}
	}
	std::erase_if(reliable_, [](const ReliableMsg& message) { return message.acked; });
}

void Session::note_received_datagram(std::uint16_t sequence)
{
	received_datagrams_.push_back(sequence);
	if (received_datagrams_.size() > kMaxTrackedDatagrams) {
		received_datagrams_.erase(received_datagrams_.begin());
	}
}

void Session::write_ack(ByteWriter& writer) const
{
	if (received_datagrams_.empty()) {
		writer.u32(0);
		writer.u32(0);
		return;
	}
	std::uint16_t highest = received_datagrams_.front();
	for (const std::uint16_t sequence : received_datagrams_) {
		if (more_recent(sequence, highest)) {
			highest = sequence;
		}
	}
	std::uint32_t bits = 0;
	for (int ago = 1; ago <= 32; ++ago) {
		const std::uint16_t prev =
		        static_cast<std::uint16_t>(highest - static_cast<std::uint16_t>(ago));
		if (sequence_in(received_datagrams_, prev)) {
			bits |= 1u << (ago - 1);
		}
	}
	writer.u32(highest);
	writer.u32(bits);
}

void Session::on_reliable(Channel channel,
                          std::uint16_t sequence,
                          std::vector<std::uint8_t> payload)
{
	const int index          = channel_index(channel);
	std::uint16_t& expect    = expect_reliable_[index];
	auto& buffer             = reorder_[index];
	if (sequence == expect) {
		dispatch(channel, std::move(payload));
		expect = static_cast<std::uint16_t>(expect + 1);
		if (expect == 0) {
			expect = 1;
		}
		while (true) {
			const auto found = buffer.find(expect);
			if (found == buffer.end()) {
				break;
			}
			dispatch(channel, std::move(found->second));
			buffer.erase(found);
			expect = static_cast<std::uint16_t>(expect + 1);
			if (expect == 0) {
				expect = 1;
			}
		}
		return;
	}
	if (is_before(sequence, expect)) {
		return;
	}
	const std::uint16_t ahead = static_cast<std::uint16_t>(sequence - expect);
	if (ahead > kMaxReorderAhead) {
		return;
	}
	buffer.emplace(sequence, std::move(payload));
}

void Session::dispatch(Channel channel, std::vector<std::uint8_t> payload)
{
	if (payload.empty()) {
		return;
	}
	if (payload[0] == kMsgFragment && try_fragment(channel, payload)) {
		return;
	}
	if (try_handshake(payload)) {
		return;
	}
	received_reliable_[channel_index(channel)].push_back(std::move(payload));
}

bool Session::try_fragment(Channel channel, const std::vector<std::uint8_t>& payload)
{
	ByteReader reader(payload);
	std::uint8_t type = 0;
	std::uint64_t id = 0;
	std::uint64_t index = 0;
	std::uint64_t count = 0;
	std::uint64_t total = 0;
	if (!reader.u8(type) || type != kMsgFragment) {
		return false;
	}
	if (!reader.varint(id) || !reader.varint(index) || !reader.varint(count) ||
	    !reader.varint(total)) {
		return false;
	}
	if (count == 0 || count > 64 || index >= count || total > 64 * kMaxFragment ||
	    id > 0xffffffffu) {
		return false;
	}
	std::span<const std::uint8_t> chunk;
	if (!reader.bytes(reader.remaining(), chunk) || reader.remaining() != 0) {
		return false;
	}

	FragmentSet& set = fragments_[static_cast<std::uint32_t>(id)];
	if (set.count == 0) {
		set.count = static_cast<std::uint32_t>(count);
		set.total = static_cast<std::uint32_t>(total);
		set.parts.resize(set.count);
		set.got.assign(set.count, 0);
	}
	if (set.count != count || set.total != total || index >= set.got.size()) {
		fragments_.erase(static_cast<std::uint32_t>(id));
		return true;
	}
	if (!set.got[static_cast<std::size_t>(index)]) {
		set.got[static_cast<std::size_t>(index)] = 1;
		set.parts[static_cast<std::size_t>(index)].assign(chunk.begin(), chunk.end());
	}
	for (const char have : set.got) {
		if (!have) {
			return true;
		}
	}
	std::vector<std::uint8_t> whole;
	whole.reserve(set.total);
	for (const auto& part : set.parts) {
		whole.insert(whole.end(), part.begin(), part.end());
	}
	fragments_.erase(static_cast<std::uint32_t>(id));
	if (whole.size() == total) {
		received_reliable_[channel_index(channel)].push_back(std::move(whole));
	}
	return true;
}

bool Session::try_handshake(const std::vector<std::uint8_t>& payload)
{
	if (payload.empty()) {
		return false;
	}
	const std::uint8_t type = payload[0];
	if (type != kMsgConnectRequest && type != kMsgConnectAccept &&
	    type != kMsgConnectComplete && type != kMsgDisconnect) {
		return false;
	}
	ByteReader reader(payload);
	std::uint8_t tag = 0;
	if (!reader.u8(tag)) {
		return true;
	}
	if (tag == kMsgDisconnect) {
		std::uint8_t reason = 0;
		if (!reader.u8(reason) || reader.remaining() != 0) {
			return true;
		}
		fail(static_cast<DisconnectReason>(reason));
		return true;
	}
	if (tag == kMsgConnectRequest) {
		std::uint8_t version = 0;
		std::uint32_t nonce  = 0;
		std::string name;
		if (!reader.u8(version) || !reader.u32(nonce) || !reader.string(name) ||
		    reader.remaining() != 0) {
			return true;
		}
		if (role_ != Role::Server) {
			return true;
		}
		if (version != kProtocolVersion) {
			if (peer_) {
				send_raw_disconnect(*peer_, DisconnectReason::VersionMismatch);
			}
			return true;
		}
		if (client_assigned_) {
			return true;
		}
		client_nonce_      = nonce;
		remote_client_id_  = 1;
		server_nonce_      = 0x0051u;
		client_assigned_   = true;
		state_             = SessionState::Connected;
		queue_accept();
		return true;
	}
	if (tag == kMsgConnectAccept) {
		std::uint32_t id    = 0;
		std::uint32_t nonce = 0;
		float tick_rate     = 0.0f;
		if (!reader.u32(id) || !reader.u32(nonce) || !reader.f32(tick_rate) ||
		    reader.remaining() != 0) {
			return true;
		}
		if (role_ != Role::Client || id == 0) {
			return true;
		}
		client_id_    = id;
		server_nonce_ = nonce;
		if (!complete_sent_) {
			state_ = SessionState::Connected;
			queue_complete();
			complete_sent_ = true;
		}
		return true;
	}
	std::uint32_t nonce = 0;
	if (!reader.u32(nonce) || reader.remaining() != 0) {
		return true;
	}
	if (role_ == Role::Server && nonce == client_nonce_) {
		state_ = SessionState::Connected;
	}
	return true;
}

void Session::queue_accept()
{
	ByteWriter payload;
	payload.u8(kMsgConnectAccept);
	payload.u32(remote_client_id_);
	payload.u32(server_nonce_);
	payload.f32(60.0f);
	enqueue_reliable(Channel::Reliable, std::move(payload.data()));
}

void Session::queue_complete()
{
	ByteWriter payload;
	payload.u8(kMsgConnectComplete);
	payload.u32(client_nonce_);
	enqueue_reliable(Channel::Reliable, std::move(payload.data()));
}

void Session::send_raw_disconnect(const Endpoint& to, DisconnectReason reason)
{
	if (!transport_) {
		return;
	}
	ByteWriter body;
	body.u8(kMsgDisconnect);
	body.u8(static_cast<std::uint8_t>(reason));

	ByteWriter packet;
	packet.u8(kProtocolVersion);
	packet.u8(0);
	packet.u16(1);
	packet.u32(0);
	packet.u32(0);
	packet.varint(1);
	packet.u8(static_cast<std::uint8_t>(Channel::Reliable));
	packet.u16(1);
	packet.varint(body.size());
	packet.bytes(std::span<const std::uint8_t>(body.data().data(), body.size()));
	transport_->send(to, packet.data());
}

void Session::flush()
{
	if (!peer_ || state_ == SessionState::Disconnected) {
		return;
	}
	for (auto& message : reliable_) {
		if (message.since_sent < kResendInterval) {
			continue;
		}
		if (framed_size(message.payload.size()) + 13 > kMtu) {
			continue;
		}
		send_datagram(0,
		              message.channel,
		              message.channel_seq,
		              message.payload,
		              &message);
	}
	for (const auto& message : unreliable_out_) {
		if (framed_size(message.payload.size()) + 13 > kMtu) {
			continue;
		}
		send_datagram(0, message.channel, message.channel_seq, message.payload, nullptr);
	}
	unreliable_out_.clear();
	if (need_ack_) {
		send_datagram(0x01, Channel::Reliable, 0, {}, nullptr);
		need_ack_ = false;
	}
}

void Session::send_datagram(std::uint8_t flags,
                            Channel channel,
                            std::uint16_t channel_seq,
                            std::span<const std::uint8_t> payload,
                            ReliableMsg* owner)
{
	if (!peer_ || !transport_) {
		return;
	}
	const std::uint16_t sequence = allocate_datagram_seq();
	ByteWriter packet;
	packet.u8(protocol_version_);
	packet.u8(flags);
	packet.u16(sequence);
	write_ack(packet);
	if ((flags & 0x01) == 0) {
		packet.varint(1);
		packet.u8(static_cast<std::uint8_t>(channel));
		packet.u16(channel_seq);
		packet.varint(payload.size());
		if (!payload.empty()) {
			packet.bytes(payload);
		}
	}
	if (packet.size() > kMtu) {
		return;
	}
	if (!transport_->send(*peer_, packet.data())) {
		return;
	}
	if (owner != nullptr) {
		owner->since_sent = 0.0f;
		owner->times_sent += 1;
		owner->in_datagrams.push_back(sequence);
	}
}

} // namespace lotus

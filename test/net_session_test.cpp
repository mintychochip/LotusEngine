#include <gtest/gtest.h>

#include "lotus/net/loopback_transport.hpp"
#include "lotus/net/session.hpp"

using namespace lotus;

namespace {

void pump(Session& client, Session& server, int ticks)
{
	for (int i = 0; i < ticks; ++i) {
		client.update(0.016f);
		server.update(0.016f);
		static_cast<LoopbackTransport*>(client.transport())->deliver();
		static_cast<LoopbackTransport*>(server.transport())->deliver();
	}
}

bool connect_pair(Session& client, Session& server)
{
	if (!client.connect("127.0.0.1", 7777)) {
		return false;
	}
	for (int i = 0; i < 100 && client.state() != SessionState::Connected; ++i) {
		client.update(0.016f);
		server.update(0.016f);
		static_cast<LoopbackTransport*>(client.transport())->deliver();
		static_cast<LoopbackTransport*>(server.transport())->deliver();
	}
	return client.state() == SessionState::Connected &&
	       server.state() == SessionState::Connected;
}

} // namespace

TEST(SessionTest, HandshakeCompletes) {
	auto [ta, tb] = make_loopback_pair();
	Session server(std::move(ta), Session::Role::Server);
	Session client(std::move(tb), Session::Role::Client);

	EXPECT_TRUE(client.connect("127.0.0.1", 7777));
	EXPECT_EQ(client.state(), SessionState::Connecting);

	for (int i = 0; i < 100 && client.state() != SessionState::Connected; ++i) {
		client.update(0.016f);
		server.update(0.016f);
		static_cast<LoopbackTransport*>(client.transport())->deliver();
		static_cast<LoopbackTransport*>(server.transport())->deliver();
	}
	EXPECT_EQ(client.state(), SessionState::Connected);
	EXPECT_EQ(server.state(), SessionState::Connected);
	EXPECT_GT(client.local_client_id(), 0u);
}

TEST(SessionTest, ReliableMessageSurvivesLoss) {
	LoopbackPolicy policy;
	policy.loss = 0.3f;
	policy.seed = 42;
	auto [ta, tb] = make_loopback_pair(policy);
	Session server(std::move(ta), Session::Role::Server);
	Session client(std::move(tb), Session::Role::Client);
	ASSERT_TRUE(connect_pair(client, server));

	ByteWriter w;
	w.string("reliable payload");
	server.send_reliable(Channel::Reliable, w.data());

	bool received = false;
	for (int i = 0; i < 200 && !received; ++i) {
		server.update(0.016f);
		client.update(0.016f);
		static_cast<LoopbackTransport*>(server.transport())->deliver();
		static_cast<LoopbackTransport*>(client.transport())->deliver();
		auto msgs = client.received_reliable(Channel::Reliable);
		if (!msgs.empty()) {
			received = true;
		}
	}
	EXPECT_TRUE(received);
}

TEST(SessionTest, FragmentationReassembles) {
	auto [ta, tb] = make_loopback_pair();
	Session server(std::move(ta), Session::Role::Server);
	Session client(std::move(tb), Session::Role::Client);
	ASSERT_TRUE(connect_pair(client, server));

	ByteWriter w;
	std::string big(3000, 'X');
	w.string(big);
	server.send_reliable(Channel::Reliable, w.data());

	bool received = false;
	for (int i = 0; i < 200 && !received; ++i) {
		server.update(0.016f);
		client.update(0.016f);
		static_cast<LoopbackTransport*>(server.transport())->deliver();
		static_cast<LoopbackTransport*>(client.transport())->deliver();
		auto msgs = client.received_reliable(Channel::Reliable);
		if (!msgs.empty()) {
			ByteReader r(msgs[0]);
			std::string s;
			EXPECT_TRUE(r.string(s));
			EXPECT_EQ(s, big);
			received = true;
		}
	}
	EXPECT_TRUE(received);
}

TEST(SessionTest, VersionMismatchRejected) {
	auto [ta, tb] = make_loopback_pair();
	Session server(std::move(ta), Session::Role::Server);
	Session client(std::move(tb), Session::Role::Client);
	client.set_protocol_version(99);
	client.connect("127.0.0.1", 7777);

	pump(client, server, 50);
	EXPECT_EQ(client.state(), SessionState::Disconnected);
}

TEST(SessionTest, ConnectionTimeout) {
	auto [ta, tb] = make_loopback_pair();
	Session server(std::move(ta), Session::Role::Server);
	Session client(std::move(tb), Session::Role::Client);
	ASSERT_TRUE(connect_pair(client, server));

	for (int i = 0; i < 400; ++i) {
		client.update(0.016f);
		server.update(0.016f);
	}
	EXPECT_EQ(client.state(), SessionState::Disconnected);
}

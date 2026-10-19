#include <gtest/gtest.h>

#include "lotus/net/udp_transport.hpp"

using namespace lotus;

TEST(UdpTest, BindAndSend) {
	UdpTransport server;
	EXPECT_TRUE(server.bind(0));
	auto ep = server.local_endpoint();
	ASSERT_TRUE(ep.has_value());

	UdpTransport client;
	EXPECT_TRUE(client.bind(0));

	std::vector<std::uint8_t> payload = {1, 2, 3, 4};
	EXPECT_TRUE(client.send(*ep, payload));

	auto dgrams = server.poll();
	ASSERT_EQ(dgrams.size(), 1u);
	EXPECT_EQ(dgrams[0].bytes, payload);
}

TEST(UdpTest, OversizedDatagramDropped) {
	UdpTransport a;
	UdpTransport b;
	ASSERT_TRUE(a.bind(0));
	ASSERT_TRUE(b.bind(0));
	auto ep = b.local_endpoint();
	ASSERT_TRUE(ep.has_value());

	std::vector<std::uint8_t> big(2000, 0xFF);
	a.send(*ep, big);
	auto dgrams = b.poll();
	EXPECT_TRUE(dgrams.empty());
}

TEST(UdpTest, Name) {
	UdpTransport t;
	EXPECT_STREQ(t.name(), "udp");
}

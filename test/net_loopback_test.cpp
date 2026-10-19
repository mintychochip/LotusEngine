#include <gtest/gtest.h>

#include <span>

#include "lotus/net/loopback_transport.hpp"

using namespace lotus;

TEST(LoopbackTest, DatagramsArriveAfterDeliver) {
	auto [a, b] = make_loopback_pair();
	Endpoint b_ep{"127.0.0.1", 7778};
	std::vector<std::uint8_t> payload = {1, 2, 3};
	EXPECT_TRUE(a->send(b_ep, payload));
	EXPECT_TRUE(b->poll().empty());
	b->deliver();
	auto dgrams = b->poll();
	ASSERT_EQ(dgrams.size(), 1u);
	EXPECT_EQ(dgrams[0].from, (Endpoint{"127.0.0.1", 7777}));
	EXPECT_EQ(dgrams[0].bytes, payload);
}

TEST(LoopbackTest, SeededLossPolicy) {
	LoopbackPolicy policy;
	policy.loss = 1.0f;
	policy.seed = 42;
	auto [a, b] = make_loopback_pair(policy);
	Endpoint b_ep{"127.0.0.1", 7778};
	a->send(b_ep, std::span<const std::uint8_t>({1}));
	b->deliver();
	EXPECT_TRUE(b->poll().empty());
}

TEST(LoopbackTest, DuplicatePolicy) {
	LoopbackPolicy policy;
	policy.duplicate = 1.0f;
	policy.seed = 42;
	auto [a, b] = make_loopback_pair(policy);
	Endpoint b_ep{"127.0.0.1", 7778};
	a->send(b_ep, std::span<const std::uint8_t>({1, 2}));
	b->deliver();
	auto dgrams = b->poll();
	EXPECT_EQ(dgrams.size(), 2u);
}

TEST(LoopbackTest, ReorderPolicy) {
	LoopbackPolicy policy;
	policy.reorder = 1.0f;
	policy.seed = 42;
	auto [a, b] = make_loopback_pair(policy);
	Endpoint b_ep{"127.0.0.1", 7778};
	a->send(b_ep, std::span<const std::uint8_t>({1}));
	a->send(b_ep, std::span<const std::uint8_t>({2}));
	b->deliver();
	auto dgrams = b->poll();
	ASSERT_EQ(dgrams.size(), 2u);
	EXPECT_EQ(dgrams[0].bytes[0], 2);
	EXPECT_EQ(dgrams[1].bytes[0], 1);
}

TEST(LoopbackTest, LocalEndpoint) {
	auto [a, b] = make_loopback_pair();
	auto ep = a->local_endpoint();
	ASSERT_TRUE(ep.has_value());
	EXPECT_EQ(ep->address, "127.0.0.1");
	EXPECT_EQ(ep->port, 7777);
}

TEST(LoopbackTest, Name) {
	auto [a, b] = make_loopback_pair();
	EXPECT_STREQ(a->name(), "loopback");
}

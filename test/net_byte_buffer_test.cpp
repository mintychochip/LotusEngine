#include <gtest/gtest.h>

#include "lotus/net/byte_buffer.hpp"

using namespace lotus;

TEST(ByteBufferTest, U8RoundTrip) {
	ByteWriter w;
	w.u8(0x42);
	ByteReader r(w.data());
	std::uint8_t v = 0;
	EXPECT_TRUE(r.u8(v));
	EXPECT_EQ(v, 0x42);
	EXPECT_TRUE(r.ok());
}

TEST(ByteBufferTest, U16RoundTrip) {
	ByteWriter w;
	w.u16(0xBEEF);
	ByteReader r(w.data());
	std::uint16_t v = 0;
	EXPECT_TRUE(r.u16(v));
	EXPECT_EQ(v, 0xBEEF);
}

TEST(ByteBufferTest, U32RoundTrip) {
	ByteWriter w;
	w.u32(0xDEADBEEF);
	ByteReader r(w.data());
	std::uint32_t v = 0;
	EXPECT_TRUE(r.u32(v));
	EXPECT_EQ(v, 0xDEADBEEF);
}

TEST(ByteBufferTest, U64RoundTrip) {
	ByteWriter w;
	w.u64(0x123456789ABCDEF0ULL);
	ByteReader r(w.data());
	std::uint64_t v = 0;
	EXPECT_TRUE(r.u64(v));
	EXPECT_EQ(v, 0x123456789ABCDEF0ULL);
}

TEST(ByteBufferTest, VarintBoundaries) {
	ByteWriter w;
	w.varint(0);
	w.varint(127);
	w.varint(128);
	w.varint(16383);
	w.varint(16384);
	w.varint(UINT32_MAX);
	w.varint(UINT64_MAX);
	ByteReader r(w.data());
	std::uint64_t v = 0;
	EXPECT_TRUE(r.varint(v));
	EXPECT_EQ(v, 0u);
	EXPECT_TRUE(r.varint(v));
	EXPECT_EQ(v, 127u);
	EXPECT_TRUE(r.varint(v));
	EXPECT_EQ(v, 128u);
	EXPECT_TRUE(r.varint(v));
	EXPECT_EQ(v, 16383u);
	EXPECT_TRUE(r.varint(v));
	EXPECT_EQ(v, 16384u);
	EXPECT_TRUE(r.varint(v));
	EXPECT_EQ(v, UINT32_MAX);
	EXPECT_TRUE(r.varint(v));
	EXPECT_EQ(v, UINT64_MAX);
}

TEST(ByteBufferTest, F32RoundTrip) {
	ByteWriter w;
	w.f32(3.14159f);
	ByteReader r(w.data());
	float v = 0;
	EXPECT_TRUE(r.f32(v));
	EXPECT_NEAR(v, 3.14159f, 1e-6f);
}

TEST(ByteBufferTest, StringRoundTrip) {
	ByteWriter w;
	w.string("hello world");
	ByteReader r(w.data());
	std::string s;
	EXPECT_TRUE(r.string(s));
	EXPECT_EQ(s, "hello world");
}

TEST(ByteBufferTest, TruncatedReadFails) {
	ByteWriter w;
	w.u32(0xDEADBEEF);
	auto data = w.data();
	data.resize(2);
	ByteReader r(data);
	std::uint32_t v = 0;
	EXPECT_FALSE(r.u32(v));
	EXPECT_FALSE(r.ok());
}

TEST(ByteBufferTest, OversizedStringRejected) {
	ByteWriter w;
	w.varint(0xFFFFFFFF);
	w.u8(0);
	ByteReader r(w.data());
	std::string s;
	EXPECT_FALSE(r.string(s));
	EXPECT_FALSE(r.ok());
}

TEST(ByteBufferTest, PatchU16) {
	ByteWriter w;
	w.u16(0);
	w.u16(0xBEEF);
	w.patch_u16(0, 0x1234);
	ByteReader r(w.data());
	std::uint16_t v = 0;
	EXPECT_TRUE(r.u16(v));
	EXPECT_EQ(v, 0x1234);
	EXPECT_TRUE(r.u16(v));
	EXPECT_EQ(v, 0xBEEF);
}

#include <gtest/gtest.h>

#include "lotus/net/byte_buffer.hpp"

#include <glm/gtc/quaternion.hpp>

#include <random>

using namespace lotus;

TEST(QuantisationTest, PositionWithinResolution) {
	ByteWriter w;
	w.position({1.0f, -2.5f, 100.0f});
	ByteReader r(w.data());
	glm::vec3 v;
	EXPECT_TRUE(r.position(v));
	EXPECT_NEAR(v.x, 1.0f, 0.02f);
	EXPECT_NEAR(v.y, -2.5f, 0.02f);
	EXPECT_NEAR(v.z, 100.0f, 0.02f);
}

TEST(QuantisationTest, PositionClampsOutOfRange) {
	ByteWriter w;
	w.position({1000.0f, -1000.0f, 0.0f});
	ByteReader r(w.data());
	glm::vec3 v;
	EXPECT_TRUE(r.position(v));
	EXPECT_NEAR(v.x, 512.0f, 0.02f);
	EXPECT_NEAR(v.y, -512.0f, 0.02f);
}

TEST(QuantisationTest, RotationRoundTrip) {
	ByteWriter w;
	glm::quat q = glm::angleAxis(1.234f, glm::vec3{0.3f, 0.7f, 0.6f});
	q            = glm::normalize(q);
	w.rotation(q);
	ByteReader r(w.data());
	glm::quat v;
	EXPECT_TRUE(r.rotation(v));
	float dot = std::abs(glm::dot(q, v));
	EXPECT_NEAR(dot, 1.0f, 0.01f);
}

TEST(QuantisationTest, RotationAllLargestComponents) {
	for (int largest = 0; largest < 4; ++largest) {
		float comps[4] = {0.1f, 0.2f, 0.3f, 0.4f};
		comps[largest] = 0.9f;
		if (largest % 2 == 1) comps[largest] = -0.9f;
		glm::quat q = glm::normalize(glm::quat(comps[3], comps[0], comps[1], comps[2]));
		ByteWriter w;
		w.rotation(q);
		ByteReader r(w.data());
		glm::quat v;
		EXPECT_TRUE(r.rotation(v));
		float dot = std::abs(glm::dot(q, v));
		EXPECT_NEAR(dot, 1.0f, 0.01f) << "largest=" << largest;
	}
}

TEST(QuantisationTest, ZeroQuaternionNoNaN) {
	ByteWriter w;
	w.rotation(glm::quat(0, 0, 0, 0));
	ByteReader r(w.data());
	glm::quat v;
	EXPECT_TRUE(r.rotation(v));
	EXPECT_FALSE(std::isnan(v.x) || std::isnan(v.y) || std::isnan(v.z) ||
	             std::isnan(v.w));
}

TEST(QuantisationTest, RandomRotationsRoundTrip) {
	std::mt19937 rng(42);
	std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
	for (int i = 0; i < 100; ++i) {
		glm::quat q(dist(rng), dist(rng), dist(rng), dist(rng));
		q = glm::normalize(q);
		ByteWriter w;
		w.rotation(q);
		ByteReader r(w.data());
		glm::quat v;
		ASSERT_TRUE(r.rotation(v));
		float dot = std::abs(glm::dot(q, v));
		EXPECT_NEAR(dot, 1.0f, 0.01f) << "iteration " << i;
	}
}

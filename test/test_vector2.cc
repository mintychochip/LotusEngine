#include <gtest/gtest.h>
#include "math.hpp"

using vec2i = Vector2<int>;
using vec2f = Vector2<float>;

TEST(Vector2, DefaultConstructor) {
    vec2i v;
    EXPECT_EQ(v[0], 0);
    EXPECT_EQ(v[1], 0);
}

TEST(Vector2, ParameterizedConstructorAndAccess) {
    vec2i v(3, 4);
    EXPECT_EQ(v[0], 3);
    EXPECT_EQ(v[1], 4);
}

TEST(Vector2, IndexOutOfBounds) {
    vec2i v(1, 2);
    EXPECT_THROW(v[2], std::runtime_error);
}

TEST(Vector2, AdditionOperator) {
    vec2i a(1, 2), b(3, 4);
    vec2i result = a + b;
    EXPECT_EQ(result[0], 4);
    EXPECT_EQ(result[1], 6);
}

TEST(Vector2, SubtractionOperator) {
    vec2i a(5, 7), b(2, 3);
    vec2i result = a - b;
    EXPECT_EQ(result[0], 3);
    EXPECT_EQ(result[1], 4);
}

TEST(Vector2, ElementWiseMultiplicationOperator) {
    vec2i a(2, 3), b(4, 5);
    vec2i result = a * b;
    EXPECT_EQ(result[0], 8);
    EXPECT_EQ(result[1], 15);
}

TEST(Vector2, ScalarMultiplicationOperator) {
    vec2i a(3, 4);
    vec2i result = a * 2;
    EXPECT_EQ(result[0], 6);
    EXPECT_EQ(result[1], 8);
}

TEST(Vector2, CompoundAdd) {
    vec2i a(1, 2), b(3, 4);
    a += b;
    EXPECT_EQ(a[0], 4);
    EXPECT_EQ(a[1], 6);
}

TEST(Vector2, CompoundSubtract) {
    vec2i a(5, 6), b(2, 3);
    a -= b;
    EXPECT_EQ(a[0], 3);
    EXPECT_EQ(a[1], 3);
}

TEST(Vector2, CompoundElementWiseMultiply) {
    vec2i a(2, 3), b(4, 5);
    a *= b;
    EXPECT_EQ(a[0], 8);
    EXPECT_EQ(a[1], 15);
}

TEST(Vector2, CompoundScalarMultiply) {
    vec2i a(2, 3);
    a *= 3;
    EXPECT_EQ(a[0], 6);
    EXPECT_EQ(a[1], 9);
}

TEST(Vector2, DotProduct) {
    vec2i a(1, 2), b(3, 4);
    EXPECT_EQ(a.dot(b), 11); // 1*3 + 2*4 = 11
}

TEST(Vector2, MagnitudeInt) {
    vec2i a(3, 4);
    EXPECT_EQ(a.magnitude(), 5); // sqrt(9 + 16) = 5
}

TEST(Vector2, MagnitudeFloat) {
    vec2f a(3.0f, 4.0f);
    EXPECT_FLOAT_EQ(a.magnitude(), 5.0f);
}
TEST(Vector2, ChainedAddition) {
    vec2i a(1, 1), b(2, 2), c(3, 3);
    vec2i result = a + b + c;
    EXPECT_EQ(result[0], 6);
    EXPECT_EQ(result[1], 6);
}

TEST(Vector2, ChainedSubtraction) {
    vec2i a(10, 10), b(3, 2), c(1, 1);
    vec2i result = a - b - c;
    EXPECT_EQ(result[0], 6);
    EXPECT_EQ(result[1], 7);
}

TEST(Vector2, SelfAssignment) {
    vec2i a(5, 7);
    a += a;
    EXPECT_EQ(a[0], 10);
    EXPECT_EQ(a[1], 14);
}

TEST(Vector2, OperatorOrderMatters) {
    vec2i a(1, 2), b(3, 4);
    vec2i ab = a + b;
    vec2i ba = b + a;
    EXPECT_EQ(ab[0], ba[0]);
    EXPECT_EQ(ab[1], ba[1]);
}

TEST(Vector2, CompoundChainedOps) {
    vec2i a(1, 1), b(2, 2), c(3, 3);
    a += b;
    a *= c;
    EXPECT_EQ(a[0], 9); // (1+2)*3
    EXPECT_EQ(a[1], 9);
}

TEST(Vector2, ScalarThenElementWiseMultiplication) {
    vec2i a(1, 2), b(2, 3);
    vec2i result = (a * 2) * b;
    EXPECT_EQ(result[0], 4);
    EXPECT_EQ(result[1], 12);
}

TEST(Vector2, OperatorPlusDoesNotMutateOperands) {
    vec2i a(1, 2), b(3, 4);
    vec2i result = a + b;
    EXPECT_EQ(a[0], 1);
    EXPECT_EQ(a[1], 2);
    EXPECT_EQ(b[0], 3);
    EXPECT_EQ(b[1], 4);
    EXPECT_EQ(result[0], 4);
    EXPECT_EQ(result[1], 6);
}

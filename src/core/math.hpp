#pragma once

#include <iostream>
#include <stdexcept>
#include <cmath>

template <typename T>
class Vector2
{
public:
    Vector2(T x, T y) : vec_{x, y} {}
    Vector2() : vec_{0, 0} {}
    Vector2<T> &operator+=(const Vector2<T> &vec)
    {
        vec_[0] += vec.vec_[0];
        vec_[1] += vec.vec_[1];
        return *this;
    }

    Vector2<T> &operator-=(const Vector2<T> &vec)
    {
        vec_[0] -= vec.vec_[0];
        vec_[1] -= vec.vec_[1];
        return *this;
    }

    Vector2<T> &operator*=(const Vector2<T> &vec)
    {
        vec_[0] *= vec.vec_[0];
        vec_[1] *= vec.vec_[1];
        return *this;
    }

    Vector2<T> &operator*=(T scalar)
    {
        vec_[0] *= scalar;
        vec_[1] *= scalar;
        return *this;
    }

    Vector2<T> operator+(const Vector2<T> &vec) const
    {
        return {vec_[0] + vec.vec_[0], vec_[1] + vec.vec_[1]};
    }

    Vector2<T> operator-(const Vector2<T> &vec) const
    {
        return {vec_[0] - vec.vec_[0], vec_[1] - vec.vec_[1]};
    }

    Vector2<T> operator*(const Vector2<T> &vec) const
    {
        return {vec_[0] * vec.vec_[0], vec_[1] * vec.vec_[1]};
    }

    Vector2<T> operator*(T scalar) const
    {
        return {vec_[0] * scalar, vec_[1] * scalar};
    }

    T &operator[](int index)
    {
        if (index < 0 || index >= 2)
            throw std::runtime_error("Index out of bounds\n");
        return vec_[index];
    }

    T operator[](int index) const
    {
        if (index < 0 || index >= 2)
            throw std::runtime_error("Index out of bounds\n");
        return vec_[index];
    }

    T dot(const Vector2<T>& vec) const
    {       
        return vec_[0] * vec.vec_[0] + vec_[1] * vec.vec_[1];
    }

    T magnitude() const
    {
        return std::sqrt(vec_[0] * vec_[0] + vec_[1] * vec_[1]);
    }

    Vector2<T> operator-() const
    {
        return {-vec_[0], -vec_[1]};
    }

    Vector2<T> normalize() const
    {
        T magnitude = magnitude();
        if (magnitude == 0) return {0,0};
        return *this * (1 / magnitude);
    }

    friend std::ostream &operator<<(std::ostream &out, const Vector2<T> &vec)
    {
        out << '(' << vec[0] << ", " << vec[1] << ')';
        return out;
    }

private:
    T vec_[2];
};

template <typename T>
class Vector3 {
public:
    Vector3(T x, T y, T z) : vec_{x, y, z} {}
    Vector3() : vec_{0, 0, 0} {}

    Vector3<T> operator+(const Vector3<T>& v) const {
        return {vec_[0] + v.vec_[0], vec_[1] + v.vec_[1], vec_[2] + v.vec_[2]};
    }

    Vector3<T> operator-(const Vector3<T>& v) const {
        return {vec_[0] - v.vec_[0], vec_[1] - v.vec_[1], vec_[2] - v.vec_[2]};
    }

    Vector3<T> operator*(T scalar) const {
        return {vec_[0] * scalar, vec_[1] * scalar, vec_[2] * scalar};
    }

    Vector3<T> operator-() const {
        return {-vec_[0], -vec_[1], -vec_[2]};
    }

    T dot(const Vector3<T>& v) const {
        return vec_[0]*v.vec_[0] + vec_[1]*v.vec_[1] + vec_[2]*v.vec_[2];
    }

    Vector3<T> cross(const Vector3<T>& v) const {
        return {
            vec_[1]*v.vec_[2] - vec_[2]*v.vec_[1],
            vec_[2]*v.vec_[0] - vec_[0]*v.vec_[2],
            vec_[0]*v.vec_[1] - vec_[1]*v.vec_[0]
        };
    }

    T magnitude() const {
        return std::sqrt(vec_[0]*vec_[0] + vec_[1]*vec_[1] + vec_[2]*vec_[2]);
    }

    Vector3<T> normalize() const {
        T mag = magnitude();
        if (mag == 0) return {0, 0, 0};
        return *this * (1 / mag);
    }

    T& operator[](int index) {
        if (index < 0 || index >= 3)
            throw std::runtime_error("Index out of bounds\n");
        return vec_[index];
    }

    T operator[](int index) const {
        if (index < 0 || index >= 3)
            throw std::runtime_error("Index out of bounds\n");
        return vec_[index];
    }

    friend std::ostream& operator<<(std::ostream& out, const Vector3<T>& v) {
        out << '(' << v[0] << ", " << v[1] << ", " << v[2] << ')';
        return out;
    }

private:
    T vec_[3];
};


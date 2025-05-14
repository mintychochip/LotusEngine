#include <stdexcept>

template <typename T>
class Vector2
{
    public:
        Vector2(T x, T y) : vec_{x,y} {}
        Vector2() : vec_{0,0} {}
        Vector2<T>& operator+=(Vector2<T>& vec)
        {
            vec_[0] += vec.vec_[0];
            vec_[1] += vec.vec_[1];
        }

        Vector2<T>& operator-=(Vector2<T>& vec)
        {
            vec_[0] -= vec.vec_[0];
            vec_[1] -= vec.vec_[1];
        }

        Vector2<T>& operator*=(Vector2<T>& vec)
        {
            vec_[0] *= vec.vec_[0];
            vec_[1] *= vec.vec_[1];
        }

        Vector2<T>& operator/=(Vector2<T>& vec)
        {
            if (vec.vec_[0] == 0 || vec.vec_[1])
                throw std::runtime_error("floating point exception\n");
            vec_[0] /= vec.vec_[0];
            vec_[1] /= vec.vec_[1];
        }

        Vector2<T> operator+(Vector2<T>& vec) const
        {
            return {vec_[0] + vec.vec_[0],vec_[1] + vec.vec_[1]};
        }

        Vector2<T> operator-(Vector2<T>& vec) const
        {
            return {vec_[0] - vec.vec_[0],vec_[1] - vec.vec_[1]};
        }

        Vector2<T> operator*(Vector2<T>& vec) const
        {
            return {vec_[0] * vec.vec_[0],vec_[1] * vec.vec_[1]};
        }

        Vector2<T> operator/(Vector2<T>& vec) const
        {
            if (vec.vec_[0] == 0 || vec.vec_[1] == 0)
               throw std::runtime_error("floating point exception\n"); 
            return {vec_[0] / vec.vec_[0],vec_[1] / vec.vec_[1]};
        }

        T& operator[](int index)
        {
            if (index > 1)   
                throw std::runtime_error("out of bounds\n");
            return vec_[index];
        }

        const T operator[](int index) const
        {
            if (index > 1)
                throw std::runtime_error("out of bounds\n");
            return vec_[index];
        }

    private:
        T vec_[2];
};


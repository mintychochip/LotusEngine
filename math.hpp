#include <stdexcept>
#include <cmath>

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
            return *this;
        }

        Vector2<T>& operator-=(Vector2<T>& vec)
        {
            vec_[0] -= vec.vec_[0];
            vec_[1] -= vec.vec_[1];
            return *this;
        }

        Vector2<T>& operator*=(Vector2<T>& vec)
        {
            vec_[0] *= vec.vec_[0];
            vec_[1] *= vec.vec_[1];
            return *this;
        }

        Vector2<T>& operator*=(T scalar)
        {   
            vec_[0] *= scalar;
            vec_[1] *= scalar;
            return *this;
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
        
        Vector2<T> operator*(T scalar) const
        {
            return {vec_[0] * scalar, vec_[1] * scalar};
        }

        T& operator[](int index)
        {
            if (index > 1)   
                throw std::runtime_error("out of bounds\n");
            return vec_[index];
        }

        T operator[](int index) const
        {
            if (index > 1)
                throw std::runtime_error("out of bounds\n");
            return vec_[index];
        }

        T dot(Vector2<T> vec) const
        {
            return vec_[0] * vec.vec_[0] + vec_[1] * vec.vec_[1]; 
        }
        
        T magnitude() const
        {
            return std::sqrt(vec_[0] * vec_[0] + vec_[1] * vec_[1]);
        }


    private:
        T vec_[2];
};


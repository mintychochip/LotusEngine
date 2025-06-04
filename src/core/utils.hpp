#pragma once

#include <cstdint>
#include "utils/types.hpp"
constexpr u32 KB(u32 x) { return x * 1024; }
constexpr u32 MB(u32 x) { return x * 1024 * 1024; }

class Global
{
public:
    static Global &get()
    {
        static Global instance;
        return instance;
    }
    
    Global &width(u32 width)
    {
        width_ = width;
        return *this;
    }

    u32 &width() { return width_; }

    Global &height(u32 height)
    {
        height_ = height;
        return *this;
    }

    u32 &height() { return height_; }

    float get_aspect() const
    {
        return static_cast<float>(width_) / static_cast<float>(height_);
    }

private:
    Global() {}
    u32 width_, height_;
};
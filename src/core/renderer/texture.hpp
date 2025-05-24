#pragma once

#include <glad/glad.h>
#include <glfw/glfw3.h>
#include <stb_image.h>
#include <string>

#include "core/assets/asset.hpp"

struct GLTextureSettings;

namespace texture_
{
    GLint format_from_channels(int channels)
    {
        if (channels == 4) {
            return GL_RGBA;
        } else if (channels == 3) {
            return GL_RGB;
        }
        return GL_RED;
    }
};
struct GLTextureSettings
{
    GLTextureSettings(GLint min_filter, GLint max_filter, GLint wrap_s, GLint wrap_t)
    {
        this->min_filter = min_filter;
        this->max_filter = max_filter;
        this->wrap_s = wrap_s;
        this->wrap_t = wrap_t;
    }
    GLint internal_format, min_filter, max_filter, wrap_s, wrap_t;
    GLTextureSettings &set_format(GLint internal_format)
    {
        this->internal_format = internal_format;
        return *this;
    }
    GLTextureSettings &set_setting(GLint setting, int value)
    {
        if (setting == GL_TEXTURE_WRAP_S)
        {
            wrap_s = value;
        }
        if (setting == GL_TEXTURE_WRAP_T)
        {
            wrap_t = value;
        }
        if (setting == GL_TEXTURE_MAG_FILTER)
        {
            max_filter = value;
        }
        if (setting == GL_TEXTURE_MIN_FILTER)
        {
            min_filter = value;
        }
        return *this;
    }
};

class GLTexture : public Asset
{
public:
    GLTexture(GLTexture&&) noexcept = default;
    GLTexture &operator=(GLTexture&&) noexcept = default;
    GLTexture(const std::string &path, GLTextureSettings &settings) : Asset{path}, settings_{settings}
    {
        glGenTextures(1, &id_);
        load();
    }

    void bind()
    {
        glBindTexture(GL_TEXTURE_2D, id_);
    }

    void unbind()
    {
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    void load()
    {
        unsigned char *buffer = stbi_load(path_.c_str(), &width_, &height_, &channels_, 0);
        glBindTexture(GL_TEXTURE_2D, id_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, settings_.min_filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, settings_.max_filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, settings_.wrap_s);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, settings_.wrap_t);
        image_format_ = texture_::format_from_channels(channels_);
        glTexImage2D(id_, 0, image_format_, width_, height_, 0, image_format_, GL_UNSIGNED_BYTE, buffer);
        glGenerateMipmap(id_);
        stbi_image_free(buffer);
    }

    void reload()
    {
        load();
    }

    ~GLTexture()
    {
        glDeleteTextures(1, &id_);
    }

private:
    int width_, height_, channels_;
    GLuint id_;
    GLint image_format_;
    GLTextureSettings settings_;
};

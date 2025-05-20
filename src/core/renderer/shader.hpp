#pragma once
#include <glad/glad.h>
#include <string>
#include <iostream>
#include <cstdint>
#include "core/math.hpp"

namespace shader_
{
    GLuint compile_shader(std::string &source, GLenum shader_type)
    {
        GLuint shader = glCreateShader(shader_type);
        const char *source_str = source.c_str();
        glShaderSource(shader, 1, &source_str, nullptr);
        glCompileShader(shader);
        GLint success;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success)
        {
            return 0;
        }
        return shader;
    }

    GLenum shader_type_to_gl(ShaderType shader_type)
    {
        if (shader_type == VERTEX)
        {
            return GL_VERTEX_SHADER;
        }
        else if (shader_type == FRAGMENT)
        {
            return GL_FRAGMENT_SHADER;
        }
        return GL_GEOMETRY_SHADER;
    }
};

enum ShaderType
{
    VERTEX,
    FRAGMENT,
    GEOMETRY,
};

class Shader
{
    using u32 = uint32_t;
    using vec2f = Vector2<float>;
    using vec2i = Vector2<int>;

public:
    static Shader create_shader(std::string &source, ShaderType shader_type)
    {
        GLenum gl_shader_type = shader_::shader_type_to_gl(shader_type);
        GLuint shader_id = shader_::compile_shader(source, gl_shader_type);
        if (!shader_id)
        {
            return Shader{0};
        }
    }

    void bind()
    {
        glUseProgram(shader_id_);
    }

    void unbind()
    {
        glUseProgram(0);
    }

    void set_float(const std::string &name, float v)
    {
        GLint location = glGetUniformLocation(shader_id_, name.c_str());
        if (location != -1)
        {
            glUniform1f(location, v);
        }
    }

    void set_float2(const std::string &name, vec2f &v)
    {
        GLint location = glGetUniformLocation(shader_id_, name.c_str());
        if (location != -1)
        {
            glUniform2f(location, v[0], v[1]);
        }
    }

    void set_int(const std::string &name, int v)
    {
        GLint location = glGetUniformLocation(shader_id_, name.c_str());
        if (location != -1)
        {
            glUniform1i(location, v);
        }
    }

    void set_int2(const std::string &name, vec2f &v)
    {
        GLint location = glGetUniformLocation(shader_id_, name.c_str());
        if (location != -1)
        {
            glUniform2i(location, v[0], v[1]);
        }
    }

private:
    Shader(GLuint shader_id) : shader_id_{shader_id} {}
    GLuint shader_id_;
};
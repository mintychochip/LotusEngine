#pragma once
#include <glad/glad.h>
#include <string>
#include <iostream>
#include <cstdint>
#include <fstream>
#include <sstream>
#include "core/math.hpp"

class Shader;
class ShaderProgram;

namespace shader_
{
    std::string read_file(const std::string &file_path)
    {
        std::ifstream in{file_path, std::ios::in};
        if (!in)
        {
            return "";
        }
        std::ostringstream content;
        content << in.rdbuf();
        return content.str();
    }

    GLuint compile_shader(std::string &source, GLenum shader_type)
    {
        GLuint shader = glCreateShader(shader_type);
        const char *source_str = source.c_str();
        glShaderSource(shader, 1, &source_str, nullptr);
        glCompileShader(shader);
        GLint status;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
        if (!status)
        {
            return 0;
        }
        return shader;
    }

    GLenum shader_type_to_gl(ShaderType shader_type)
    {
        switch (shader_type)
        {
        case VERTEX:
            return GL_VERTEX_SHADER;
        case FRAGMENT:
            return GL_FRAGMENT_SHADER;
        case GEOMETRY:
            return GL_GEOMETRY_SHADER;
        default:
            return 0;
        }
    }

    GLint get_uniform_location(GLuint shader_id, const std::string &name)
    {
        return glGetUniformLocation(shader_id, name.c_str());
    }

    GLuint link_program(Shader &vertex, Shader &fragment)
    {
        GLuint program = glCreateProgram();
        vertex.attach(program);
        fragment.attach(program);
        glLinkProgram(program);
        GLint status;
        glGetProgramiv(program, GL_LINK_STATUS, &status);
        if (!status)
        {
            return 0;
        }
        vertex.detach(program);
        fragment.detach(program);
        return program;
    }

    bool valid_shader(Shader &shader)
    {
        return shader.get_id();
    }

    bool invalid_program(const ShaderProgram &program)
    {
        return !(program.program_id() && program.fragment_id() && program.vertex_id());
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
public:
    static Shader create_shader(const std::string &file_path, ShaderType shader_type)
    {
        std::string shader_source = shader_::read_file(file_path);
        if (shader_source.empty())
        {
            return Shader{0};
        }
        GLenum gl_shader_type = shader_::shader_type_to_gl(shader_type);
        GLuint shader_id = shader_::compile_shader(shader_source, gl_shader_type);
        if (!shader_id)
        {
            return Shader{0};
        }
        return shader_id;
    }

    GLuint get_id() const
    {
        return shader_id_;
    }

    void attach(GLuint program)
    {
        glAttachShader(program, shader_id_);
    }

    void detach(GLuint program)
    {
        glDetachShader(program, shader_id_);
    }

    void destroy()
    {
        if (shader_id_)
        {
            glDeleteShader(shader_id_);
            shader_id_ = 0;
        }
    }

    ~Shader()
    {
        destroy();
    }

private:
    Shader(GLuint shader_id) : shader_id_{shader_id} {}
    GLuint shader_id_;
};

class ShaderProgram
{
    using u32 = uint32_t;

    using vec2f = Vector2<float>;
    using vec2i = Vector2<int>;

    using vec3f = Vector3<float>;
    using vec3i = Vector3<int>;

public:
    static ShaderProgram create(Shader &vertex, Shader &fragment)
    {
        GLuint program_id = shader_::link_program(vertex, fragment);
        if (!program_id)
        {
            return {0, 0, 0};
        }
        return {vertex.get_id(), fragment.get_id(), program_id};
    }
    void bind() const
    {
        glUseProgram(program_id_);
    }

    void unbind() const
    {
        glUseProgram(0);
    }

    void set_float(const std::string &name, float v) const
    {
        GLint location = shader_::get_uniform_location(program_id_, name);
        if (location != -1)
        {
            glUniform1f(location, v);
        }
    }

    void set_float2(const std::string &name, const vec2f &v) const
    {
        GLint location = shader_::get_uniform_location(program_id_, name);
        if (location != -1)
        {
            glUniform2f(location, v[0], v[1]);
        }
    }

    void set_float3(const std::string &name, const vec3f &v) const
    {
        GLint location = shader_::get_uniform_location(program_id_, name);
        if (location != -1)
        {
            glUniform3f(location, v[0], v[1], v[2]);
        }
    }

    void set_int(const std::string &name, int v) const
    {
        GLint location = shader_::get_uniform_location(program_id_, name);
        if (location != -1)
        {
            glUniform1i(location, v);
        }
    }

    void set_int2(const std::string &name, const vec2i &v) const
    {
        GLint location = shader_::get_uniform_location(program_id_, name);
        if (location != -1)
        {
            glUniform2i(location, v[0], v[1]);
        }
    }

    void set_int3(const std::string &name, const vec3i &v) const
    {
        GLint location = shader_::get_uniform_location(program_id_, name);
        if (location != -1)
        {
            glUniform3i(location, v[0], v[1], v[2]);
        }
    }

    void destroy()
    {
        if (program_id_)
        {
            glDeleteProgram(program_id_);
            program_id_ = 0;
        }
    }

    GLuint fragment_id() const
    {
        return fragment_;
    }

    GLuint vertex_id() const
    {
        return vertex_;
    }

    GLuint program_id() const
    {
        return program_id_;
    }

    ~ShaderProgram()
    {
        destroy();
    }

private:
    ShaderProgram(GLuint vertex, GLuint fragment, GLuint program_id)
        : vertex_{vertex}, fragment_{fragment}, program_id_{program_id} {}
    GLuint vertex_, fragment_, program_id_;
};
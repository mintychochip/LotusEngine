#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glad/glad.h>
#include <string>
#include <iostream>
#include <cstdint>
#include <fstream>
#include <sstream>
#include "core/allocator.hpp"
#include "core/assets/asset.hpp"

class Shader;
class ShaderProgram;

namespace shader_
{
    std::string read_file(const std::string &file_path);
    GLuint compile_shader(std::string &source, GLenum shader_type);
    GLint get_uniform_location(GLuint shader_id, const std::string &name);
    GLuint link_program(Shader *shaders[], GLuint count);
    bool invalid_shader(Shader &shader);
    bool invalid_program(const ShaderProgram &program);
}

class Shader : public Asset
{
public:
    Shader(const std::string &path, GLenum shader_type) : Asset{path}, shader_type_{shader_type} , shader_id_{0}
    {
        load();
    }

    void load()
    {
        std::cout << shader_type_ << ' ' << GL_FRAGMENT_SHADER << std::endl;
        if (shader_id_ != 0) {
            glDeleteShader(shader_id_);
            shader_id_ = 0;
        }
        std::string source = shader_::read_file(path_);
        if (source.empty())
            return;
        shader_id_ = shader_::compile_shader(source,shader_type_);
        std::cout << shader_id_ << std::endl;
    }

    GLuint get_id() const { return shader_id_; }

    void attach(GLuint program) { glAttachShader(program, shader_id_); }
    void detach(GLuint program) { glDetachShader(program, shader_id_); }

    ~Shader()
    {
        glDeleteShader(shader_id_);
        shader_id_ = 0;
    }
private:
    GLenum shader_type_;
    GLuint shader_id_;
};

class ShaderProgram
{
    using u32 = uint32_t;

public:
    static ShaderProgram create(Shader &vertex, Shader &fragment)
    {
        Shader *shaders[] = {&vertex, &fragment};
        std::cout << vertex.get_id() << ' ' << fragment.get_id();
        GLuint program_id = shader_::link_program(shaders, 2);
        if (!program_id)
            return {0, 0, 0};
        return {vertex.get_id(), fragment.get_id(), program_id};
    }

    void bind() const { glUseProgram(program_id_); }
    void unbind() const { glUseProgram(0); }

    void set_float(const std::string &name, float v) const
    {
        GLint location = shader_::get_uniform_location(program_id_, name);
        if (location != -1)
            glUniform1f(location, v);
    }

    void set_float2(const std::string &name, const glm::vec2 &v) const
    {
        GLint location = shader_::get_uniform_location(program_id_, name);
        if (location != -1)
            glUniform2f(location, v[0], v[1]);
    }

    void set_float3(const std::string &name, const glm::vec3 &v) const
    {
        GLint location = shader_::get_uniform_location(program_id_, name);
        if (location != -1)
            glUniform3f(location, v[0], v[1], v[2]);
    }

    void set_int(const std::string &name, int v) const
    {
        GLint location = shader_::get_uniform_location(program_id_, name);
        if (location != -1)
            glUniform1i(location, v);
    }

    void set_int2(const std::string &name, const glm::ivec2 &v) const
    {
        GLint location = shader_::get_uniform_location(program_id_, name);
        if (location != -1)
            glUniform2i(location, v[0], v[1]);
    }

    void set_int3(const std::string &name, const glm::ivec3 &v) const
    {
        GLint location = shader_::get_uniform_location(program_id_, name);
        if (location != -1)
            glUniform3i(location, v[0], v[1], v[2]);
    }

    void set_mat2(const std::string &name, const glm::mat2 &v) const
    {
        GLint location = shader_::get_uniform_location(program_id_,name);
        if (location != -1)
            glUniformMatrix2fv(location,1,GL_FALSE,glm::value_ptr(v));
    }

    void destroy()
    {
        if (program_id_)
        {
            glDeleteProgram(program_id_);
            program_id_ = 0;
        }
    }

    GLuint fragment_id() const { return fragment_; }
    GLuint vertex_id() const { return vertex_; }
    GLuint program_id() const { return program_id_; }

    ~ShaderProgram() { destroy(); }

private:
    ShaderProgram(GLuint vertex, GLuint fragment, GLuint program_id)
        : vertex_{vertex}, fragment_{fragment}, program_id_{program_id} {}

    GLuint vertex_, fragment_, program_id_;
};

namespace shader_
{
    std::string read_file(const std::string &file_path)
    {
        std::ifstream in{file_path, std::ios::in};
        if (!in)
            return "";
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
        std::cout << source << shader << std::endl;
        GLint status;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
        if (!status)
        {
            // Get the length of the compile log

            // Retrieve the compile log
            char infoLog[512];
            glGetShaderInfoLog(shader, 512, nullptr, infoLog);

            // Print the error message with shader type information
            std::cerr << "shader compilation failed (" << shader_type << "): " << infoLog << std::endl;
            return 0;
        }

        return shader;
    }

    GLint get_uniform_location(GLuint shader_id, const std::string &name)
    {
        return glGetUniformLocation(shader_id, name.c_str());
    }

    GLuint link_program(Shader *shaders[], GLuint count)
    {
        GLuint program = glCreateProgram();
        for (GLuint i = 0; i < count; ++i)
        {
            shaders[i]->attach(program);
        }

        glLinkProgram(program);

        GLint status;
        glGetProgramiv(program, GL_LINK_STATUS, &status);

        if (!status)
        {
            char infoLog[512];
            glGetProgramInfoLog(program, 512, nullptr, infoLog);
            std::cerr << "Shader program failed to link:\n"
                      << infoLog << std::endl;
            glDeleteProgram(program);
            return 0;
        }

        for (GLuint i = 0; i < count; ++i)
            shaders[i]->detach(program);

        return program;
    }

    bool invalid_shader(Shader &shader)
    {
        return !shader.get_id();
    }

    bool invalid_program(const ShaderProgram &program)
    {
        return !(program.program_id() && program.fragment_id() && program.vertex_id());
    }
}

/**
**********************************************************************************************************************************************************************************************************************************
* @file:	vertex_buffer.hpp
* @author:	mintychochip
* @date:	2025年05月25日 18:54:04 Sunday
* @brief:   Encapsulates an OpenGL vertex buffer (VBO)
**********************************************************************************************************************************************************************************************************************************
**/

#pragma once

#include <glad/glad.h>
#include <glfw/glfw3.h>

namespace lotus
{
    class VertexBuffer
    {
    public:
        VertexBuffer(GLsizeiptr size, GLenum usage, const void *data = nullptr) : size_{size}, usage_{usage}
        {
            glGenBuffers(1, &id_);
            bind();
            glBufferData(GL_ARRAY_BUFFER, size_, data, usage_);
        }

        ~VertexBuffer()
        {
            glDeleteBuffers(1, &id_);
        }

        void bind()
        {
            glBindBuffer(GL_ARRAY_BUFFER, id_);
        }

        void unbind()
        {
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        }

        void move(const void *data, GLintptr offset = 0)
        {
            move(data, size_, offset);
        }

        void move(const void *data, GLsizeiptr size, GLintptr offset = 0)
        {
            bind();
            glBufferSubData(GL_ARRAY_BUFFER, offset, size, data);
        }

    private:
        GLuint id_;
        GLsizeiptr size_;
        GLenum usage_;
    };
}
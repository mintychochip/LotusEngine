#pragma once

#include <glad/glad.h>
#include <glfw/glfw3.h>
#include "core/utils/types.hpp"

namespace lotus
{
    class VertexArray
    {
    public:
        VertexArray()
        {
            glGenVertexArrays(1, &id_);
        }

        ~VertexArray()
        {
            glDeleteVertexArrays(1, &id_);
        }

        void bind()
        {
            glBindVertexArray(id_);
        }

        void unbind()
        {
            glBindVertexArray(0);
        }

        GLuint id() const
        {
            return id_;
        }

    private:
        GLuint id_;
    };

    /*
    Represents a single vertex attribute in a Vertex Array Object (VAO).

    This struct defines how the vertex data is laid out in memory for one attribute,
    such as position, normal, color, or texture coordinates.

    Example usage: describing a position attribute with 3 float components.

    Fields:
    - components: Number of components per vertex (e.g., 3 for vec3).
    - type: Data type (e.g., GL_FLOAT, GL_UNSIGNED_BYTE).
    - normalized: Whether the data should be normalized when sent to the shader.
    - offset: Byte offset from the start of the vertex to this attribute.
    */
    struct Attribute_
    {
        GLuint components;    // Number of components (e.g., 3 for vec3)
        GLenum type;          // Data type (e.g., GL_FLOAT)
        GLboolean normalized; // Normalize integer values when converted
        GLsizei offset;       // Offset in bytes from the start of the vertex
    };

    class AttributeLayout
    {

    public:
        AttributeLayout(Attribute_ *attributes, u32 capacity, GLsizei stride) : attributes_{attributes}, capacity_{capacity}, count_{0}, stride_{stride} {}

        void add(GLuint components, GLenum type, GLboolean normalized, GLsizei offset)
        {
            if (count_ >= capacity_)
                return;
            attributes_[count_++] = {components, type, normalized, offset};
        }

        void apply(VertexArray &array, u32 base = 0)
        {
            array.bind();
            for (auto i = 0; i < count_; ++i)
            {
                const Attribute_ &attr = attributes_[i];
                glEnableVertexAttribArray(base + i);
                glVertexAttribPointer(base + i, attr.components, attr.type, attr.normalized, stride_, reinterpret_cast<const void *>(attr.offset));
            }
        }

        u32 count() const { return count_; }

        GLsizei stride() const { return stride_; }

        Attribute_ *attributes() const { return attributes_; }

    private:
        Attribute_ *attributes_;
        u32 capacity_, count_;
        GLsizei stride_;
    };
}
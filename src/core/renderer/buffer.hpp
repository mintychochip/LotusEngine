#pragma once

#include <glad/glad.h>
#include <glfw/glfw3.h>
#include <vector>
#include <cstdint>

namespace buffer_
{
    struct Attribute_
    {
        GLuint size;
        GLenum type;
        GLboolean normalized;
        GLuint offset;
    };

    constexpr GLsizei gl_type_size(GLenum type)
    {
        switch (type)
        {
        case GL_FLOAT:
            return sizeof(GLfloat);
        case GL_INT:
            return sizeof(GLint);
        case GL_UNSIGNED_INT:
            return sizeof(GLuint);
        case GL_SHORT:
            return sizeof(GLshort);
        case GL_UNSIGNED_SHORT:
            return sizeof(GLushort);
        case GL_BYTE:
            return sizeof(GLbyte);
        case GL_UNSIGNED_BYTE:
            return sizeof(GLubyte);
        case GL_DOUBLE:
            return sizeof(GLdouble);
        default:
            return 0;
        }
    }
};

class Buffer
{
public:
    virtual void bind(GLuint index = 0) const = 0;
};

class VertexAttributeLayout
{
public:
    VertexAttributeLayout() : attribute_count_{0}, stride_{0} {}
    VertexAttributeLayout &addAttribute(GLuint size, GLenum type, GLboolean normalized)
    {
        GLuint attr_size = buffer_::gl_type_size(type) * size;
        GLuint offset = stride_;
        attributes_.push_back({size, type, normalized, offset});
        stride_ += attr_size;
        ++attribute_count_;
        return *this;
    }

    const std::vector<buffer_::Attribute_> &attributes() const
    {
        return attributes_;
    }

private:
    friend class VertexArray;
    GLuint stride_; // current running offset for attributes
    GLuint attribute_count_;
    std::vector<buffer_::Attribute_> attributes_;
};

class VertexArray : public Buffer
{
public:
    static VertexArray *create(LinearAllocator &allocator, GLuint count, const VertexAttributeLayout &layout)
    {
        GLuint *ids = allocator.alloc<GLuint>(count);
        if (!ids)
        {
            return nullptr;
        }
        glGenVertexArrays(count, ids);
        VertexArray *vao = allocator.construct<VertexArray>(count, ids);

        auto &attributes = layout.attributes_;

        for (GLuint j = 0; j < count; ++j)
        {
            vao->bind(j); // Bind VAO for this index

            for (GLuint i = 0; i < attributes.size(); ++i)
            {
                const auto &attribute = attributes[i];
                glVertexAttribPointer(
                    i,
                    attribute.size,
                    attribute.type,
                    attribute.normalized,
                    layout.stride_,
                    reinterpret_cast<const void *>(attribute.offset));
                glEnableVertexAttribArray(i);
            }
        }

        VertexArray::unbind();

        return vao;
    }

    void bind(GLuint index = 0) const
    {
        if (index >= count_)
        {
            return;
        }
        GLuint id = ids_[index];
        glBindVertexArray(id);
    }

    static void unbind()
    {
        glBindVertexArray(0);
    }

    void destroy()
    {
        glDeleteVertexArrays(count_, ids_);
    }

    ~VertexArray()
    {
        destroy();
    }

private:
    friend class LinearAllocator;
    VertexArray(GLuint count, GLuint *ids) : count_{count}, ids_{ids} {}
    GLuint count_, *ids_;
};

class VertexBuffer : public Buffer
{
public:
    static VertexBuffer *create(LinearAllocator &allocator, GLuint count, GLsizeiptr data_size, const void* data, GLenum usage)
    {
        GLuint *ids = allocator.alloc<GLuint>(count);
        if (!ids) {
            return nullptr;
        }
        glGenBuffers(count,ids);
        for (GLuint i = 0; i < count; ++i) {
            glBindBuffer(GL_ARRAY_BUFFER,ids[i]);
            glBufferData(GL_ARRAY_BUFFER,data_size,data,usage);
        }
        return allocator.construct<VertexBuffer>(count,ids);
    }
    void bind(GLuint index = 0) const
    {
        if (index >= count_)
            return;
        GLuint id = ids_[index];
        glBindBuffer(GL_ARRAY_BUFFER,id);
    }

    void destroy()
    {
        glDeleteBuffers(count_,ids_);
    }
    
    static void unbind()
    {
        glBindBuffer(GL_ARRAY_BUFFER,0);
    }

    ~VertexBuffer()
    {
        destroy();
    }
private:
    friend class LinearAllocator;
    VertexBuffer(GLuint count, GLuint *ids) : count_{count}, ids_{ids} {}
    GLuint count_, *ids_;
};
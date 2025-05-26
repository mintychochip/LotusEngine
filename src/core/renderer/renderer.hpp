#pragma once
#include <glad/glad.h>
#include <glfw/glfw3.h>
#include <core/allocator.hpp>
#include <glm/glm.hpp>
namespace lotus
{
    class VertexBuffer
    {
        public:
            VertexBuffer(GLsizeiptr size, GLenum usage)
            {
                glGenBuffers(1,&id_);
                bind();
                glBufferData(GL_ARRAY_BUFFER,size,nullptr,usage);
            };

            ~VertexBuffer()
            {
                glDeleteBuffers(1,&id_);
            }

            void bind()
            {
                glBindBuffer(GL_ARRAY_BUFFER,id_);
            }
            
            void unbind()
            {
                glBindBuffer(GL_ARRAY_BUFFER,0);
            }

            void set_data(const void* data, GLsizeiptr size, GLintptr offset = 0)
            {
                bind();
                glBufferSubData(GL_ARRAY_BUFFER,offset,size,data);
            }

        private:
            GLuint id_;
    };
    template <typename DrawableT>
    class VertexBatch
    {
    public:
        using Vertex = typename DrawableT::Vertex;
        using Uniform = typename DrawableT::Uniform;
        VertexBatch(StackAllocator &allocator, u32 max_vertices)
            : allocator_{allocator}, max_vertices_{max_vertices} 
            {
                glGenBuffers(1,&vbo_);
            }

        void begin()
        {
            buffer_ = allocator_.alloc<Vertex>(max_vertices_);
            count_ = 0;
        }

        void push_vertex(const Vertex& vertex)
        {
            if (count_ < max_vertices_)
                buffer_[count++] = vertex;
        }

        void flush()
        {
            
        }

        Vertex *data() {
            return buffer_;
        }

        u32 count() const
        {
            return count_;
        }

    private:
        StackAllocator &allocator_;
        u32 max_vertices_, count_;
        DrawableT *buffer_;
        GLuint vbo_;
    };

    class Quad
    {
        struct QuadVertex
        {
            glm::vec2 position;
            glm::vec2 uv;
        };
        struct QuadUniform
        {
            glm::mat4 transform;
            glm::vec4 tint;
        };

        void push_

        void push(VertexBatch<Quad> &batch)
        {
            batch.push_vertex()
        }
    };
};
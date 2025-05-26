/**
**********************************************************************************************************************************************************************************************************************************
* @file:	vertex_batch.hpp
* @author:	mintychochip
* @date:	2025年05月26日 01:09:40 Monday
* @brief:	
**********************************************************************************************************************************************************************************************************************************
**/
#pragma once

#include <glad/glad.h>
#include <glfw/glfw3.h>
#include <cstdint>
#include <iostream>
#include <core/renderer/vertex_buffer.hpp>
#include <core/renderer/vertex_array.hpp>
namespace lotus
{
    template <typename DrawableT>
    class VertexBatch
    {
        using u32 = uint32_t;
        using VertexType = typename DrawableT::VertexType;

    public:
        VertexBatch(VertexType *buffer, u32 max_vertices, GLenum usage, AttributeLayout& layout)
            : buffer_{buffer}, 
            vbo_{static_cast<GLsizeiptr>(max_vertices * sizeof(VertexType)), usage}, 
            max_vertices_{max_vertices}, 
            count_{0} 
        {
            vao_.bind();
            vbo_.bind();
            layout.apply(vao_);
        }

        void begin()
        {
            count_ = 0;
        }

        void push_vertex(const VertexType& vertex)
        {
            if (count_ >= max_vertices_)
                return;
            std::cout << count_ << std::endl;
            buffer_[count_++] = vertex;
        }

        void clear()
        {
            count_ = 0;
        }

        /*
            Moves data in 'buffer_' from CPU memory to GPU memory
        */
        void forward()
        {
            vbo_.move(buffer_, static_cast<GLsizeiptr>(count_ * sizeof(VertexType)), 0);
        }

        void draw()
        {
            vao_.bind();
            glDrawArrays(GL_TRIANGLES, 0, count_);
        }

    private:
        VertexBuffer vbo_;
        VertexArray vao_;
        VertexType *buffer_;
        u32 max_vertices_, count_;
    };

    template <typename DrawableT>
    class VertexBatchFactory
    {
        using u32 = uint32_t;
        using VertexType = typename DrawableT::VertexType;
        public:
            VertexBatchFactory(u32 max_vertices) : max_vertices_{max_vertices}, allocator_{max_vertices} {}

            VertexBatch<DrawableT> create(u32 max_vertices, GLenum usage, AttributeLayout& layout)
            {
                auto allocation = allocator_.alloc(max_vertices);
                return {allocation.member, max_vertices, usage, layout};
            }
        private:
            PoolAllocator<VertexType> allocator_;
            u32 max_vertices_;
    };
}
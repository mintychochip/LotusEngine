#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <cstdint>
#include <iostream>
#include "core/renderer/buffer.hpp"
#include "core/renderer/vertex_array.hpp"
#include "core/utils/types.hpp"

namespace lotus
{
    enum BatchBufferType
    {
        VERTEX_BUFFER = 0,
        UNIFORM_BUFFER = 1,
        ELEMENT_BUFFER = 2
    };
    
    template <typename DrawableT>
    class VertexBatch
    {
        using VertexType = typename DrawableT::VertexType;
        using UniformDataType = typename DrawableT::UniformDataType;

        static constexpr int BUFFER_COUNT = 3;

        struct Buffers_
        {
            VertexType* vertex = nullptr;
            UniformDataType* uniform = nullptr;
            u32* element = nullptr;
            u32 size[BUFFER_COUNT] = {};
            u32 count[BUFFER_COUNT] = {};
        };

    public:
        
        VertexBatch(
            VertexType* vertex_data, u32 max_vertices,
            UniformDataType* uniform_data, u32 max_uniforms,
            u32* element_data, u32 max_elements,
            GLenum usage, AttributeLayout& layout
        )
            : vbo_{GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(max_vertices * sizeof(VertexType)), usage},
              ebo_{GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(max_elements * sizeof(u32)), usage},
              ubo_{static_cast<GLsizeiptr>(max_uniforms * sizeof(UniformDataType)), usage}
        {
            assert(max_elements % 3 == 0 && "Element buffer size must be a multiple of 3.");

            buffers_.vertex = vertex_data;
            buffers_.uniform = uniform_data;
            buffers_.element = element_data;

            buffers_.size[VERTEX_BUFFER] = max_vertices;
            buffers_.size[UNIFORM_BUFFER] = max_uniforms;
            buffers_.size[ELEMENT_BUFFER] = max_elements;

            vao_.bind();
            vbo_.bind();
            layout.apply(vao_);
        }

        void push_vertex(const VertexType& v)
        {
            auto& count = buffers_.count[VERTEX_BUFFER];
            assert(count < buffers_.size[VERTEX_BUFFER]);
            buffers_.vertex[count++] = v;
        }

        void push_uniform(const UniformDataType& u)
        {
            auto& count = buffers_.count[UNIFORM_BUFFER];
            assert(count < buffers_.size[UNIFORM_BUFFER]);
            buffers_.uniform[count++] = u;
        }

        void push_element(const glm::uvec3& tri)
        {
            auto& count = buffers_.count[ELEMENT_BUFFER];
            assert(count + 3 <= buffers_.size[ELEMENT_BUFFER]);
            auto* e = buffers_.element;
            e[count++] = tri[0];
            e[count++] = tri[1];
            e[count++] = tri[2];
        }

        void clear()
        {
            for (int i = 0; i < BUFFER_COUNT; ++i)
                clear(i);
        }

        void clear(int buffer)
        {
            buffers_.count[buffer] = 0;
        }

        void forward()
        {
            if (buffers_.count[VERTEX_BUFFER] > 0)
                vbo_.move(buffers_.vertex, buffers_.count[VERTEX_BUFFER] * sizeof(VertexType), 0);
            if (buffers_.count[UNIFORM_BUFFER] > 0)
                ubo_.move(buffers_.uniform, buffers_.count[UNIFORM_BUFFER] * sizeof(UniformDataType), 0);
            if (buffers_.count[ELEMENT_BUFFER] > 0)
                ebo_.move(buffers_.element, buffers_.count[ELEMENT_BUFFER] * sizeof(u32), 0);
        }

        void draw()
        {
            vao_.bind();
            glDrawElements(GL_TRIANGLES, buffers_.count[ELEMENT_BUFFER], GL_UNSIGNED_INT, nullptr);
        }

    private:
        Buffers_ buffers_;
        Buffer vbo_, ebo_;
        UniformBuffer ubo_;
        VertexArray vao_;
    };
}

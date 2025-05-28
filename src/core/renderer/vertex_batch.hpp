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
#include "core/renderer/buffer.hpp"
#include "core/renderer/vertex_array.hpp"
#include "core/utils/types.hpp"
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
    template <typename DrawableT>
    class VertexBatch
    {
        using VertexType = typename DrawableT::VertexType;
        using UniformDataType = typename DrawableT::UniformDataType;

    public:
        VertexBatch(
            VertexType* vertex_data, u32 max_vertices,
            UniformDataType* uniform_data, u32 max_uniforms,
            u32* element_data, u32 max_elements,
            GLenum usage, AttributeLayout& layout
        )
            : vertex_data_(vertex_data), uniform_data_(uniform_data), element_data_(element_data),
              max_vertices_(max_vertices), max_uniforms_(max_uniforms), max_elements_(max_elements),
              vertex_count_(0), uniform_count_(0), element_count_(0),
              vbo_{GL_ARRAY_BUFFER, max_vertices * sizeof(VertexType), usage},
              ebo_{GL_ELEMENT_ARRAY_BUFFER, max_elements * sizeof(u32), usage},
              ubo_{max_uniforms * sizeof(UniformDataType), usage}
        {
            assert(max_elements % 3 == 0 && "Element buffer size must be a multiple of 3.");
            vao_.bind();
            vbo_.bind();
            layout.apply(vao_);
        }

        void push_vertex(const VertexType& v)
        {
            assert(vertex_count_ < max_vertices_);
            vertex_data_[vertex_count_++] = v;
        }

        void push_uniform(const UniformDataType& u)
        {
            assert(uniform_count_ < max_uniforms_);
            uniform_data_[uniform_count_++] = u;
        }

        void push_element(const glm::uvec3& tri)
        {
            assert(element_count_ + 3 <= max_elements_);
            element_data_[element_count_++] = tri[0];
            element_data_[element_count_++] = tri[1];
            element_data_[element_count_++] = tri[2];
        }

        void clear()
        {
            vertex_count_ = 0;
            uniform_count_ = 0;
            element_count_ = 0;
        }

        void forward()
        {
            if (!vbo_.empty()) vbo_.move(vertex_data_, vertex_count_ * sizeof(VertexType), 0);
            if (!ubo_.empty()) ubo_.move(uniform_data_, uniform_count_ * sizeof(UniformDataType), 0);
            if (!ebo_.empty()) ebo_.move(element_data_, element_count_ * sizeof(u32), 0);
        }

        void draw()
        {
            vao_.bind();
            glDrawElements(GL_TRIANGLES, element_count_, GL_UNSIGNED_INT, nullptr);
        }

    private:
        VertexType* vertex_data_;
        UniformDataType* uniform_data_;
        u32* element_data_;

        u32 max_vertices_, max_uniforms_, max_elements_;
        u32 vertex_count_, uniform_count_, element_count_;

        Buffer vbo_, ebo_;
        UniformBuffer ubo_;
        VertexArray vao_;
    };
}

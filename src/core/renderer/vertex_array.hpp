/**
**********************************************************************************************************************************************************************************************************************************
* @file:	vertex_array.hpp
* @author:	mintychochip
* @date:	2025年05月25日 21:59:08 Sunday
* @brief:
**********************************************************************************************************************************************************************************************************************************
**/
#pragma once

#include <glad/glad.h>
#include <glfw/glfw3.h>
#include <cstdint>
#include <core/allocator.hpp>

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

    private:
        GLuint id_;
    };

    struct Attribute_
    {
        GLuint components;
        GLenum type;
        GLboolean normalized;
        GLsizei offset;
    };

    class AttributeLayout
    {
        using u32 = uint32_t;

    public:
        AttributeLayout(Attribute_ *attributes, u32 attribute_count, GLsizei stride) : attributes_{attributes}, capacity_{attribute_count}, count_{0}, stride_{stride} {}

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

    class AttributeLayoutFactory
    {
        using u32 = uint32_t;

    public:
        /*
            @param max_attribute_blocks 
                The maximum number of attribute entries (blocks) that can be allocated 
                by this factory. Each block represents a single vertex attribute 
                definition (e.g., position, UV, color) used in a vertex layout.

                This value determines the total capacity of the internal memory pool.
                It should be large enough to accommodate all attribute layouts that 
                will be created and used concurrently. Each call to `create()` reserves 
                a contiguous range of these blocks.

                Example:
                    If you plan to create up to 8 attribute layouts, each using 4 attributes,
                    set max_attribute_blocks = 8 * 4 = 32.
        */
        explicit AttributeLayoutFactory(u32 max_attribute_blocks)
            : allocator_{max_attribute_blocks} {}

        template <typename Vertex>
        AttributeLayout create(u32 attributes)
        {
            return create(attributes, sizeof(Vertex));
        }

        AttributeLayout create(u32 attributes, GLsizei stride)
        {
            auto allocation = allocator_.alloc(attributes);
            allocator_.free(allocation);
            return {allocation.member, attributes, stride};
        }

        void free(AttributeLayout &layout)
        {
            PoolAllocationBlock<Attribute_> block;
            block.member = layout.attributes();
            allocator_.free({layout.count(), block});
        }

    private:
        PoolAllocator<lotus::Attribute_> allocator_;
    };
}
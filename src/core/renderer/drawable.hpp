#pragma once

#include <glm/glm.hpp>

namespace lotus
{
    class Quad
    {
    public:
        struct QuadVertex
        {
            glm::vec2 pos;
        };

        struct QuadUniformData
        {
            glm::vec3 color;
        };

        using VertexType = QuadVertex;
        using UniformDataType = QuadUniformData;
        VertexType vertices[4];
    };
}
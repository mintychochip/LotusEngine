#pragma once

#include <glm/glm.hpp>

#include "vertex_batch.hpp"

namespace lotus {
class Shape {
public:
	struct Vertex {
		glm::vec2 pos;
	};
	using VertexType = Vertex;

	virtual void push(VertexBatch<Shape>& batch) = 0;

	virtual ~Shape() = default;
};

template <size_t Vertices>
class Polygon : public Shape {
protected:
	Polygon(const glm::vec2 vertices[Vertices])
	{
		for (size_t i = 0; i < Vertices; ++i) {
			vertices_[i] = vertices[i];
		}
	}
	glm::vec2 vertices_[Vertices];
};

class Triangle : public Polygon<3> {
public:
	using Polygon::Polygon;
	void push(VertexBatch<Shape>& batch) override
	{
		batch.push_element({0, 1, 2});
		for (size_t i = 0; i < 3; ++i) {
			batch.push_vertex({vertices_[i]});
		}
	}
};

class Quad : public Polygon<4> {
public:
	using Polygon::Polygon;
	void push(VertexBatch<Shape>& batch) override
	{
		batch.push_element({0, 1, 2});
		batch.push_element({2, 3, 0});
		for (size_t i = 0; i < 4; ++i) {
			batch.push_vertex({vertices_[i]});
		}
	}

	static Quad square(glm::vec2 pos, float size)
	{
		glm::vec2 verts[4] = {
		        pos,
		        pos + glm::vec2{size, 0.0f},
		        pos + glm::vec2{size, size},
		        pos + glm::vec2{0.0f, size}
                };
		return Quad{verts};
	}
};

} // namespace lotus

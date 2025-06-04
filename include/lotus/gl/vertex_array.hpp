#pragma once

#include <glad/glad.h>
#include <glfw/glfw3.h>

namespace lotus {
class VertexArray {
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
struct Attribute_ {
	GLuint components;    // Number of components (e.g., 3 for vec3)
	GLenum type;          // Data type (e.g., GL_FLOAT)
	GLboolean normalized; // Normalize integer values when converted
	GLsizei offset;       // Offset in bytes from the start of the vertex
};

class AttributeLayout {
public:
	class Builder {
	public:
		Builder(Attribute_* attributes, size_t capacity, GLsizei stride)
		        : attributes_{attributes},
		          capacity_{capacity},
		          count_{0},
		          offset_{0},
		          stride_{stride}
		{}

		template <GLuint components>
		inline Builder& add_float(GLboolean normalized = GL_FALSE)
		{
			return add(components,
			           GL_FLOAT,
			           normalized,
			           sizeof(float) * components);
		}

		template <GLuint components>
		inline Builder& add_int(GLboolean normalized = GL_FALSE)
		{
			return add(components, GL_INT, normalized, sizeof(int) * components);
		}

		template <GLuint components>
		inline Builder& add_uint(GLboolean normalized = GL_FALSE)
		{
			return add(components,
			           GL_UNSIGNED_INT,
			           normalized,
			           sizeof(unsigned int) * components);
		}

		AttributeLayout build()
		{
			return {attributes_, capacity_, count_, stride_};
		}

	private:
		inline Builder& add(GLuint components, GLenum type,
		                    GLboolean normalized, GLsizei size)
		{
			if (count_ >= capacity_) {
				return *this;
			}
			attributes_[count_++] = {components, type, normalized, offset_};
			offset_ += size;
			return *this;
		}
		Attribute_* attributes_;
		size_t capacity_, count_;
		GLsizei offset_;
		GLsizei stride_;
	};

	AttributeLayout(Attribute_* attributes, size_t capacity, size_t count,
	                GLsizei stride)
	        : attributes_{attributes},
	          capacity_{capacity},
	          count_{count},
	          stride_{stride}
	{}

	void apply(VertexArray& array, size_t base = 0)
	{
		array.bind();
		for (auto i = 0; i < count_; ++i) {
			const Attribute_& attr = attributes_[i];
			glEnableVertexAttribArray(base + i);
			glVertexAttribPointer(base + i,
			                      attr.components,
			                      attr.type,
			                      attr.normalized,
			                      stride_,
			                      reinterpret_cast<const void*>(
			                              attr.offset));
		}
	}

	size_t count() const
	{
		return count_;
	}

	GLsizei stride() const
	{
		return stride_;
	}

	Attribute_* attributes() const
	{
		return attributes_;
	}

private:
	Attribute_* attributes_;
	size_t capacity_, count_;
	GLsizei stride_;
};
} // namespace lotus
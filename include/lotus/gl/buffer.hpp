
#pragma once

#include <glad/glad.h>
#include <glfw/glfw3.h>

namespace lotus {
class Buffer {

public:
	Buffer(GLenum buffer_type, GLsizeiptr size, GLenum usage,
	       const void* data = nullptr)
	        : buffer_type_{buffer_type},
	          size_{size},
	          usage_{usage}
	{
		glGenBuffers(1, &id_);
		bind();
		glBufferData(buffer_type_, size_, data, usage_);
	}
	/*
	    Binds the GLobject to the current OpenGL context
	*/
	void bind()
	{
		glBindBuffer(buffer_type_, id_);
	}

	/*
	    Unbinds the GLobject from the current OpenGL context
	*/
	void unbind()
	{
		glBindBuffer(buffer_type_, 0);
	}

	GLuint id() const
	{
		return id_;
	}

	GLenum buffer_type() const
	{
		return buffer_type_;
	}

	GLsizeiptr size() const
	{
		return size_;
	}

	bool empty() const
	{
		return !size_;
	}

	/*
	    Writes contents of the buffer from CPU to GPU memory.

	    The size of 'data' is assumed to be the same size as the allocated
	   buffer on the GPU. This is an implementation detail.

	    @param data The data being copied
	    @param offset Starting byte offset of where the data will be copied
	*/
	void move(const void* data, GLintptr offset = 0)
	{
		move(data, size_, offset);
	}

	/*
	    Writes contents of the buffer from CPU to GPU memory.

	    @param data The data being copied
	    @param size Amount of bytes being copied
	    @param offset Starting byte offset of where the data will be copied
	*/
	void move(const void* data, GLsizeiptr size, GLintptr offset = 0)
	{
		bind();
		glBufferSubData(buffer_type_, offset, size, data);
	}

protected:
	GLenum buffer_type_, usage_;
	GLsizeiptr size_;
	GLuint id_;
};

class UniformBuffer : public Buffer {
public:
	UniformBuffer(GLsizeiptr size, GLenum usage, const void* data = nullptr)
	        : Buffer{GL_UNIFORM_BUFFER, size, usage, data}
	{
		glBindBufferRange(GL_UNIFORM_BUFFER, 0, id_, 0, size_);
	}
};
} // namespace lotus
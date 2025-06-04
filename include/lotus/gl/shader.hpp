#pragma once

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>

namespace lotus {
class Shader {

public:
	inline void attach(GLuint program_id) const
	{
		glAttachShader(program_id, shader_id_);
	}

	inline void detach(GLuint program_id) const
	{
		glDetachShader(program_id, shader_id_);
	}

	GLuint get_id() const
	{
		return shader_id_;
	}

private:
	Shader(GLuint shader_id, GLenum shader_type)
	        : shader_id_{shader_id},
	          shader_type_{shader_type}
	{}
	friend class ShaderCache;
	GLuint shader_id_;
	GLenum shader_type_;
};

class ShaderCache {
public:
	Shader get_or_create(const std::string& path, GLenum shader_type);

private:
	std::unordered_map<std::string, Shader> shaders_;
};
} // namespace lotus

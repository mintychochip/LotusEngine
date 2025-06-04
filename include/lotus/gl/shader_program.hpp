#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "shader.hpp"

namespace lotus {

class ShaderProgram {

public:
	ShaderProgram() : program_id_{glCreateProgram()} {}
	~ShaderProgram()
	{
		if (glIsProgram(program_id_)) {
			glDeleteProgram(program_id_);
		}
	}

	bool link(Shader** shaders, size_t count);

	inline void float1(const std::string& uniform, float val) const
	{
		GLint loc = glGetUniformLocation(program_id_, uniform.c_str());
		if (loc != -1) {
			glUniform1f(loc, val);
		}
	}

	inline void float2(const std::string& uniform, const glm::vec2& val) const
	{
		GLint loc = glGetUniformLocation(program_id_, uniform.c_str());
		if (loc != -1) {
			glUniform2fv(loc, 1, glm::value_ptr(val));
		}
	}

	inline void float3(const std::string& uniform, const glm::vec3& val) const
	{
		GLint loc = glGetUniformLocation(program_id_, uniform.c_str());
		if (loc != -1) {
			glUniform3fv(loc, 1, glm::value_ptr(val));
		}
	}

	inline void float4(const std::string& uniform, const glm::vec4& val) const
	{
		GLint loc = glGetUniformLocation(program_id_, uniform.c_str());
		if (loc != -1) {
			glUniform4fv(loc, 1, glm::value_ptr(val));
		}
	}

	inline void int1(const std::string& uniform, int val) const
	{
		GLint loc = glGetUniformLocation(program_id_, uniform.c_str());
		if (loc != -1) {
			glUniform1i(loc, val);
		}
	}

	inline void int2(const std::string& uniform, const glm::ivec2& val) const
	{
		GLint loc = glGetUniformLocation(program_id_, uniform.c_str());
		if (loc != -1) {
			glUniform2iv(loc, 1, glm::value_ptr(val));
		}
	}

	inline void int3(const std::string& uniform, const glm::ivec3& val) const
	{
		GLint loc = glGetUniformLocation(program_id_, uniform.c_str());
		if (loc != -1) {
			glUniform3iv(loc, 1, glm::value_ptr(val));
		}
	}

	inline void int4(const std::string& uniform, const glm::ivec4& val) const
	{
		GLint loc = glGetUniformLocation(program_id_, uniform.c_str());
		if (loc != -1) {
			glUniform4iv(loc, 1, glm::value_ptr(val));
		}
	}

	inline void uint1(const std::string& uniform, unsigned int val) const
	{
		GLint loc = glGetUniformLocation(program_id_, uniform.c_str());
		if (loc != -1) {
			glUniform1ui(loc, val);
		}
	}

	inline void uint2(const std::string& uniform, const glm::uvec2& val) const
	{
		GLint loc = glGetUniformLocation(program_id_, uniform.c_str());
		if (loc != -1) {
			glUniform2uiv(loc, 1, glm::value_ptr(val));
		}
	}

	inline void uint3(const std::string& uniform, const glm::uvec3& val) const
	{
		GLint loc = glGetUniformLocation(program_id_, uniform.c_str());
		if (loc != -1) {
			glUniform3uiv(loc, 1, glm::value_ptr(val));
		}
	}

	inline void uint4(const std::string& uniform, const glm::uvec4& val) const
	{
		GLint loc = glGetUniformLocation(program_id_, uniform.c_str());
		if (loc != -1) {
			glUniform4uiv(loc, 1, glm::value_ptr(val));
		}
	}

	inline void float2x2(const std::string& uniform, const glm::mat2& val) const
	{
		GLint loc = glGetUniformLocation(program_id_, uniform.c_str());
		if (loc != -1) {
			glUniformMatrix2fv(loc, 1, GL_FALSE, glm::value_ptr(val));
		}
	}

	inline void float2x3(const std::string& uniform, const glm::mat2x3& val) const
	{
		GLint loc = glGetUniformLocation(program_id_, uniform.c_str());
		if (loc != -1) {
			glUniformMatrix2x3fv(loc, 1, GL_FALSE, glm::value_ptr(val));
		}
	}

	inline void float2x4(const std::string& uniform, const glm::mat2x4& val) const
	{
		GLint loc = glGetUniformLocation(program_id_, uniform.c_str());
		if (loc != -1) {
			glUniformMatrix2x4fv(loc, 1, GL_FALSE, glm::value_ptr(val));
		}
	}

	inline void float3x2(const std::string& uniform, const glm::mat3x2& val) const
	{
		GLint loc = glGetUniformLocation(program_id_, uniform.c_str());
		if (loc != -1) {
			glUniformMatrix3x2fv(loc, 1, GL_FALSE, glm::value_ptr(val));
		}
	}

	inline void float3x3(const std::string& uniform, const glm::mat3& val) const
	{
		GLint loc = glGetUniformLocation(program_id_, uniform.c_str());
		if (loc != -1) {
			glUniformMatrix3fv(loc, 1, GL_FALSE, glm::value_ptr(val));
		}
	}

	inline void float3x4(const std::string& uniform, const glm::mat3x4& val) const
	{
		GLint loc = glGetUniformLocation(program_id_, uniform.c_str());
		if (loc != -1) {
			glUniformMatrix3x4fv(loc, 1, GL_FALSE, glm::value_ptr(val));
		}
	}

	inline void float4x2(const std::string& uniform, const glm::mat4x2& val) const
	{
		GLint loc = glGetUniformLocation(program_id_, uniform.c_str());
		if (loc != -1) {
			glUniformMatrix4x2fv(loc, 1, GL_FALSE, glm::value_ptr(val));
		}
	}

	inline void float4x3(const std::string& uniform, const glm::mat4x3& val) const
	{
		GLint loc = glGetUniformLocation(program_id_, uniform.c_str());
		if (loc != -1) {
			glUniformMatrix4x3fv(loc, 1, GL_FALSE, glm::value_ptr(val));
		}
	}

	inline void float4x4(const std::string& uniform, const glm::mat4& val) const
	{
		GLint loc = glGetUniformLocation(program_id_, uniform.c_str());
		if (loc != -1) {
			glUniformMatrix4fv(loc, 1, GL_FALSE, glm::value_ptr(val));
		}
	}

	inline void use() const
	{
		glUseProgram(program_id_);
	}

	inline GLuint get_id() const
	{
		return program_id_;
	}

private:
	GLuint program_id_;
};
} // namespace lotus
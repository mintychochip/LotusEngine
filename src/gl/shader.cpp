#include "lotus/gl/shader.hpp"
#include "lotus/global.hpp"
using namespace lotus;

std::string read_file(const std::string& path)
{
	std::ifstream file(path);
	if (!file) {
		return "";
	}

	std::stringstream buffer;
	buffer << file.rdbuf();
	return buffer.str();
}

bool compile_shader(const char* source, GLuint shader)
{
	glShaderSource(shader, 1, &source, nullptr);
	glCompileShader(shader);
	GLint status;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	return status == GL_TRUE;
}

Shader ShaderCache::get_or_create(const std::string& path, GLenum shader_type)
{
	auto it = shaders_.find(path);
	if (it != shaders_.end()) {
		std::cout << "did not compile twice";
		return it->second;
	}

	if (!Global::flag(LOTUS_GLFW_INITIALIZED)) {
		throw std::runtime_error("glfw is not initialized");
	}
	GLuint shader_id = glCreateShader(shader_type);
	std::cout << shader_id << std::endl;
	std::string source = read_file(path);
	if (source.empty()) {
		throw std::runtime_error("failed to read file");
	}
	bool status = compile_shader(source.c_str(), shader_id);
	if (status) {

	} else {
		throw std::runtime_error("failed to compile shader");
	}
	Shader shader{shader_id, shader_type};
	auto [inserted, success] = shaders_.emplace(path, shader);
	return inserted->second;
}
#include "lotus/gl/shader_program.hpp"

using namespace lotus;

bool ShaderProgram::link(Shader** shaders, size_t count)
{
	for (size_t i = 0; i < count; ++i) {
		if (shaders[i]->get_id() != 0) {
			shaders[i]->attach(program_id_);
		}
	}

	glLinkProgram(program_id_);
	GLint status;
	glGetProgramiv(program_id_, GL_LINK_STATUS, &status);

	if (!status) {
		return false;
	}

	for (size_t i = 0; i < count; ++i) {
		if (shaders[i]->get_id() != 0) {
			shaders[i]->detach(program_id_);
		}
	}
	return true;
}
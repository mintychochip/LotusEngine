
#include "lotus/application.hpp"
#include "lotus/gl.hpp"
#include "lotus/global.hpp"
#include "lotus/stack_allocator.hpp"
#include "lotus/utils.hpp"
#include <stdexcept>

using namespace lotus;

void GLApplication::on_start()
{
	if (!glfwInit()) {
		throw std::runtime_error("failed to initialize glfw");
	}
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, profile_.major);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, profile_.minor);
	glfwWindowHint(GLFW_OPENGL_PROFILE, profile_.gl_profile);
	Global::flag(LOTUS_GLFW_INITIALIZED, true);
}

void GLApplication::run()
{
	on_start();
	GLWindow window{800, 600, "test"};
	window.set_current();

	ShaderCache shader_cache;
	Shader vertex   = shader_cache.get_or_create("../shaders/test.vert",
                                                   GL_VERTEX_SHADER);
	Shader fragment = shader_cache.get_or_create("../shaders/test.frag",
	                                             GL_FRAGMENT_SHADER);

	ShaderProgram program;
	Shader* shaders[2] = {&vertex, &fragment};
	bool status        = program.link(shaders, 2);
	if (!status) {
		return;
	}
	StackAllocator<128> allocator{KB(1000), 8};
	AttributeLayout::Builder builder{allocator.alloc<Attribute_>(1),
	                                 1,
	                                 sizeof(Shape::VertexType)};
	auto layout = builder.add_float<2>().build();
	VertexBatch<Shape> batch{allocator.alloc<Shape::VertexType>(12),
	                         12,
	                         allocator.alloc<u32>(12),
	                         12,
	                         GL_STATIC_DRAW,
	                         layout};
	glClearColor(0.0f, 0.0f, 0.5f, 1.0f);
	while (!window.should_close()) {
		glClear(GL_COLOR_BUFFER_BIT);
		float time = static_cast<float>(glfwGetTime());

		const int quadCount = 2;
		batch.clear();
		Quad::square({std::sin(time), 0.0f}, 0.5f).push(batch);

		program.use();
		batch.forward();
		batch.draw();
		window.swap_buffers();
		glfwPollEvents();
	}
}
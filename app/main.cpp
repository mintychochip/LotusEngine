#include "lotus/application.hpp"
#include "lotus/gl.hpp"
#include "lotus/stack_allocator.hpp"
#include "lotus/utils.hpp"
#include <cmath>
#include <iostream>
#include <optional>

class DemoApplication : public lotus::GLApplication {
public:
	using lotus::GLApplication::GLApplication;

protected:
	void on_ready(lotus::GLWindow& /*window*/) override
	{
		program_.emplace();
		lotus::ShaderCache shader_cache;
		lotus::Shader vertex =
		        shader_cache.get_or_create("../shaders/test.vert", GL_VERTEX_SHADER);
		lotus::Shader fragment =
		        shader_cache.get_or_create("../shaders/test.frag",
		                                   GL_FRAGMENT_SHADER);

		lotus::Shader* shaders[2] = {&vertex, &fragment};
		if (!program_->link(shaders, 2)) {
			throw std::runtime_error("shader program link failed");
		}

		lotus::AttributeLayout::Builder builder{
		        allocator_.alloc<lotus::Attribute_>(1),
		        1,
		        sizeof(lotus::Shape::VertexType)};
		auto layout = builder.add_float<2>().build();
		batch_.emplace(allocator_.alloc<lotus::Shape::VertexType>(12),
		               12,
		               allocator_.alloc<lotus::u32>(12),
		               12,
		               GL_STATIC_DRAW,
		               layout);
		glClearColor(0.0f, 0.0f, 0.5f, 1.0f);
	}

	void on_update(float dt) override
	{
		anim_time_ += dt;
	}

	void on_render() override
	{
		glClear(GL_COLOR_BUFFER_BIT);
		batch_->clear();
		lotus::Quad::square({std::sin(anim_time_), 0.0f}, 0.5f).push(*batch_);
		program_->use();
		batch_->forward();
		batch_->draw();
	}

	void on_shutdown() override {}

private:
	lotus::StackAllocator<128> allocator_{KB(1000), 8};
	std::optional<lotus::ShaderProgram> program_;
	std::optional<lotus::VertexBatch<lotus::Shape>> batch_;
	float anim_time_{0.0f};
};

int main()
{
	try {
		DemoApplication app{{3, 3, true}};
		app.run(800, 600, "lotus");
	} catch (const std::exception& ex) {
		std::cerr << "lotus: " << ex.what() << '\n';
		return 1;
	}
	return 0;
}

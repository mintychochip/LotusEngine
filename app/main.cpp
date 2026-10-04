#include "lotus/application.hpp"
#include "lotus/gpu/renderer.hpp"
#include <cmath>
#include <cstring>
#include <iostream>

namespace {

struct GpuVertex {
	float x, y, z;
	float r, g, b;
};

void identity_mvp(float* m)
{
	std::memset(m, 0, sizeof(float) * 16);
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void translation_mvp(float* m, float tx, float ty)
{
	identity_mvp(m);
	m[12] = tx;
	m[13] = ty;
}

void quad_vertices(float ox, float oy, float size, GpuVertex out[6])
{
	const float z = 0.0f;
	const float s = size * 0.5f;
	const float gray = 0.5f;
	auto corner = [&](float x, float y) {
		return GpuVertex{x + ox, y + oy, z, gray, gray, gray};
	};
	const GpuVertex v0 = corner(-s, s);
	const GpuVertex v1 = corner(s, s);
	const GpuVertex v2 = corner(s, -s);
	const GpuVertex v3 = corner(-s, -s);
	out[0] = v0;
	out[1] = v1;
	out[2] = v2;
	out[3] = v0;
	out[4] = v2;
	out[5] = v3;
}

} // namespace

class DemoApplication : public lotus::Application {
public:
	using lotus::Application::Application;

protected:
	void on_ready(lotus::Window& window, lotus::GpuRenderer& gpu) override
	{
		(void)window;
		std::cout << "gpu driver: " << gpu.driver_name() << '\n';
		GpuVertex verts[6];
		quad_vertices(0.0f, 0.0f, 0.5f, verts);
		gpu.set_vertices(verts, sizeof(verts));
	}

	void on_update(float dt) override { anim_time_ += dt; }

	void on_prepare_frame(lotus::GpuRenderer& gpu) override
	{
		GpuVertex verts[6];
		const float ox = std::sin(anim_time_);
		quad_vertices(ox, 0.0f, 0.5f, verts);
		gpu.set_vertices(verts, sizeof(verts));
	}

	void on_render(lotus::GpuRenderer& gpu) override
	{
		float mvp[16];
		translation_mvp(mvp, 0.0f, 0.0f);
		gpu.draw(mvp, 6);
	}

	void on_shutdown() override {}

private:
	float anim_time_{0.0f};
};

int main()
{
	try {
		DemoApplication app;
		app.run(800, 600, "lotus");
	} catch (const std::exception& ex) {
		std::cerr << "lotus: " << ex.what() << '\n';
		return 1;
	}
	return 0;
}

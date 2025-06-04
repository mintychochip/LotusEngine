#include "lotus/allocator.hpp"
#include "lotus/application.hpp"
#include "lotus/global.hpp"
#include <iostream>

class Application : public lotus::GLApplication {
public:
	Application(const lotus::GLProfile& profile)
	        : lotus::GLApplication{profile}
	{}
	void on_shutdown() {}
	void on_render() {}
	void on_update() {}
};
int main()
{
	Application app{
	        {3, 3, GLFW_OPENGL_CORE_PROFILE}
        };
	app.run();
	std::cout << "here" << std::endl;
	return 0;
}
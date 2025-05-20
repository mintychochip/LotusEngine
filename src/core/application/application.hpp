
#include <atomic>
#include <gl/gl.h>
#include <glfw/glfw3.h>
class Application
{
    public:
    static Application &get()
    {
        static Application instance;
        return instance;
    }
    void Run()
    {
        while (!done_) {

        }
    }
    private:
        Application() {}
        std::atomic<bool> done_;
};
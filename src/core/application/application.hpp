#pragma once

#include "window.hpp"
#include "allocators.hpp"
class GLApplication
{
public:
    ~GLApplication()
    {
        on_shutdown();
    }
    void run();
protected:
    GLApplication(std::tuple<int,int> profile, const std::string &application_name = "")
    : profile_{profile}, application_name_{application_name}
    {
        on_start();
    }

    GLWindow* create_window(lotus::Allocator &allocator, int width, int height, const std::string& title)
    {
        allocator.construct<GLWindow>(width,height,title);
        void* memory = allocator.alloc(sizeof(GLWindow),1);
        static_cast<GLWindow>();
    }

    std::tuple<int,int> get_profile()
    {

    }

    void on_start();
    virtual void on_update(float dt) = 0;
    virtual void on_render() = 0;
    virtual void on_shutdown() = 0;
private:
    std::tuple<int,int> profile_;
    std::string application_name_;
};
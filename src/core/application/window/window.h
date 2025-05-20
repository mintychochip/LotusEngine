#pragma once

#include <string> 

template <typename T>
class IWindow
{
public:
    virtual void close(bool force) = 0;
    virtual int should_close() = 0;
    virtual void swap_buffers() = 0;
    virtual void clear() = 0;
    virtual void set_title(const std::string& title) = 0;
    virtual void set_size(int width, int height) = 0;
    virtual void get_size(int& width, int& height) = 0;
    virtual T *handle() = 0;

    virtual ~IWindow() = default;
protected:
    IWindow(int x, int y, T *window) {}
};

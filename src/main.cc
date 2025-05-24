#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "core/renderer/shader.hpp" // your shader header
#include "core/renderer/buffer.hpp"
#include "core/application/gl_application.hpp"
#include "core/renderer/buffer.hpp"

int main() {
    GLApplication &application = GLApplication::get();
    application.run();
    return 0;
}

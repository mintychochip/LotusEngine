#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "core/renderer/shader.hpp" // your shader header
#include "core/application/gl_application.hpp"

int main() {
    GLApplication &application = GLApplication::get();
    application.run();
    return 0;
}

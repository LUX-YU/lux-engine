#include <lux/engine/window/GlfwRuntime.hpp>
#include <GLFW/glfw3.h>

namespace lux::window
{
    GlfwRuntime::GlfwRuntime()
    {
        valid_ = (glfwInit() == GLFW_TRUE);
    }

    GlfwRuntime::~GlfwRuntime()
    {
        if (valid_)
            glfwTerminate();
    }

} // namespace lux::window

#include <lux/engine/window/GlfwRuntime.hpp>
#include <GLFW/glfw3.h>
#include <cstdlib>

namespace lux::window
{
    namespace
    {
        std::uint64_t display_revision{1};
    }

    std::uint64_t GlfwRuntime::displayRevision() noexcept
    {
        return display_revision;
    }

    GlfwRuntime::GlfwRuntime()
    {
        valid_ = (glfwInit() == GLFW_TRUE);
        if (valid_)
            glfwSetMonitorCallback(
                [](GLFWmonitor*, int)
                {
                    if (display_revision == UINT64_MAX)
                        std::abort();
                    ++display_revision;
                }
            );
    }

    GlfwRuntime::~GlfwRuntime()
    {
        if (valid_)
            glfwTerminate();
    }

} // namespace lux::window

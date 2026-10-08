#include <GLFW/glfw3.h>
#include <cstdlib>
#include <lux/engine/window/GlfwRuntime.hpp>

namespace lux::window
{
    namespace
    {
        std::uint64_t display_revision{1};
        GlfwRuntime* runtime_owner{};
    } // namespace

    std::uint64_t GlfwRuntime::displayRevision() noexcept
    {
        return display_revision;
    }

    GlfwRuntime::CreateResult GlfwRuntime::create() noexcept
    {
        if (runtime_owner)
        {
            return lux::cxx::unexpected(EGlfwInitError::ALREADY_ACTIVE);
        }
        if (glfwInit() != GLFW_TRUE)
        {
            return lux::cxx::unexpected(EGlfwInitError::BACKEND_FAILURE);
        }
        glfwSetMonitorCallback(
            [](GLFWmonitor*, int)
            {
                if (display_revision == UINT64_MAX)
                {
                    std::abort();
                }
                ++display_revision;
            }
        );
        auto runtime = std::unique_ptr<GlfwRuntime>(new GlfwRuntime());
        runtime_owner = runtime.get();
        return runtime;
    }

    GlfwRuntime::~GlfwRuntime() noexcept
    {
        glfwSetMonitorCallback(nullptr);
        glfwTerminate();
        runtime_owner = nullptr;
    }

} // namespace lux::window

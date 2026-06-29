#pragma once
#include <lux/engine/window/visibility.h>

struct GLFWwindow;

namespace lux::window
{
    /// RAII guard for the GLFW library lifetime.
    /// Typically one instance per process, created before any LuxWindow.
    class LUX_PLATFORM_WINDOW_PUBLIC GlfwRuntime
    {
    public:
        GlfwRuntime();
        ~GlfwRuntime();

        /// Returns true if glfwInit() succeeded.
        [[nodiscard]] bool valid() const noexcept { return valid_; }

        // Non-copyable, non-movable.
        GlfwRuntime(const GlfwRuntime&) = delete;
        GlfwRuntime& operator=(const GlfwRuntime&) = delete;
        GlfwRuntime(GlfwRuntime&&) = delete;
        GlfwRuntime& operator=(GlfwRuntime&&) = delete;

    private:
        bool valid_ = false;
    };

} // namespace lux::window

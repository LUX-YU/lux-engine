#pragma once
#include <lux/engine/window/visibility.h>
#include <cstdint>

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
        [[nodiscard]] bool valid() const noexcept
        {
            return valid_;
        }

        // Owner-thread fact generation. A host re-queries displays only when this
        // changes; it need not enumerate monitors on every frame.
        [[nodiscard]] static std::uint64_t displayRevision() noexcept;

        // Non-copyable, non-movable.
        GlfwRuntime(const GlfwRuntime&) = delete;
        GlfwRuntime& operator=(const GlfwRuntime&) = delete;
        GlfwRuntime(GlfwRuntime&&) = delete;
        GlfwRuntime& operator=(GlfwRuntime&&) = delete;

    private:
        bool valid_ = false;
    };

} // namespace lux::window

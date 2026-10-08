#pragma once
#include <cstdint>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/window/visibility.h>
#include <memory>

namespace lux::window
{
    enum class EGlfwInitError : std::uint8_t
    {
        BACKEND_FAILURE,
        ALREADY_ACTIVE
    };

    /// One platform-thread owner, created before and destroyed after all native windows.
    class LUX_PLATFORM_WINDOW_PUBLIC GlfwRuntime final
    {
    public:
        using CreateResult = lux::cxx::expected<std::unique_ptr<GlfwRuntime>, EGlfwInitError>;
        [[nodiscard]] static CreateResult create() noexcept;
        ~GlfwRuntime() noexcept;

        // Owner-thread fact generation. A host re-queries displays only when this
        // changes; it need not enumerate monitors on every frame.
        [[nodiscard]] static std::uint64_t displayRevision() noexcept;

        // Non-copyable, non-movable.
        GlfwRuntime(const GlfwRuntime&) = delete;
        GlfwRuntime& operator=(const GlfwRuntime&) = delete;
        GlfwRuntime(GlfwRuntime&&) = delete;
        GlfwRuntime& operator=(GlfwRuntime&&) = delete;

    private:
        GlfwRuntime() noexcept = default;
    };

} // namespace lux::window

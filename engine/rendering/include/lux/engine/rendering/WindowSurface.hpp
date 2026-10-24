#pragma once

#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/window/NativeWindowHandle.hpp>

namespace lux::rendering
{
    /// Translate a supported native window into a borrowed surface description, without creating GPU objects.
    /// Returns false for unsupported/empty handles. The window must remain alive through target retirement.
    [[nodiscard]] bool makeWindowSurface(
        const window::VNativeWindowHandle& native_window,
        bool vertical_sync,
        render::SurfaceDescription& surface
    ) noexcept;
} // namespace lux::rendering

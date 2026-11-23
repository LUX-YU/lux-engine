#pragma once

#include <lux/engine/systems/render/visibility.h>

#include <lux/engine/render/runtime/Target.hpp>
#include <lux/engine/window/NativeWindowHandle.hpp>

namespace lux::engine::render
{
    /// Translate a supported native window into a borrowed surface description, without creating GPU objects.
    /// Returns false for unsupported/empty handles. The window must remain alive through target retirement.
    [[nodiscard]] LUX_ENGINE_SYSTEM_RENDER_PUBLIC bool makeWindowSurface(
        const window::VNativeWindowHandle& native_window,
        bool vertical_sync,
        lux::render::SurfaceDescription& surface
    ) noexcept;
} // namespace lux::engine::render

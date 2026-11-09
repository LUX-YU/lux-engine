#include <lux/engine/systems/render/WindowSurface.hpp>

namespace lux::engine::render
{
    bool makeWindowSurface(
        const window::VNativeWindowHandle& native_window,
        bool vertical_sync,
        lux::render::SurfaceDescription& surface
    ) noexcept
    {
        const auto* win32 = std::get_if<window::Win32WindowHandle>(&native_window);
        if (!win32 || !win32->window)
        {
            return false;
        }
        surface = {};
        surface.window = lux::render::Win32Surface{win32->window};
        surface.vertical_sync = vertical_sync;
        return true;
    }
} // namespace lux::engine::render

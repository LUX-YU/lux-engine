#include <lux/engine/rendering/WindowSurface.hpp>

namespace lux::rendering
{
    bool makeWindowSurface(
        const window::VNativeWindowHandle& native_window,
        bool vertical_sync,
        render::SurfaceDescription& surface
    ) noexcept
    {
        const auto* win32 = std::get_if<window::Win32WindowHandle>(&native_window);
        if (!win32 || !win32->window)
        {
            return false;
        }
        surface = {};
        surface.window.windows.hwnd = win32->window;
        surface.vertical_sync = vertical_sync;
        return true;
    }
} // namespace lux::rendering

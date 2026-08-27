#pragma once
#ifdef __TRAY_ENABLED__
#include <memory>
#include <lux/engine/window/visibility.h>

namespace lux::window
{
    class LuxWindow;

    class TrayIcon
    {
    public:
        LUX_PLATFORM_WINDOW_PUBLIC explicit TrayIcon(LuxWindow& window);

        TrayIcon(const TrayIcon&) = delete;
        TrayIcon& operator=(const TrayIcon&) = delete;

        LUX_PLATFORM_WINDOW_PUBLIC TrayIcon(TrayIcon&&) noexcept;
        LUX_PLATFORM_WINDOW_PUBLIC TrayIcon& operator=(TrayIcon&&) noexcept;

        LUX_PLATFORM_WINDOW_PUBLIC ~TrayIcon();

    private:
        class Impl;
        std::unique_ptr<Impl> _impl;
    };
}
#endif

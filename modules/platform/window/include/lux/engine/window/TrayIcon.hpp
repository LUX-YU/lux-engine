#pragma once
#ifdef __TRAY_ENABLED__
#include <cstdint>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/window/visibility.h>
#include <memory>

namespace lux::window
{
    class LuxWindow;

    enum class ETrayError : std::uint8_t
    {
        ALREADY_ATTACHED,
        MENU_CREATION_FAILED,
        MENU_ITEM_FAILED,
        ICON_UNAVAILABLE,
        REGISTRATION_FAILED,
        CALLBACK_REGISTRATION_FAILED
    };

    // Platform-thread owner. One tray registration per window; different windows are independent.
    // Closing hides the window while attached. Destruction restores the previous close policy.
    // Native-window destruction revokes the registration before its borrowed window becomes invalid.
    class TrayIcon final
    {
    public:
        using CreateResult = lux::cxx::expected<std::unique_ptr<TrayIcon>, ETrayError>;
        [[nodiscard]] LUX_PLATFORM_WINDOW_PUBLIC static CreateResult create(LuxWindow&) noexcept;
        LUX_PLATFORM_WINDOW_PUBLIC ~TrayIcon() noexcept;

        TrayIcon(const TrayIcon&) = delete;
        TrayIcon& operator=(const TrayIcon&) = delete;

        TrayIcon(TrayIcon&&) = delete;
        TrayIcon& operator=(TrayIcon&&) = delete;

    private:
        struct Impl;
        explicit TrayIcon(std::shared_ptr<Impl>) noexcept;
        // A synchronous native menu callback pins its physical state through reentrant teardown.
        std::shared_ptr<Impl> impl_;
    };
} // namespace lux::window
#endif

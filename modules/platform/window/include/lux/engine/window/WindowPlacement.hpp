#pragma once

#include <cstdint>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/window/visibility.h>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lux::window
{
    // All rectangles use signed desktop coordinates and window content units,
    // never framebuffer pixels. Display enumeration order is not persistent identity.
    struct WindowRect final
    {
        int x{}, y{}, width{}, height{};
        bool operator==(const WindowRect&) const = default;
    };

    struct WindowSize final
    {
        int width{}, height{};
    };

    struct ContentScale final
    {
        float x{1.f}, y{1.f};
    };

    struct WindowInsets final
    {
        int left{8}, top{32}, right{8}, bottom{8};
    };

    struct DisplayMode final
    {
        int width{}, height{}, refresh_rate{};
    };

    struct DisplayHint final
    {
        std::string name;
        WindowRect work_area;
    };

    struct DisplayInfo final
    {
        DisplayHint hint;
        WindowRect bounds;
        ContentScale scale;
        DisplayMode current_mode;
        std::vector<DisplayMode> modes;
        bool primary{};
    };
    enum class EWindowMode : std::uint8_t
    {
        ORDINARY,
        MAXIMIZED,
        FULLSCREEN
    };

    struct WindowPlacement final
    {
        WindowRect normal;
        EWindowMode mode{EWindowMode::ORDINARY};
        DisplayHint display;
    };

    struct WindowPlacementRequest final
    {
        std::optional<WindowPlacement> saved;
        std::optional<WindowSize> size;
        std::optional<EWindowMode> mode;
        std::optional<DisplayHint> display;
    };

    struct WindowState final
    {
        WindowPlacement placement;
        WindowRect content;
        ContentScale scale;
        WindowInsets insets;
        bool minimized{};
    };
    enum class EWindowPlacementError : std::uint8_t
    {
        INVALID_REQUEST,
        NO_DISPLAY,
        UNSUPPORTED = 3,
        PLATFORM
    };

    struct WindowPlacementFailure final
    {
        EWindowPlacementError code;
        std::string detail;
    };

    struct ResolvedWindowPlacement final
    {
        WindowPlacement placement;
        bool adjusted{};
    };

    using WindowPlacementResult = lux::cxx::expected<ResolvedWindowPlacement, WindowPlacementFailure>;

    // Pure, bounded policy. Insets reserve the outer decoration inside the work
    // area; after creation the backend supplies its actual frame insets.
    [[nodiscard]] LUX_PLATFORM_WINDOW_PUBLIC WindowPlacementResult
    resolveWindowPlacement(const WindowPlacementRequest&, std::span<const DisplayInfo>, WindowInsets = {}) noexcept;
} // namespace lux::window

#pragma once
#include <lux/cxx/compile_time/expected.hpp>
#include <cstdint>
#include <string>
#include <filesystem>
#include <optional>
#include <lux/engine/ui/UiFontSource.hpp>

namespace lux::editor::ui
{
    struct WorkspaceId final
    {
        std::uint64_t value{};
        friend constexpr bool operator==(WorkspaceId, WorkspaceId) noexcept = default;
    };
    struct WindowFontSpec final
    {
        std::filesystem::path file; // Explicit cold input; the Editor never searches system font directories.
        std::uint32_t face{};
        float size_pixels{18};
        std::vector<lux::ui::UiGlyphRange> ranges{{0x20, 0x7E}, {0x3000, 0x303F}, {0x4E00, 0x9FFF}, {0xFF00, 0xFFEF}};
    };
    struct WindowSpec final
    {
        std::uint32_t width{1600}, height{900};
        std::string title{"Lux Editor"};
        bool visible{true};
        std::optional<WindowFontSpec> font;
    };
    enum class EWindowError : std::uint8_t
    {
        INVALID_ARGUMENT,
        WRONG_THREAD,
        BUSY,
        CLOSED,
        PLATFORM_FAILURE,
        UI_FAILURE,
        LAYOUT_FAILURE,
        ALLOCATION_FAILURE,
        FONT_OPEN_FAILURE,
        FONT_READ_FAILURE,
        FONT_LIMIT,
        UI_INITIALIZATION_FAILURE
    };
    struct WindowFailure final
    {
        EWindowError code{};
        std::uint64_t request{};
        std::optional<lux::ui::EUiInitError> ui_initialization;
    };
    enum class ETextInputPlatformState : std::uint8_t
    {
        INACTIVE,
        APPLIED,
        UNAVAILABLE,
        INVALID_COORDINATES,
        PLATFORM_FAILURE
    };
    struct TextInputPlatformStatus final
    {
        ETextInputPlatformState state{ETextInputPlatformState::INACTIVE};
        std::uint64_t frame{};
        bool composition_positioned{}, candidate_positioned{};
    };
    template <class T> using WindowResult = lux::cxx::expected<T, WindowFailure>;
} // namespace lux::editor::ui

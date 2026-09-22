#pragma once
#include <cstdint>
#include <filesystem>
#include <lux/engine/ui/UiFontSource.hpp>
#include <optional>
#include <string>

namespace lux::editor::gui
{
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
} // namespace lux::editor::gui

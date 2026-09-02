#pragma once

#include <lux/engine/function/visibility.h>
#include <lux/engine/ui/Geometry.hpp>

namespace lux::ui
{
    struct ThemeTypography final
    {
        float body_size{15.0F};
        float heading_size{17.0F};
    };

    struct ThemePalette final
    {
        Color window_background;
        Color panel_background;
        Color field_background;
        Color text;
        Color muted_text;
        Color border;
        Color accent;
        Color selection;
        Color disabled;
        Color warning;
        Color error;
    };

    struct ThemeSpacing final
    {
        Vec2 compact;
        Vec2 item;
        Vec2 section;
        Vec2 panel_padding;
    };

    struct ThemeMetrics final
    {
        float row_height{24.0F};
        float toolbar_height{30.0F};
        float rounding{4.0F};
        float border_width{1.0F};
        float property_label_width{140.0F};
        float tree_indent{18.0F};
        float asset_tile_size{96.0F};
    };

    struct Theme final
    {
        ThemeTypography typography;
        ThemePalette palette;
        ThemeSpacing spacing;
        ThemeMetrics metrics;

        [[nodiscard]] static LUX_FUNCTION_PUBLIC Theme luxDark() noexcept;
    };
} // namespace lux::ui

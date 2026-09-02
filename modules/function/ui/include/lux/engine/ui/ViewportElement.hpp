#pragma once

#include <cstddef>
#include <optional>
#include <span>

#include <lux/engine/function/visibility.h>
#include <lux/engine/ui/DragDrop.hpp>
#include <lux/engine/ui/Frame.hpp>
#include <lux/engine/ui/Geometry.hpp>
#include <lux/engine/ui/TextureHandle.hpp>
#include <lux/engine/ui/UiIds.hpp>

namespace lux::ui
{
    struct ViewportSpec final
    {
        TextureHandle texture;
        Vec2 uv_min{};
        Vec2 uv_max{1.0F, 1.0F};
    };

    struct ViewportResult final
    {
        Size size{};
        Point content_origin{};
        Point local_pointer{};
        bool hovered{false};
        bool window_focused{false};
        bool resized{false};
        bool left_clicked{false};
        bool middle_clicked{false};
        bool right_clicked{false};
        std::optional<DragDropPayloadView> drop;
    };

    class LUX_FUNCTION_PUBLIC ViewportElement final
    {
    public:
        [[nodiscard]] ViewportResult draw(Frame& frame, const ViewportSpec& spec);

    private:
        Size previous_size_{};
    };
} // namespace lux::ui

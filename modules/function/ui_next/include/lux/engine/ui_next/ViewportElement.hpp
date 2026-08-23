#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <imgui.h>

#include <lux/engine/ui_next/DragDrop.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/ui_next/UiIds.hpp>

namespace lux::ui
{
    struct ViewportResult final
    {
        ImVec2 size{};
        ImVec2 content_origin{};
        ImVec2 local_pointer{};
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
        [[nodiscard]] ViewportResult
        draw(ImTextureID texture, ImVec2 uv0 = {0.0F, 0.0F}, ImVec2 uv1 = {1.0F, 1.0F});

    private:
        ImVec2 previous_size_{};
    };
} // namespace lux::ui

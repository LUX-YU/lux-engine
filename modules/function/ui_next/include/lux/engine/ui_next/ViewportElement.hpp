#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include <imgui.h>

#include <lux/engine/function/visibility.h>
#include <lux/engine/ui_next/UiIds.hpp>

namespace lux::ui
{
    struct ViewportDrop final
    {
        PayloadTypeId type;
        std::vector<std::byte> bytes;
    };

    struct ViewportResult final
    {
        ImVec2 size{};
        ImVec2 content_origin{};
        ImVec2 local_pointer{};
        bool hovered{false};
        bool focused{false};
        bool resized{false};
        bool left_clicked{false};
        bool middle_clicked{false};
        bool right_clicked{false};
        std::optional<ViewportDrop> drop;
    };

    class LUX_FUNCTION_PUBLIC ViewportElement final
    {
    public:
        [[nodiscard]] ViewportResult
        draw(ImTextureID texture, ImVec2 uv0 = {0.0F, 0.0F}, ImVec2 uv1 = {1.0F, 1.0F});

    private:
        ImVec2 previous_size_{};
    };

    LUX_FUNCTION_PUBLIC void
    setDragDropPayload(PayloadTypeIdView type, std::span<const std::byte> bytes);
} // namespace lux::ui

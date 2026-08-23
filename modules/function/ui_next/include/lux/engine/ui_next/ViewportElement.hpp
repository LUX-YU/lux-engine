#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include <imgui.h>

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
        bool hovered{false};
        bool focused{false};
        bool resized{false};
        std::optional<ViewportDrop> drop;
    };

    class ViewportElement final
    {
      public:
        [[nodiscard]] ViewportResult draw(
            ImTextureID texture,
            ImVec2 uv0 = {0.0F, 0.0F},
            ImVec2 uv1 = {1.0F, 1.0F}
        );

      private:
        ImVec2 previous_size_{};
    };

    void setDragDropPayload(PayloadTypeIdView type, std::span<const std::byte> bytes);
}

#include <lux/engine/ui/ViewportElement.hpp>

#include <imgui.h>

#include <lux/engine/ui/detail/DragDropEncoding.hpp>

namespace lux::ui
{
    ViewportResult ViewportElement::draw(Frame& frame, const ViewportSpec& spec)
    {
        static_cast<void>(frame);
        ViewportResult result;
        const auto available = ImGui::GetContentRegionAvail();
        const auto origin = ImGui::GetCursorScreenPos();
        result.size = {available.x, available.y};
        result.content_origin = {origin.x, origin.y};
        result.resized = result.size != previous_size_;
        previous_size_ = result.size;
        ImGui::Image(
            static_cast<ImTextureID>(spec.texture.value),
            available,
            ImVec2{spec.uv_min.x, spec.uv_min.y},
            ImVec2{spec.uv_max.x, spec.uv_max.y}
        );
        result.hovered = ImGui::IsItemHovered();
        const auto pointer = ImGui::GetIO().MousePos;
        result.local_pointer = {pointer.x - origin.x, pointer.y - origin.y};
        result.left_clicked = result.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        result.middle_clicked = result.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
        result.right_clicked = result.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
        result.window_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

        result.drop = detail::acceptDragDropPayload();
        return result;
    }
} // namespace lux::ui

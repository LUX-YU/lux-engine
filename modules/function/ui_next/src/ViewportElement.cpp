#include <lux/engine/ui_next/ViewportElement.hpp>

#include <array>
#include <vector>

#include <lux/engine/ui_next/detail/DragDropEncoding.hpp>

namespace lux::ui
{
    namespace
    {
        constexpr const char* kPayloadName = "LUX_UI_PAYLOAD";

    } // namespace

    void setDragDropPayload(PayloadTypeIdView type, std::span<const std::byte> bytes)
    {
        std::array<std::byte, detail::kInlineDragDropBytes> inline_storage{};
        std::vector<std::byte> heap_storage;
        const auto encoded = detail::encodeDragDropPayload(
            type,
            bytes,
            inline_storage,
            heap_storage
        );
        ImGui::SetDragDropPayload(kPayloadName, encoded.data(), encoded.size());
    }

    ViewportResult ViewportElement::draw(ImTextureID texture, ImVec2 uv0, ImVec2 uv1)
    {
        ViewportResult result;
        result.size = ImGui::GetContentRegionAvail();
        result.content_origin = ImGui::GetCursorScreenPos();
        result.resized =
            result.size.x != previous_size_.x || result.size.y != previous_size_.y;
        previous_size_ = result.size;
        ImGui::Image(texture, result.size, uv0, uv1);
        result.hovered = ImGui::IsItemHovered();
        const auto pointer = ImGui::GetIO().MousePos;
        result.local_pointer = {
            pointer.x - result.content_origin.x,
            pointer.y - result.content_origin.y
        };
        result.left_clicked =
            result.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        result.middle_clicked =
            result.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
        result.right_clicked =
            result.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
        result.window_focused =
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

        if (ImGui::BeginDragDropTarget())
        {
            if (const auto* payload = ImGui::AcceptDragDropPayload(kPayloadName))
            {
                const auto bytes = std::span{
                    static_cast<const std::byte*>(payload->Data),
                    static_cast<std::size_t>(payload->DataSize)
                };
                result.drop = detail::decodeDragDropPayload(bytes);
            }
            ImGui::EndDragDropTarget();
        }
        return result;
    }
} // namespace lux::ui

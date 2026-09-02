#include <lux/engine/ui/DragDrop.hpp>

#include <array>
#include <vector>

#include <imgui.h>

#include <lux/engine/ui/detail/DragDropEncoding.hpp>

namespace lux::ui
{
    namespace
    {
        constexpr const char* kPayloadName = "LUX_UI_PAYLOAD";
    }

    void setDragDropPayload(PayloadTypeIdView type, std::span<const std::byte> bytes)
    {
        std::array<std::byte, detail::kInlineDragDropBytes> inline_storage{};
        std::vector<std::byte> heap_storage;
        const auto encoded = detail::encodeDragDropPayload(type, bytes, inline_storage, heap_storage);
        ImGui::SetDragDropPayload(kPayloadName, encoded.data(), encoded.size());
    }

    namespace detail
    {
        std::optional<DragDropPayloadView> acceptDragDropPayloadInActiveTarget()
        {
            std::optional<DragDropPayloadView> result;
            if (const auto* payload = ImGui::AcceptDragDropPayload(kPayloadName))
            {
                const auto bytes = std::span{
                    static_cast<const std::byte*>(payload->Data),
                    static_cast<std::size_t>(payload->DataSize)
                };
                result = decodeDragDropPayload(bytes);
            }
            return result;
        }

        std::optional<DragDropPayloadView> acceptDragDropPayload()
        {
            if (!ImGui::BeginDragDropTarget())
                return std::nullopt;
            auto result = acceptDragDropPayloadInActiveTarget();
            ImGui::EndDragDropTarget();
            return result;
        }
    } // namespace detail
} // namespace lux::ui

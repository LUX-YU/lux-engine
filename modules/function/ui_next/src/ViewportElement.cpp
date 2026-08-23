#include <lux/engine/ui_next/ViewportElement.hpp>

#include <cstring>
#include <string>

namespace lux::ui
{
    namespace
    {
        constexpr const char* kPayloadName = "LUX_UI_PAYLOAD";

        struct PayloadHeader final
        {
            std::uint64_t hash;
            std::uint32_t name_size;
        };
    }

    void setDragDropPayload(PayloadTypeIdView type, std::span<const std::byte> bytes)
    {
        std::vector<std::byte> encoded(
            sizeof(PayloadHeader) + type.name().size() + bytes.size()
        );
        const PayloadHeader header{type.hash(), static_cast<std::uint32_t>(type.name().size())};
        std::memcpy(encoded.data(), &header, sizeof(header));
        std::memcpy(encoded.data() + sizeof(header), type.name().data(), type.name().size());
        std::memcpy(
            encoded.data() + sizeof(header) + type.name().size(),
            bytes.data(),
            bytes.size()
        );
        ImGui::SetDragDropPayload(kPayloadName, encoded.data(), encoded.size());
    }

    ViewportResult ViewportElement::draw(ImTextureID texture, ImVec2 uv0, ImVec2 uv1)
    {
        ViewportResult result;
        result.size = ImGui::GetContentRegionAvail();
        result.resized = result.size.x != previous_size_.x || result.size.y != previous_size_.y;
        previous_size_ = result.size;
        ImGui::Image(texture, result.size, uv0, uv1);
        result.hovered = ImGui::IsItemHovered();
        result.focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

        if (ImGui::BeginDragDropTarget())
        {
            if (const auto* payload = ImGui::AcceptDragDropPayload(kPayloadName))
            {
                const auto bytes = std::span{
                    static_cast<const std::byte*>(payload->Data),
                    static_cast<std::size_t>(payload->DataSize)
                };
                if (bytes.size() >= sizeof(PayloadHeader))
                {
                    PayloadHeader header{};
                    std::memcpy(&header, bytes.data(), sizeof(header));
                    if (header.name_size <= bytes.size() - sizeof(header))
                    {
                        const auto name = std::string_view{
                            reinterpret_cast<const char*>(bytes.data() + sizeof(header)),
                            header.name_size
                        };
                        if (auto id = PayloadTypeIdView::fromVerified(name, header.hash); id.isValid())
                        {
                            ViewportDrop drop{PayloadTypeId{name}, {}};
                            const auto content = bytes.subspan(sizeof(header) + header.name_size);
                            drop.bytes.assign(content.begin(), content.end());
                            result.drop = std::move(drop);
                        }
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        return result;
    }
}

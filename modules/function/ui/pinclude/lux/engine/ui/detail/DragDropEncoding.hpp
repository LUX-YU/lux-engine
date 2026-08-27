#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <lux/engine/ui/DragDrop.hpp>

namespace lux::ui::detail
{
    inline constexpr std::size_t kInlineDragDropBytes = 256;

    struct DragDropPayloadHeader final
    {
        std::uint64_t hash;
        std::uint32_t name_size;
    };

    [[nodiscard]] inline std::span<const std::byte> encodeDragDropPayload(
        PayloadTypeIdView type,
        std::span<const std::byte> bytes,
        std::array<std::byte, kInlineDragDropBytes>& inline_storage,
        std::vector<std::byte>& heap_storage
    )
    {
        const auto encoded_size = sizeof(DragDropPayloadHeader) + type.name().size() + bytes.size();
        std::span<std::byte> encoded;
        if (encoded_size <= inline_storage.size())
        {
            encoded = std::span<std::byte>{inline_storage}.first(encoded_size);
            heap_storage.clear();
        }
        else
        {
            heap_storage.resize(encoded_size);
            encoded = std::span<std::byte>{heap_storage};
        }

        const DragDropPayloadHeader header{type.hash(), static_cast<std::uint32_t>(type.name().size())};
        std::memcpy(encoded.data(), &header, sizeof(header));
        std::memcpy(encoded.data() + sizeof(header), type.name().data(), type.name().size());
        if (!bytes.empty())
        {
            std::memcpy(encoded.data() + sizeof(header) + type.name().size(), bytes.data(), bytes.size());
        }
        return encoded;
    }

    [[nodiscard]] inline std::optional<DragDropPayloadView>
    decodeDragDropPayload(std::span<const std::byte> bytes) noexcept
    {
        if (bytes.size() < sizeof(DragDropPayloadHeader))
            return std::nullopt;
        DragDropPayloadHeader header{};
        std::memcpy(&header, bytes.data(), sizeof(header));
        if (header.name_size > bytes.size() - sizeof(header))
            return std::nullopt;
        const auto name =
            std::string_view{reinterpret_cast<const char*>(bytes.data() + sizeof(header)), header.name_size};
        const auto id = PayloadTypeIdView::fromVerified(name, header.hash);
        if (!id.isValid())
            return std::nullopt;
        return DragDropPayloadView{id, bytes.subspan(sizeof(header) + header.name_size)};
    }

    [[nodiscard]] std::optional<DragDropPayloadView> acceptDragDropPayload();
} // namespace lux::ui::detail

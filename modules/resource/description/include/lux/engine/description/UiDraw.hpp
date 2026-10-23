#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>

namespace lux::rdesc
{
    // Opaque application token. Zero selects the UI font atlas. Capture resolves tokens on Main;
    // this value owns no image and contains no Renderer/ImGui type or callback.
    struct UiImageToken final
    {
        std::uint64_t value = 0;
        auto operator<=>(const UiImageToken&) const noexcept = default;
    };

    struct UiVertex final
    {
        std::array<float, 2> position, uv;
        std::uint32_t color;
    };

    struct UiDrawCommand final
    {
        std::uint32_t index_offset = 0, index_count = 0;
        std::int32_t vertex_offset = 0;
        std::array<float, 4> clip;
        UiImageToken image;
    };

    struct UiDrawData final
    {
        std::span<const UiVertex> vertices;
        std::span<const std::byte> indices;
        std::span<const UiDrawCommand> commands;
        std::array<float, 2> display_position{}, display_size{}, framebuffer_scale{1, 1};
        std::uint32_t index_stride = 2;
    };
} // namespace lux::rdesc

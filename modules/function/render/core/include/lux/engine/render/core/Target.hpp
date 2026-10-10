#pragma once

#include <cstdint>
#include <lux/engine/render/core/Identity.hpp>

namespace lux::render
{
    enum class ERenderTargetKind : std::uint8_t
    {
        OFFSCREEN,
        PRESENTATION
    };

    inline constexpr auto kSceneColorSemantic = renderTargetSemanticId("lux.render.target.scene_color.v1");
    inline constexpr auto kDepthSemantic = renderTargetSemanticId("lux.render.target.depth.v1");

    // Backend-neutral, immutable value projection populated by a later backend.
    // Zero means no reported capacity. This value does not select formats,
    // allocate resources or promise optional backend capabilities.
    struct DeviceCaps final
    {
        std::uint32_t max_image_dimension_2d{};
        std::uint32_t max_color_attachments{};
    };
}

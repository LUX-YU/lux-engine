#pragma once

#include <lux/engine/render/effect/CapabilityDefinition.hpp>
#include <lux/engine/render/visibility.h>

namespace lux::render
{
    // Forward ZO depth uses near=0 and far=1. Each pyramid texel stores the farthest covered depth.
    struct DepthPyramid final
    {
        static constexpr std::string_view capability = "lux.render.depth_pyramid";
        static constexpr std::string_view forward_zo = "lux.render.depth_pyramid.forward_zo";
        static constexpr std::uint32_t version = 1;
        static constexpr auto depth = effectPortSemantic("depth");
        static constexpr auto current = effectPortSemantic("current");
        static constexpr auto previous = effectPortSemantic("previous");
        static constexpr std::string_view depth_type = "lux.depth.forward-zo";
        static constexpr std::string_view pyramid_type = "lux.depth.forward-zo-pyramid";

        [[nodiscard]] LUX_RENDER_PUBLIC static CapabilityDefinition definition() noexcept;
    };
} // namespace lux::render

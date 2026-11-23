#pragma once

#include <lux/engine/render/effect/EffectNodeDefinition.hpp>
#include <lux/engine/render/visibility.h>

namespace lux::render
{
    // Forward ZO depth uses near=0 and far=1. Each pyramid texel stores the farthest covered depth.
    namespace DepthPyramid
    {
        inline constexpr std::string_view capability = "lux.render.depth_pyramid";
        inline constexpr std::string_view forward_zo = "lux.render.depth_pyramid.forward_zo";
        inline constexpr std::uint32_t version = 1;
        inline constexpr auto depth = effectPortSemantic("depth");
        inline constexpr auto current = effectPortSemantic("current");
        inline constexpr auto previous = effectPortSemantic("previous");
        inline constexpr std::string_view depth_type = "lux.depth.forward-zo";
        inline constexpr std::string_view pyramid_type = "lux.depth.forward-zo-pyramid";

        [[nodiscard]] LUX_RENDER_PUBLIC EffectNodeDefinition definition() noexcept;
    }
} // namespace lux::render

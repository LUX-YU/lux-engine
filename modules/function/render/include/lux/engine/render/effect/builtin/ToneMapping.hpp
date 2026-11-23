#pragma once

#include <lux/engine/render/effect/EffectNodeDefinition.hpp>
#include <lux/engine/render/visibility.h>

namespace lux::render
{
    namespace ToneMapping
    {
        inline constexpr std::string_view capability = "lux.render.tone_mapping";
        inline constexpr std::string_view linear = "lux.render.tone_mapping.linear";
        inline constexpr std::string_view srgb = "lux.render.tone_mapping.srgb";
        inline constexpr std::uint32_t version = 1;
        inline constexpr auto hdr_color = effectPortSemantic("hdr_color");
        inline constexpr auto color = effectPortSemantic("color");
        inline constexpr std::string_view display_type = "lux.color.display";

        [[nodiscard]] LUX_RENDER_PUBLIC EffectNodeDefinition definition() noexcept;
    }
} // namespace lux::render

#pragma once

#include <array>
#include <lux/engine/render/effect/EffectNodeDefinition.hpp>
#include <lux/engine/render/visibility.h>

namespace lux::render
{
    struct ShadowBundle final
    {
        static constexpr std::string_view type = "lux.lighting.shadow.forward-z";
        static constexpr std::uint32_t version = 1;
        static constexpr std::array<std::string_view, 4> members{
            "shadow_views", "shadow_slices", "shadow_ranges", "shadow_depth"
        };
    };

    namespace Shadow
    {
        inline constexpr std::string_view capability = "lux.render.shadow";
        inline constexpr std::string_view indexed = "lux.render.shadow.indexed";
        inline constexpr std::string_view indexed_clockwise = "lux.render.shadow.indexed.clockwise";
        inline constexpr std::string_view lights_binding = "shadow.lights";
        inline constexpr std::uint32_t version = 1;
        inline constexpr auto shadows = effectPortSemantic("shadows");
        inline constexpr auto draws = effectPortSemantic("draws");

        [[nodiscard]] LUX_RENDER_PUBLIC EffectNodeDefinition definition() noexcept;
    }
} // namespace lux::render

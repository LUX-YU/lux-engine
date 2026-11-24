#pragma once

#include <lux/engine/render/effect/EffectCapability.hpp>

namespace lux::render
{
    namespace VisibleDraws
    {
        inline constexpr std::string_view type = "lux.mesh.indexed-visibility";
        inline constexpr auto groups = effectPortSemantic("groups");
        inline constexpr auto visible = effectPortSemantic("visible");
        inline constexpr auto commands = effectPortSemantic("commands");
    }
}

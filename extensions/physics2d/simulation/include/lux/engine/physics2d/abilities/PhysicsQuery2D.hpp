#pragma once

#include <lux/engine/function/script/ScriptAbilityAnnotations.hpp>

namespace lux::physics2d
{
    struct LUX_SCRIPT_ABILITY(id = lux.physics2d.query,
                              name = Physics2D,
                              display = Physics Query 2D,
                              version = 1,
                              receiver = provider_instance) PhysicsQuery2D
    {
        LUX_SCRIPT_QUERY(id = lux.physics2d.query.overlaps_box, display = Overlaps Box, result_lifetime = owned_value)
        bool overlapsBox(LUX_SCRIPT_PARAM(lifetime = owned_value) double center_x,
                         LUX_SCRIPT_PARAM(lifetime = owned_value) double center_y,
                         LUX_SCRIPT_PARAM(lifetime = owned_value) double half_width,
                         LUX_SCRIPT_PARAM(lifetime = owned_value) double half_height) noexcept;
    };
}

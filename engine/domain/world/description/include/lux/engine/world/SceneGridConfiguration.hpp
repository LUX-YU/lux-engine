#pragma once

#include <lux/engine/math/Position.hpp>
#include <lux/engine/meta/MetaAnnotations.hpp>

namespace lux::world
{
    // The registered algorithm selects 2D or 3D. 2D requires origin.z == 0.
    struct LUX_TYPE_INFO(static) SceneGridConfiguration final
    {
        math::Position3d origin;
        double           cell_size{};
    };
} // namespace lux::world

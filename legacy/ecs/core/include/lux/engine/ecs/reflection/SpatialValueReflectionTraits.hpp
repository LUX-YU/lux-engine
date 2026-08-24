#pragma once

#include <lux/engine/math/Grid.hpp>
#include <lux/engine/math/Position.hpp>
#include <lux/engine/meta/MetaDef.hpp>

namespace lux::meta
{
    template <>
    inline constexpr bool is_reflected_value_v<lux::math::Position2d> = true;

    template <>
    inline constexpr bool is_reflected_value_v<lux::math::Position3d> = true;

    template <>
    inline constexpr bool is_reflected_value_v<lux::math::GridCoord2i64> = true;

    template <>
    inline constexpr bool is_reflected_value_v<lux::math::GridCoord3i64> = true;
} // namespace lux::meta

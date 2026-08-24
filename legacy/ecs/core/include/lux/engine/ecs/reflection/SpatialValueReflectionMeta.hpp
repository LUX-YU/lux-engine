#pragma once

#include <lux/engine/math/Position.hpp>
#include <lux/engine/math/Grid.hpp>

#include <lux/engine/ecs/reflection/SpatialValueReflectionTraits.hpp>

#include <lux/cxx/reflection/runtime/Marker.hpp>

// Math remains a pure value owner. ECS owns the runtime-reflection view needed
// by tagged component properties, registration sidecars and Lua bindings.
LUX_REFLECT_EXTERNAL(luxref::class, ::lux::math::Position2d)
LUX_REFLECT_EXTERNAL(luxref::class, ::lux::math::Position3d)
LUX_REFLECT_EXTERNAL(luxref::class, ::lux::math::GridCoord2i64)
LUX_REFLECT_EXTERNAL(luxref::class, ::lux::math::GridCoord3i64)

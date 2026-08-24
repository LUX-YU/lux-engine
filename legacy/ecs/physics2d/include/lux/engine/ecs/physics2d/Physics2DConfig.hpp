#pragma once
// ============================================================================
//  Physics2DConfig.hpp — world-level 2D physics parameters (lux::ecs).
//  Extracted from D2ScenePlan so the solver depends on physics vocabulary only
//  (ecs/scene → ecs/physics is the one-way dependency; never the reverse).
// ============================================================================

namespace lux::ecs
{
    struct Physics2DConfig
    {
        float gravity_x{0.f};      ///< world gravity (units/s²); +y is up
        float gravity_y{-9.81f};
    };

} // namespace lux::ecs

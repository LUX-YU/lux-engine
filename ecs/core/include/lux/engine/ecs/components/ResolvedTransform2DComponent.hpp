#pragma once
// ============================================================================
//  ResolvedTransform2DComponent.hpp — derived registry-space 2D transform.
//
//  Mirrors ResolvedTransform3DComponent. System-generated (TransformSystem),
//  NOT reflected, NOT persisted — the TransformSystem contract owns it (G-07).
//  Stores the absolute double position plus the resolved float linear basis.
//  Render extraction converts that pair into its backend-private tiled form.
// ============================================================================

#include <lux/engine/math/Position.hpp>
#include <Eigen/Core>

namespace lux::ecs
{
    struct ResolvedTransform2DComponent
    {
        lux::math::Position2d position{};
        Eigen::Matrix2f          linear = Eigen::Matrix2f::Identity();
    };

} // namespace lux::ecs

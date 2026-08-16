#pragma once
// ============================================================================
//  ResolvedTransform2DComponent.hpp — derived registry-space 2D transform.
//
//  Mirrors ResolvedTransform3DComponent. System-generated (TransformSystem),
//  NOT reflected, NOT persisted — the TransformSystem contract owns it (G-07).
//  Stores the absolute double position plus the resolved float linear basis.
//  Render extraction converts that pair into its backend-private tiled form.
// ============================================================================

#include <lux/engine/resource/spatial/Spatial.hpp>
#include <Eigen/Core>

namespace lux::ecs
{
    struct ResolvedTransform2DComponent
    {
        lux::spatial::Position2D position{};
        Eigen::Matrix2f          linear = Eigen::Matrix2f::Identity();
    };

} // namespace lux::ecs

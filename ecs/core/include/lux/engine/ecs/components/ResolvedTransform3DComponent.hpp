#pragma once
// ============================================================================
//  ResolvedTransform3DComponent.hpp — derived registry-space 3D transform.
//  System-generated (TransformSystem, G-07), NOT reflected, NOT persisted.
//  Symmetric-suffix naming (ADR §5): ResolvedTransform2D ↔ ResolvedTransform3D.
// ============================================================================
#include <lux/engine/math/Position.hpp>
#include <Eigen/Core>

namespace lux::ecs
{
    /// Double-precision resolved transform maintained by Transform3DSystem.
    /// System-generated (G-07), NOT reflected, NOT persisted.
    struct ResolvedTransform3DComponent
    {
        lux::math::Position3d position{};
        Eigen::Matrix3f          linear = Eigen::Matrix3f::Identity();
    };

} // namespace lux::ecs

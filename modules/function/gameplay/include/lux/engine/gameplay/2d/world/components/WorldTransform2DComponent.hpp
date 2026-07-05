#pragma once
// ============================================================================
//  WorldTransform2DComponent.hpp — derived world 2D transform (lux::gameplay::d2).
//
//  Mirrors d3::WorldTransformComponent. System-generated (Transform2DSystem), NOT
//  reflected, NOT persisted — the TransformSystem contract owns it (G-07). Stored as
//  a column-major 4x4 (the 2D affine embedded with z = 0, identity z-row/col) so the
//  render side reuses the same zero-copy std430 mat4 path as 3D instances
//  (EcsRenderTraits::InstanceTransform) — no separate 2D matrix layout.
// ============================================================================

#include <Eigen/Core>

namespace lux::gameplay::d2
{
    struct WorldTransform2DComponent
    {
        Eigen::Matrix4f world      = Eigen::Matrix4f::Identity();
        Eigen::Matrix4f prev_world = Eigen::Matrix4f::Identity();
        bool            dirty      = true;
    };

} // namespace lux::gameplay::d2

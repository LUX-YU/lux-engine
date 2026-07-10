#pragma once
// ============================================================================
//  WorldTransform2DComponent.hpp — derived world 2D transform (lux::pack).
//
//  Mirrors d3::WorldTransformComponent. System-generated (Transform2DSystem), NOT
//  reflected, NOT persisted — the TransformSystem contract owns it (G-07). Stored as
//  a column-major 4x4 (the 2D affine embedded with z = 0, identity z-row/col) so the
//  render side reuses the same zero-copy std430 mat4 path as 3D instances
//  (EcsRenderTraits::InstanceTransform) — no separate 2D matrix layout.
// ============================================================================

#include "Transform2DComponent.hpp"

#include <lux/engine/meta/LuxObject.hpp>
#include <Eigen/Core>

namespace lux::pack
{
    struct WorldTransform2DComponent
    {
        Eigen::Matrix4f world      = Eigen::Matrix4f::Identity();
        Eigen::Matrix4f prev_world = Eigen::Matrix4f::Identity();
        bool            dirty      = true;

        /// HierarchicalTransformSystem's value-based skip gate: the EXACT compose
        /// inputs `world` was last recomposed from — the local pose and the parent
        /// entity whose world was consumed (null = composed as a root). Same inputs
        /// next frame → the compose is skipped with zero writes. System-owned
        /// scratch; never read these as gameplay state.
        Transform2DComponent last_local;
        lux::meta::entity_id last_parent = lux::meta::null_entity;
    };

} // namespace lux::pack

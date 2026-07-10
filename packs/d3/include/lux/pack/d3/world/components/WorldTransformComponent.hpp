#pragma once
#include "TransformComponent.hpp"

#include <lux/engine/meta/LuxObject.hpp>
#include <Eigen/Core>
#include <lux/engine/ecs/World.hpp>

namespace lux::pack
{
    using lux::ecs::World;

    /// World-space transform matrix, maintained by TransformSystem.
    /// System-generated (G-07), NOT reflected, NOT persisted.
    struct WorldTransformComponent
    {
        Eigen::Matrix4f world      = Eigen::Matrix4f::Identity();
        Eigen::Matrix4f prev_world = Eigen::Matrix4f::Identity();
        bool            dirty      = true;

        /// HierarchicalTransformSystem's value-based skip gate: the EXACT compose
        /// inputs `world` was last recomposed from — the local pose and the parent
        /// entity whose world was consumed (null = composed as a root). Same inputs
        /// next frame → the compose is skipped with zero writes. System-owned
        /// scratch; never read these as gameplay state.
        TransformComponent   last_local;
        lux::meta::entity_id last_parent = lux::meta::null_entity;
    };

} // namespace lux::pack

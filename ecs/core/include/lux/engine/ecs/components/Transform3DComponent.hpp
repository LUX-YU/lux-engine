#pragma once
// ============================================================================
//  Transform3DComponent.hpp — local 3D pose (lux::ecs).
//  Symmetric-suffix naming (ecs-layer ADR §5): a component is dimension-suffixed
//  when a cross-dimension counterpart exists — Transform2D ↔ Transform3D. Shared
//  component (§4): TransformSystem composes it into a ResolvedTransform3DComponent
//  each frame; render nodes and physics read the world matrix.
// ============================================================================
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/resource/spatial/Spatial.hpp>
#include <Eigen/Core>
#include <Eigen/Geometry>

namespace lux::ecs
{
    /// Entity pose. Position is absolute for a root and parent-relative when a
    /// ParentComponent exists.
    struct LUX_COMPONENT() Transform3DComponent
    {
        LUX_MEMBER(display_name=Position, tooltip=3D position of the entity)
        lux::spatial::Position3D position{};

        LUX_MEMBER(display_name=Rotation, tooltip=Local rotation as a unit quaternion)
        Eigen::Quaternionf rotation = Eigen::Quaternionf::Identity();

        LUX_MEMBER(display_name=Scale, min=0.001, max=100.0, tooltip=Local scale per axis)
        Eigen::Vector3f    scale    = Eigen::Vector3f::Ones();

    };

} // namespace lux::ecs

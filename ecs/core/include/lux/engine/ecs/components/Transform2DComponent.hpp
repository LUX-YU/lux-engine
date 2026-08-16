#pragma once
// ============================================================================
//  Transform2DComponent.hpp — 2D pose (lux::ecs).
//
//  Mirrors Transform3DComponent. TransformSystem (the 2D variant) composes
//  this through the shared ParentComponent into a ResolvedTransform2DComponent.
//
//  Shared component (ADR §4): read by render nodes, physics and animation.
//  Annotated LUX_COMPONENT for future Inspector/serialisation; reflection is
//  wired when the editor gains 2D support.
// ============================================================================

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/resource/spatial/Spatial.hpp>
#include <Eigen/Core>

namespace lux::ecs
{
    struct LUX_COMPONENT() Transform2DComponent
    {
        /// Absolute registry-space position for a root; parent-relative for a
        /// child. Streaming and render-origin representations never leak here.
        LUX_MEMBER(display_name=Position, tooltip=2D position (x, y))
        lux::spatial::Position2D position{};

        /// Local rotation in RADIANS about +Z (counter-clockwise, screen-space
        /// convention). A single scalar — 2D has one rotational DOF.
        LUX_MEMBER(display_name=Rotation, tooltip=Local rotation in radians about +Z (CCW))
        float rotation = 0.0f;

        LUX_MEMBER(display_name=Scale, tooltip=Local 2D scale per axis (x, y); negative flips, zero is degenerate)
        Eigen::Vector2f scale = Eigen::Vector2f::Ones();

    };
} // namespace lux::ecs

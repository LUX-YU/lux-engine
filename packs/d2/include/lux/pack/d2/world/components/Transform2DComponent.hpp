#pragma once
// ============================================================================
//  Transform2DComponent.hpp — local 2D pose (lux::pack).
//
//  Mirrors d3::TransformComponent. Transform2DSystem composes this (through the
//  shared neutral HierarchyComponent — 2D does NOT define a Parent2D) into a
//  WorldTransform2DComponent each frame, auto-maintained (G-07). The world matrix
//  is a 4x4 (WorldTransform2DComponent) so the render side reuses the 3D zero-copy
//  InstanceTransform path.
//
//  NOTE: annotated LUX_COMPONENT for future Inspector/serialisation, but not yet in
//  the gameplay_meta TARGET_FILES list — reflection is wired when the editor gains
//  2D support (headless simulation + tests do not need it).
// ============================================================================

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <Eigen/Core>

namespace lux::pack
{
    struct LUX_COMPONENT() Transform2DComponent
    {
        LUX_MEMBER(display_name=Position, tooltip=Local 2D position (x, y))
        Eigen::Vector2f position = Eigen::Vector2f::Zero();

        /// Local rotation in RADIANS about +Z (counter-clockwise, screen-space
        /// convention). A single scalar — 2D has one rotational DOF.
        LUX_MEMBER(display_name=Rotation, tooltip=Local rotation in radians about +Z (CCW))
        float rotation = 0.0f;

        LUX_MEMBER(display_name=Scale, tooltip=Local 2D scale per axis (x, y); negative flips, zero is degenerate)
        Eigen::Vector2f scale = Eigen::Vector2f::Ones();

        /// Force Transform2DSystem to recompute the world transform. Internal state.
        bool LUX_NO_MEMBER() dirty = true;
    };

} // namespace lux::pack

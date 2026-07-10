#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>

#include <Eigen/Core>
#include <cmath>

namespace lux::pack
{
    /// Camera projection parameters and derived matrices.
    /// CameraSystem recomputes view/proj matrices from TransformComponent each frame.
    ///
    /// Configuration fields (fov, near/far, aspect, projection mode) are
    /// editable via the Inspector. Derived matrices (`view`, `proj`,
    /// `view_proj`, `prev_view_proj`) are intentionally hidden — they are
    /// rewritten by `CameraSystem::update` every tick, so user edits would
    /// be clobbered immediately.
    struct LUX_COMPONENT() CameraComponent
    {
        enum class Projection { Perspective, Orthographic };

        // ------------------------------------------------------------------ //
        //  Configuration                                                      //
        // ------------------------------------------------------------------ //

        // Enum reflection through a nested type is not yet supported by the
        // generator; hide from the Inspector for now. Edit programmatically.
        Projection LUX_NO_MEMBER() projection  = Projection::Perspective;

        /// Vertical field of view in radians.
        LUX_MEMBER(display_name=FOV Rad, min=0.1, max=3.14, tooltip=Vertical field of view in radians)
        float fov_rad   = 60.f * (3.14159265f / 180.f);

        LUX_MEMBER(display_name=Near, min=0.001, max=10.0)
        float near_z    = 0.1f;

        LUX_MEMBER(display_name=Far, min=1.0, max=10000.0)
        float far_z     = 1000.f;

        /// Viewport width / height.  Only used when auto_aspect = false.
        LUX_MEMBER(display_name=Aspect, min=0.1, max=10.0, tooltip=Used only when Auto Aspect is off)
        float aspect    = 16.f / 9.f;

        LUX_MEMBER(display_name=Auto Aspect, tooltip=Let the editor viewport drive the aspect ratio automatically)
        bool  auto_aspect = true;

        /// Orthographic half-extents (only relevant for Projection::Orthographic).
        LUX_MEMBER(display_name=Ortho Width)
        float ortho_width  = 10.f;

        LUX_MEMBER(display_name=Ortho Height)
        float ortho_height = 10.f;

        /// Flip the Y coordinate in the projection matrix (Vulkan NDC convention).
        LUX_MEMBER(display_name=Y Flip, tooltip=Bake Vulkan Y-down NDC into the projection matrix)
        bool y_flip = false;

        // ------------------------------------------------------------------ //
        //  Derived matrices — written by CameraSystem; hidden from Inspector //
        // ------------------------------------------------------------------ //

        Eigen::Matrix4f LUX_NO_MEMBER() view           = Eigen::Matrix4f::Identity();
        Eigen::Matrix4f LUX_NO_MEMBER() proj           = Eigen::Matrix4f::Identity();
        Eigen::Matrix4f LUX_NO_MEMBER() view_proj      = Eigen::Matrix4f::Identity();
        Eigen::Matrix4f LUX_NO_MEMBER() prev_view_proj = Eigen::Matrix4f::Identity();

        /// G-08 diagnostic: set by CameraSystem when the camera's world matrix carries
        /// scale (basis columns not unit-length — typically a scaled ancestor), which
        /// skews the view basis. Runtime-only (not persisted); the system also de-scales
        /// the basis so the view is not magnitude-distorted. Tools can surface this.
        bool LUX_NO_MEMBER() ancestry_scale_warning = false;
    };

} // namespace lux::pack

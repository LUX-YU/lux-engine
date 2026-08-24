#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>

#include <cmath>
#include <cstdint>

namespace lux::ecs
{
    enum class LUX_ENUM() ECameraProjection3D : std::uint8_t
    {
        PERSPECTIVE,
        ORTHOGRAPHIC
    };

    /// Authored 3D camera projection parameters. Pose comes exclusively from
    /// Transform3DComponent; derived matrices and the effective viewport aspect
    /// live in the non-cooked Camera3DCacheComponent.
    struct LUX_COMPONENT() Camera3DComponent
    {
        // ------------------------------------------------------------------ //
        //  Configuration                                                      //
        // ------------------------------------------------------------------ //

        LUX_MEMBER(display_name=Projection)
        ECameraProjection3D projection{ECameraProjection3D::PERSPECTIVE};

        /// Vertical field of view in radians.
        LUX_MEMBER(display_name=FOV Rad, min=0.1, max=3.14, tooltip=Vertical field of view in radians)
        float fov_rad   = 60.f * (3.14159265f / 180.f);

        LUX_MEMBER(display_name=Near, min=0.001, max=10.0)
        float near_z    = 0.1f;

        LUX_MEMBER(display_name=Far, min=1.0, max=10000.0)
        float far_z     = 1000.f;

        /// Authored fallback/fixed width-to-height ratio. Camera3DSystem never
        /// writes this field; auto-aspect is stored in Camera3DCacheComponent.
        LUX_MEMBER(display_name=Aspect, min=0.1, max=10.0, tooltip=Used only when Auto Aspect is off)
        float aspect    = 16.f / 9.f;

        LUX_MEMBER(display_name=Auto Aspect, tooltip=Let the editor viewport drive the aspect ratio automatically)
        bool  auto_aspect = true;

        /// Orthographic extents (only relevant for ORTHOGRAPHIC projection).
        LUX_MEMBER(display_name=Ortho Width)
        float ortho_width  = 10.f;

        LUX_MEMBER(display_name=Ortho Height)
        float ortho_height = 10.f;

    };

} // namespace lux::ecs

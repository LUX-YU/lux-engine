#pragma once
// ============================================================================
//  Camera2DComponent.hpp — 2D orthographic camera PARAMETERS only (lux::ecs).
//
//  Mirrors d3::CameraComponent's split: this holds ONLY projection/scale; the camera's
//  position + rotation come SOLELY from Transform2D / ResolvedTransform2D (single
//  source of truth — never duplicated here). The derived view/proj matrices live in a
//  separate Camera2DCacheComponent (system-generated). Design §2.2 (review V9).
//
//  Scale is a SINGLE base extent — `units_per_view_height` (world units mapped to the
//  full viewport height). A camera controller changes this value to zoom; there is no
//  independent `zoom` + `pixels_per_unit` (that would be an ambiguous double state).
//
//  The authored component and its scale-mode enum are reflected/cooked; all
//  viewport-derived matrices remain in Camera2DCacheComponent.
// ============================================================================

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <cstdint>

namespace lux::ecs
{

    /// How the fixed base extent maps to a viewport of a given aspect.
    enum class LUX_ENUM() ECamera2DScaleMode : std::uint8_t
    {
        FIT_HEIGHT = 0,   ///< view height = units_per_view_height; width follows aspect (default)
        FIT_WIDTH,        ///< view width  = units_per_view_height; height follows aspect
        STRETCH,          ///< both axes fixed to units_per_view_height (ignores aspect)
    };

    struct LUX_COMPONENT() Camera2DComponent
    {
        /// World units spanning the full viewport height (the single zoom control).
        LUX_MEMBER(display_name=Units Per View Height, min=0.001, max=100000.0)
        float units_per_view_height = 10.0f;

        /// Authored fallback width / height ratio. Camera2DSystem never writes
        /// this field; a bound viewport supplies the effective value in the
        /// transient Camera2DCacheComponent.
        LUX_MEMBER(display_name=Aspect, min=0.01, max=100.0)
        float aspect = 16.0f / 9.0f;

        LUX_MEMBER(display_name=Scale Mode)
        ECamera2DScaleMode scale_mode{ECamera2DScaleMode::FIT_HEIGHT};

        /// Snap the camera to whole pixels to avoid shimmer (applied render-side).
        LUX_MEMBER(display_name=Pixel Snap, tooltip=Snap camera to whole pixels to avoid shimmer)
        bool pixel_snap = false;
    };

    // 主相机的作者选择走维度中立的 PrimaryCameraTag；只认显式标签、
    // 绝不隐式取第一台
    // (设计 T2-03),恰好一台是契约 —— 见 activeCamera() 的判定。
    // (曾有 2D 专属的 ActiveCamera2DTag,2026-07-11 裁定「哪台相机是主相机」
    //  没有维度,已并入中立标签。)

} // namespace lux::ecs

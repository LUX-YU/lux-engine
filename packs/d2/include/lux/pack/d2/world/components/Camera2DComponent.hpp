#pragma once
// ============================================================================
//  Camera2DComponent.hpp — 2D orthographic camera PARAMETERS only (lux::pack).
//
//  Mirrors d3::CameraComponent's split: this holds ONLY projection/scale; the camera's
//  position + rotation come SOLELY from its Transform2D / WorldTransform2D (single
//  source of truth — never duplicated here). The derived view/proj matrices live in a
//  separate Camera2DCacheComponent (system-generated). Design §2.2 (review V9).
//
//  Scale is a SINGLE base extent — `units_per_view_height` (world units mapped to the
//  full viewport height). A camera controller changes this value to zoom; there is no
//  independent `zoom` + `pixels_per_unit` (that would be an ambiguous double state).
//
//  NOTE: annotated for reflection but not yet in the meta TARGET_FILES list (see
//  Transform2DComponent.hpp) — wired when the editor gains 2D support.
// ============================================================================

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <cstdint>
#include <lux/engine/ecs/World.hpp>

namespace lux::pack
{
    using lux::ecs::World;

    /// How the fixed base extent maps to a viewport of a given aspect.
    enum class Camera2DScaleMode : std::uint8_t
    {
        FitHeight = 0,   ///< view height = units_per_view_height; width follows aspect (default)
        FitWidth,        ///< view width  = units_per_view_height; height follows aspect
        Stretch,         ///< both axes fixed to units_per_view_height (ignores aspect)
    };

    struct LUX_COMPONENT() Camera2DComponent
    {
        /// World units spanning the full viewport height (the single zoom control).
        LUX_MEMBER(display_name=Units Per View Height, min=0.001, max=100000.0)
        float units_per_view_height = 10.0f;

        /// Viewport aspect (width / height). Driven by the render viewport (like d3's
        /// auto_aspect); stored so the projection is well-defined headless too.
        LUX_MEMBER(display_name=Aspect, min=0.01, max=100.0)
        float aspect = 16.0f / 9.0f;

        Camera2DScaleMode LUX_NO_MEMBER() scale_mode = Camera2DScaleMode::FitHeight;

        /// Bake a Y flip into the projection (Vulkan NDC is Y-down). Mirrors d3's y_flip.
        LUX_MEMBER(display_name=Y Flip, tooltip=Flip Y in the projection (Vulkan Y-down NDC))
        bool y_flip = false;

        /// Snap the camera to whole pixels to avoid shimmer (applied render-side).
        LUX_MEMBER(display_name=Pixel Snap, tooltip=Snap camera to whole pixels to avoid shimmer)
        bool pixel_snap = false;
    };

    /// Explicit "this is the scene's active camera" tag. The active camera is chosen by
    /// this tag ONLY — never the implicit first Camera2D (design T2-03: a host must pick
    /// deliberately). Exactly one tagged camera is the contract; see d2::activeCamera().
    struct ActiveCamera2DTag {};

} // namespace lux::pack

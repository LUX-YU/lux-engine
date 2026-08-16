#pragma once
/**
 * @file Grid3DComponent.hpp
 * @brief ECS-side description of the editor's reference grid.
 *
 * Fields mirror `lux::render::Grid3DSetParamsPayload` (plus an implicit
 * "is enabled = is component present"). `Grid3DSubsystem`(特性参数形状)
 * pushes `SetGrid3DParams` to the GridPass feature whenever a field changes.
 */

#include <lux/engine/meta/MetaAnnotations.hpp>

namespace lux::ecs
{

    struct LUX_COMPONENT() Grid3DComponent
    {
        LUX_MEMBER(display_name=Plane Y, min=-100.0, max=100.0, tooltip=World-space Y coordinate of the grid plane)
        float plane_y    = 0.f;

        LUX_MEMBER(display_name=Cell Size, min=0.05, max=50.0, tooltip=World-space distance between grid lines)
        float cell_size  = 1.f;

        LUX_MEMBER(display_name=Line Px, min=0.5, max=8.0, tooltip=On-screen width of grid lines in pixels)
        float line_px    = 1.1f;

        LUX_MEMBER(display_name=Fade Distance, min=1.0, max=500.0, tooltip=Distance beyond which the grid fades to invisible)
        float fade_dist  = 50.f;

        LUX_MEMBER(display_name=Hole Ratio, min=0.0, max=1.0, tooltip=Width of the bright accent line as a fraction of cell width)
        float hole_ratio = 0.45f;

        // Display priority is selectable (by design): false = depth-tested,
        // occluded by geometry (sits in the world); true = see-through
        // (X-ray — the fragment writes near-plane depth).
        LUX_MEMBER(display_name=On Top, tooltip=Draw the grid over geometry (X-ray) instead of depth-tested)
        bool on_top = false;
    };

} // namespace lux::ecs

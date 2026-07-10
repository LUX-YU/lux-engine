#pragma once
/**
 * @file GridComponent.hpp
 * @brief ECS-side description of the editor's reference grid.
 *
 * Fields mirror `lux::render::GridSetParamsPayload` (plus an implicit
 * "is enabled = is component present"). `EcsRenderTraits<GridComponent>`
 * (PARAM) pushes `SetGridParams` to the GridPass feature whenever a field changes.
 */

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/ecs/World.hpp>

namespace lux::pack
{
    using lux::ecs::World;

    struct LUX_COMPONENT() GridComponent
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
    };

} // namespace lux::pack

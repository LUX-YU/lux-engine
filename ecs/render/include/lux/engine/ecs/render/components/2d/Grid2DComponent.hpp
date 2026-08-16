#pragma once
/**
 * @file Grid2DComponent.hpp
 * @brief ECS-side description of the 2D scene's reference grid (XY content
 *        plane, orthographic). The Grid3DComponent sibling — parameters are
 *        adjusted on the grid ENTITY (by design: parameters are tuned on the
 *        top-level ECS entity, the same way for 2D and 3D).
 *
 * `Grid2DSubsystem`(特性参数形状)pushes `Grid2DSetParams` to the
 * Grid2DPass feature whenever a field changes. Display priority is
 * selectable: `on_top` selects whether the grid draws UNDER the 2D content
 * (default — the usual 2D-editor look) or OVER it.
 */

#include <lux/engine/meta/MetaAnnotations.hpp>

namespace lux::ecs
{
    struct LUX_COMPONENT() Grid2DComponent
    {
        LUX_MEMBER(display_name=Cell Size, min=0.05, max=100.0, tooltip=World-space size of one minor grid cell)
        float cell_size = 1.f;

        LUX_MEMBER(display_name=Major Every, min=2, max=50, tooltip=Minor cells per major (accent) line)
        int major_every = 10;

        LUX_MEMBER(display_name=Line Px, min=0.5, max=8.0, tooltip=On-screen width of grid lines in pixels)
        float line_px = 1.0f;

        LUX_MEMBER(display_name=On Top, tooltip=Draw the grid over the 2D content instead of under it)
        bool on_top = false;
    };

} // namespace lux::ecs

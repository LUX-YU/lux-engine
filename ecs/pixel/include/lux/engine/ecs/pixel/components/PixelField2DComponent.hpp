#pragma once
// ============================================================================
//  PixelField2DComponent.hpp — the ECS face of one pixel field (d2, F2-00).
//
//  The component contains authored placement/draw facts only. Cells and the
//  scene-local runtime handle live behind a transient binding component.
//
//  Placement: the entity's resolved Transform2D position is the field's min
//  corner (rotation/scale are intentionally unsupported). Runtime handles,
//  chunk residency and finite/infinite partition policy are intentionally not
//  authored component facts.
//
//  Annotated LUX_COMPONENT and registered by the Pixel schema sidecar.
// ============================================================================

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/resource/asset/AssetId.hpp>

#include <cstdint>

namespace lux::ecs
{

    struct LUX_COMPONENT() PixelField2DComponent
    {
        LUX_MEMBER(display_name=Definition, asset_type=pixel_field, tooltip=Pixel field definition asset)
        lux::asset::asset_id_t definition{};

        LUX_MEMBER(display_name=Material, asset_type=material, tooltip=Pixel field presentation material)
        lux::asset::asset_id_t material{};

        LUX_MEMBER(display_name=Cell Size, tooltip=World units per cell (> 0))
        double cell_size = 0.1;

        LUX_MEMBER(display_name=Priority, tooltip=Draw priority; higher is drawn on top)
        std::int32_t draw_priority = 0;

        LUX_MEMBER(display_name=Visible, tooltip=Whether the field is drawn)
        bool visible = true;

        LUX_MEMBER(display_name=Simulation Enabled, tooltip=Whether resident cells participate in simulation)
        bool simulation_enabled = true;
    };

} // namespace lux::ecs

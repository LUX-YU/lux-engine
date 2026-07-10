#pragma once
// ============================================================================
//  PixelField2DComponent.hpp — the ECS face of one pixel field (d2, F2-00).
//
//  The component is a HANDLE + placement/draw knobs — the field's cells live in
//  PixelFieldRuntime, so cell/chunk counts never touch entity counts. Exactly
//  ONE owning component references one field (MVP; read-only reference types
//  arrive when a consumer exists): the runtime's owner maintenance reclaims any
//  field no live component references (leak-proof by construction), so create
//  the field and assign the handle before the next simulation tick.
//
//  World placement: the entity's Transform2D → WorldTransform2D TRANSLATION is
//  the field's min corner (rotation/scale forbidden — the F2-02 MVP contract);
//  `cell_size` is the grid scale. `priority` is the unified canvas depth axis
//  (same float as sprites — fields interleave freely, the 2026-07-06 ruling).
//
//  Annotated LUX_COMPONENT for future Inspector/serialisation, but not yet in
//  the gameplay_meta reflection list (mirrors Transform2DComponent).
// ============================================================================

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/pack/d2/pixel/PixelFieldTypes.hpp>   // PixelFieldHandle
#include <lux/engine/ecs/World.hpp>

namespace lux::pack
{
    using lux::ecs::World;

    struct LUX_COMPONENT() PixelField2DComponent
    {
        /// Owning handle into the scene's PixelFieldRuntime. Internal state —
        /// created via the runtime API, not authored in the Inspector.
        PixelFieldHandle LUX_NO_MEMBER() field{};

        LUX_MEMBER(display_name=Cell Size, tooltip=World units per cell (> 0))
        float cell_size = 0.1f;

        LUX_MEMBER(display_name=Priority, tooltip=Draw priority; higher is drawn on top)
        float priority = 0.f;

        LUX_MEMBER(display_name=Visible, tooltip=Whether the field is drawn)
        bool visible = true;
    };

} // namespace lux::pack

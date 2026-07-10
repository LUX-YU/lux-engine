#pragma once
/**
 * @file MeshComponent.hpp
 * @brief Static (un-rigged) renderable mesh + material.
 *
 * Pair with WorldTransformComponent for placement; the RenderSystem
 * bridges (MeshComponent + WorldTransformComponent) to the renderer.
 */

#include <lux/engine/asset/Asset.hpp>   // for lux::asset::asset_id_t
#include <lux/engine/meta/MetaAnnotations.hpp>

namespace lux::pack
{
    struct LUX_COMPONENT() MeshComponent
    {
        /// UUID of the MeshAsset to render.
        lux::asset::asset_id_t mesh_asset_id;

        /// UUID of the MaterialAsset to use. Optional; renderer falls back to
        /// a default material if zero.
        lux::asset::asset_id_t material_asset_id;

        LUX_MEMBER(display_name=Cast Shadow, tooltip=Whether this mesh casts shadows in shadow passes)
        bool cast_shadow = true;

        LUX_MEMBER(display_name=Visible, tooltip=Hide without removing the entity)
        bool visible = true;

        /// Opt-in: render this mesh into the shadow map with both faces.
        ///
        /// The shadow pass uses conventional back-face culling by default.
        /// Closed solid meshes get correct shadowing — the light-facing
        /// surface is recorded, and self-shadow acne on the mesh itself is
        /// prevented by the slope-scale rasterizer depth bias. Single-face
        /// geometry whose surface normal points *toward* the light is also
        /// recorded correctly (a floor plane lit from above, an upright
        /// wall lit from the front).
        ///
        /// Set this flag to `true` only for genuinely two-sided geometry
        /// whose visible-from-camera side may face *away* from the light —
        /// cloth, leaves, billboards/cards, hair strips, alpha-cutout
        /// foliage. Such meshes need a CULL_NONE shadow pass so their back
        /// side also enters the shadow map.
        ///
        /// NOTE: wiring this flag to a CULL_NONE pipeline variant + per-
        /// instance routing in the shadow cull compute is a planned
        /// follow-up. Until then the flag is reflected for Inspector and
        /// serialisation-schema stability but has no GPU effect; the demo
        /// (cube + plane lit from above) is unaffected by that gap.
        LUX_MEMBER(display_name=Two-Sided Shadow, tooltip=Opt in only for two-sided geometry - cloth, cards, leaves. Solid meshes leave this off.)
        bool two_sided_shadow = false;
    };

} // namespace lux::pack

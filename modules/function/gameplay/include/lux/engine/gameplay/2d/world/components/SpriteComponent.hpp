#pragma once
// ============================================================================
//  SpriteComponent.hpp — a textured 2D quad (lux::gameplay::d2).
//
//  The minimal traditional-2D sprite (design §4 / Slice A §9): a texture region +
//  pivot + world size + tint + painter layer/order. Its world POSITION comes from the
//  entity's Transform2D → WorldTransform2D (composed by Transform2DSystem); this
//  component only adds the quad's own size/appearance/order. Sprite2DBridge (S2-01)
//  reads it each frame and submits a SpriteDraw to Canvas2DFeature.
//
//  MVP render is tint-only (R2-02): `texture` + `uv_rect` + `pivot` are carried for the
//  bindless-atlas texturing follow-up (R2-04/S2-xx) but the first-visible-sprite path
//  draws the flat `tint`. No SpriteAtlasAsset yet — uv_rect is authored/hardcoded.
//
//  NOTE: annotated LUX_COMPONENT for future Inspector/serialisation, but not yet in the
//  gameplay_meta TARGET_FILES list (mirrors Transform2DComponent) — headless tests do
//  not need reflection.
// ============================================================================

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/asset/Asset.hpp>   // lux::asset::asset_id_t
#include <Eigen/Core>

#include <cstdint>

namespace lux::gameplay::d2
{
    struct LUX_COMPONENT() SpriteComponent
    {
        /// Texture/atlas asset. Null → the tint-only quad (MVP). Sampled once texturing
        /// (bindless set 2) lands; the bridge resolves it to a bindless index then.
        LUX_MEMBER(display_name=Texture, tooltip=Texture or atlas asset (null = flat tint))
        lux::asset::asset_id_t texture{};

        /// UV sub-rect within the texture: (x, y, w, h) in [0,1]. Whole texture by default.
        LUX_MEMBER(display_name=UV Rect, tooltip=Atlas sub-rect x y w h in [0,1])
        Eigen::Vector4f uv_rect = Eigen::Vector4f(0.f, 0.f, 1.f, 1.f);

        /// Normalised pivot (0.5,0.5 = centred). The quad is placed so this point sits at
        /// the entity's world position.
        LUX_MEMBER(display_name=Pivot, tooltip=Normalised pivot; 0.5 0.5 is centred)
        Eigen::Vector2f pivot = Eigen::Vector2f(0.5f, 0.5f);

        /// Sprite size in WORLD units (multiplies the unit quad, on top of Transform2D scale).
        LUX_MEMBER(display_name=Size, tooltip=Sprite size in world units)
        Eigen::Vector2f size = Eigen::Vector2f::Ones();

        /// Premultiplied RGBA8 tint (R[7:0]..A[31:24]). Opaque white = no tint.
        LUX_MEMBER(display_name=Tint, tooltip=Premultiplied RGBA8 tint)
        std::uint32_t tint = 0xFFFFFFFFu;

        /// Painter layer — coarse draw order (→ DrawOrderKey.layer). Lower draws first.
        LUX_MEMBER(display_name=Layer, tooltip=Painter layer (lower draws first))
        std::int16_t layer = 0;

        /// Within-layer order (→ DrawOrderKey.order); may encode a y-sort later.
        LUX_MEMBER(display_name=Order, tooltip=Within-layer draw order)
        std::int32_t order = 0;

        /// Skip submission when false (still a live entity; just not drawn this frame).
        LUX_MEMBER(display_name=Visible, tooltip=Whether the sprite is drawn)
        bool visible = true;
    };

} // namespace lux::gameplay::d2

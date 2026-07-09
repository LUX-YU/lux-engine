#pragma once
// ============================================================================
//  SpriteComponent.hpp — a textured 2D quad (lux::gameplay::d2).
//
//  The minimal traditional-2D sprite (design §4, v2 GPU-driven): a texture region +
//  pivot + world size + tint + a single float draw PRIORITY. Its world POSITION comes
//  from the entity's Transform2D → WorldTransform2D (composed by Transform2DSystem);
//  this component only adds the quad's own size/appearance/priority. The retained
//  Sprite2DBridge owns one GPU-resident instance per sprite entity and pushes DELTAS
//  (create/remove/dirty transform/visual/key) — never per-frame content.
//
//  The bridge resolves `texture` to a bindless index and the sprite shader samples it at
//  `uv_rect`, modulated by `tint`; a null texture draws the flat tint. `pivot` places the
//  quad so its normalised pivot point sits at the entity's world position. There is no
//  SpriteAtlasAsset yet — `uv_rect` is authored/hardcoded.
//
//  Annotated LUX_COMPONENT for future Inspector/serialisation, but not yet in the
//  gameplay_meta reflection list (mirrors Transform2DComponent) — headless tests do not
//  need reflection.
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

        /// Draw priority — the ONE ordering knob (decision ① 2026-07-06): HIGHER
        /// priority draws ON TOP. Equal priorities tie-break deterministically by
        /// creation order (server slot index). Quantized order-preservingly into
        /// the canvas sort key; kept a float so gameplay can interpolate/derive it
        /// (e.g. a future y-sort writes priority = -y).
        LUX_MEMBER(display_name=Priority, tooltip=Draw priority; higher is drawn on top)
        float priority = 0.f;

        /// Skip submission when false (still a live entity; just not drawn this frame).
        LUX_MEMBER(display_name=Visible, tooltip=Whether the sprite is drawn)
        bool visible = true;
    };

} // namespace lux::gameplay::d2

#pragma once
// ============================================================================
//  SpriteAtlas2D.hpp — the gameplay-side atlas seam (A2-00).
//
//  An atlas frame is applied to a SpriteComponent by PLAIN FIELD COPY: the
//  component keeps its existing texture/uv_rect/pivot fields, the wire
//  protocol and the bridge are untouched, and the direct texture+uv authoring
//  path stays first-class. This header is the ONLY coupling point between the
//  atlas description and the 2D components (SpriteAnimationSystem reuses it).
// ============================================================================

#include <lux/engine/gameplay/2d/world/components/SpriteComponent.hpp>
#include <lux/engine/asset/Sprite2DAssets.hpp>   // rdesc types + assetIdFromOpaque

#include <string_view>

namespace lux::gameplay::d2
{
    /// Point @p sp at the atlas's texture and copy @p frame's uv/pivot into it.
    inline void applyAtlasFrame(SpriteComponent&                    sp,
                                const lux::rdesc::SpriteAtlas&      atlas,
                                const lux::rdesc::SpriteAtlasFrame& frame) noexcept
    {
        sp.texture = lux::asset::assetIdFromOpaque(atlas.texture_uuid);
        sp.uv_rect = frame.uv_rect;
        sp.pivot   = frame.pivot;
    }

    /// Name-resolved variant. False (component untouched) when the frame is
    /// absent — a missing frame is a content error the caller reports, never
    /// a silent identity-uv fallback.
    inline bool applyAtlasFrame(SpriteComponent&               sp,
                                const lux::rdesc::SpriteAtlas& atlas,
                                std::string_view               frame_name) noexcept
    {
        const auto* f = atlas.findFrame(frame_name);
        if (f == nullptr) return false;
        applyAtlasFrame(sp, atlas, *f);
        return true;
    }

} // namespace lux::gameplay::d2

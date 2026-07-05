#pragma once
// ============================================================================
//  Canvas2DResources.hpp — the scene-registry-owned state of Canvas2DFeature.
//
//  Held via sceneRegistry().ensure<Canvas2DResources>() (the LightResources /
//  PointCloud feature-owned-resource pattern), so its lifetime is the scene's:
//  torn down at scene teardown, released whether or not the feature detaches
//  first. This is what satisfies R2-01's "attach/remove 资源完整释放".
//
//  R2-01 scope: it holds ONLY the per-frame draw SNAPSHOT ingest (submit ops
//  append here; the feature drains it each frame). The heavier RETAINED producer
//  data (tilemap chunk buffers, pixel-field chunk textures behind the R2-00 owner
//  handles) lands with those producers' slices — NOT pre-built here (YAGNI). The
//  sprite MVP path is transient (rebuilt per frame), so its "retained" store is
//  just this pending list.
// ============================================================================

#include <lux/engine/render/renderer/features/canvas2d/Canvas2DOperation.hpp>  // SpriteDraw

#include <span>
#include <vector>

namespace lux::render
{
    struct Canvas2DResources
    {
        /// Per-frame sprite draws, appended by the submit handler on the render
        /// thread and drained by the feature at frame start. Sorting (R2-03) and the
        /// GPU upload + draw (R2-02) consume this; R2-01 only owns the ingest.
        std::vector<SpriteDraw> pending_sprites;

        /// Append a submitted batch (copies out of the frame blob — no borrowed
        /// pointer is retained past the call).
        void ingestSprites(std::span<const SpriteDraw> batch)
        {
            pending_sprites.insert(pending_sprites.end(), batch.begin(), batch.end());
        }

        /// Hand off this frame's accumulated sprites, leaving the ingest empty
        /// (swap, not move — guarantees the source is cleared, not merely valid).
        [[nodiscard]] std::vector<SpriteDraw> takeSprites() noexcept
        {
            std::vector<SpriteDraw> out;
            out.swap(pending_sprites);
            return out;
        }
    };

} // namespace lux::render

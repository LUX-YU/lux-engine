#pragma once
// ============================================================================
//  Canvas2DSort.hpp — R2-03: the painter-order sort + adjacent batch coalescing
//  that turns a scramble of submitted draws into a deterministic draw sequence.
//
//  Pure, device-free logic (unit-tested headless): the Canvas2DFeature calls it
//  each frame before expanding + uploading. Two rules (design §3.3):
//    1. Sort the WHOLE draw list by the full DrawOrderKey (stable) → painter order.
//       This is what expresses `tile-bg < pixel-field < tile-fg < sprite < pixel-fg`,
//       which coarse per-feature z-buckets cannot.
//    2. Coalesce ONLY sorted-ADJACENT, state-compatible draws into a batch. Because
//       merging is adjacency-limited, a batch can NEVER span a draw of a different
//       layer/texture that sorts between two otherwise-mergeable draws — so same-texture
//       sprites straddling a pixel-field/tilemap layer are NOT merged across it.
//
//  MVP producer = sprites only; "state-compatible" = same bindless texture (the sole
//  per-draw pipeline state that varies). Tile / pixel-field producers extend the
//  compatibility key with their own state when their slices land.
// ============================================================================

#include <lux/engine/render/renderer/features/canvas2d/Canvas2DOperation.hpp>  // SpriteDraw, DrawOrderKey

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace lux::render
{
    /// A run of sorted-adjacent, state-compatible sprites → one draw call.
    struct Canvas2DBatch
    {
        std::uint32_t first{0};             ///< index of the first sprite (post-sort)
        std::uint32_t count{0};             ///< number of sprites in the run
        std::uint32_t texture_bindless{0};  ///< the shared state that defines the batch
    };

    /// Stable-sort @p draws into painter order by full DrawOrderKey, then group
    /// sorted-adjacent same-texture runs into batches. Mutates @p draws (in place)
    /// and returns the batch ranges (over the SORTED order).
    [[nodiscard]] inline std::vector<Canvas2DBatch> sortAndBatch(std::span<SpriteDraw> draws)
    {
        std::stable_sort(draws.begin(), draws.end(),
                         [](const SpriteDraw& a, const SpriteDraw& b) { return a.key < b.key; });

        std::vector<Canvas2DBatch> batches;
        for (std::uint32_t i = 0; i < draws.size(); )
        {
            const std::uint32_t tex = draws[i].texture_bindless;
            std::uint32_t j = i + 1;
            // Extend the run only while adjacent AND state-compatible. Any draw with a
            // different texture (e.g. a different layer's producer that sorts here) ends
            // the batch — the next same-texture draw after it starts a NEW batch, so
            // nothing is ever merged ACROSS an intervening incompatible draw.
            while (j < draws.size() && draws[j].texture_bindless == tex)
                ++j;
            batches.push_back(Canvas2DBatch{i, j - i, tex});
            i = j;
        }
        return batches;
    }

} // namespace lux::render

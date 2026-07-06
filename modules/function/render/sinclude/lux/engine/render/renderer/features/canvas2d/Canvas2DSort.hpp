#pragma once
// ============================================================================
//  Canvas2DSort.hpp — the painter-order sort that turns a scramble of submitted
//  draws into a deterministic draw sequence.
//
//  Pure, device-free logic (unit-tested headless): the Canvas2DFeature calls it
//  each frame before expanding + uploading. One rule (design §3.3): stable-sort the
//  whole draw list by the full DrawOrderKey → painter order. This expresses
//  `tile-bg < pixel-field < tile-fg < sprite < pixel-fg`, which coarse per-feature
//  z-buckets cannot.
//
//  There is intentionally NO batch coalescing here: sprites are drawn in one
//  instanced/indexed pass, and per-sprite texture selection is bindless (the index
//  travels with the vertex), so a change of texture does NOT require a new draw call.
//  Real render-run batching only becomes meaningful once heterogeneous producers with
//  DIFFERENT pipelines/state exist (Tilemap / PixelField) — it belongs with them, not
//  as speculative scaffolding here.
// ============================================================================

#include <lux/engine/render/renderer/features/canvas2d/Canvas2DOperation.hpp>  // SpriteDraw, DrawOrderKey

#include <algorithm>
#include <span>

namespace lux::render
{
    /// Stable-sort @p draws in place into painter order by the full DrawOrderKey.
    inline void sortDraws(std::span<SpriteDraw> draws) noexcept
    {
        std::stable_sort(draws.begin(), draws.end(),
                         [](const SpriteDraw& a, const SpriteDraw& b) { return a.key < b.key; });
    }

} // namespace lux::render

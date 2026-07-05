// ============================================================================
//  batch_does_not_cross_layer_test.cpp — R2-03 (cpu): batch coalescing merges ONLY
//  sorted-adjacent, state-compatible draws. Two same-texture sprites separated (in
//  painter order) by a different-texture draw of another layer must NOT be merged
//  into one batch — that would let a sprite hop over the intervening layer (design
//  §3.3: "严禁跨层重排 / 禁止跨 PixelField/Tilemap 层抽取同纹理 Sprite 合批").
//
//  Self-checking: 0 = PASS, 1 = FAIL.
// ============================================================================

#include <lux/engine/render/renderer/features/canvas2d/Canvas2DSort.hpp>

#include <cstdio>
#include <vector>

using namespace lux::render;

namespace
{
    int g_failures = 0;
    void check(bool c, const char* m) { std::printf("[%s] %s\n", c ? " ok " : "FAIL", m); if (!c) ++g_failures; }

    SpriteDraw make(std::int16_t layer, std::uint64_t stable, std::uint32_t tex)
    {
        SpriteDraw s{};
        s.key = DrawOrderKey{layer, 0, 0, 0, stable};
        s.texture_bindless = tex;
        return s;
    }
}

int main()
{
    std::printf("=== batch_does_not_cross_layer_test (R2-03) ===\n");

    constexpr std::uint32_t TEX_A = 0xA;
    constexpr std::uint32_t TEX_B = 0xB;

    // Layer 0: texture A. Layer 1: texture B. Layer 2: texture A again. After the sort
    // the order is A(0), B(1), A(2) — the two A's are NOT adjacent, so they must be
    // TWO separate batches, never merged across the B layer.
    {
        std::vector<SpriteDraw> draws = {
            make(2, 20, TEX_A),   // submitted scrambled
            make(0, 10, TEX_A),
            make(1, 15, TEX_B),
        };
        const auto batches = sortAndBatch(draws);
        check(batches.size() == 3, "same-texture sprites straddling another layer stay 3 separate batches");
        if (batches.size() == 3)   // guard indexing so a wrong count FAILS cleanly, not OOB
        {
            check(batches[0].texture_bindless == TEX_A && batches[0].count == 1, "batch 0 = A (layer 0), count 1");
            check(batches[1].texture_bindless == TEX_B && batches[1].count == 1, "batch 1 = B (layer 1), count 1");
            check(batches[2].texture_bindless == TEX_A && batches[2].count == 1, "batch 2 = A (layer 2), count 1 (NOT merged with batch 0)");
        }
    }

    // Positive control: sorted-adjacent same-texture draws DO coalesce into one batch.
    {
        std::vector<SpriteDraw> draws = {
            make(0, 3, TEX_A),
            make(0, 1, TEX_A),
            make(0, 2, TEX_A),
        };
        const auto batches = sortAndBatch(draws);
        check(batches.size() == 1, "three adjacent same-texture sprites coalesce into ONE batch");
        if (batches.size() == 1)
            check(batches[0].count == 3, "the merged batch covers all 3 sprites");
    }

    // A run then a switch then a run: A,A,B,B,A → 3 batches (2,2,1), proving adjacency
    // (not global grouping) drives coalescing.
    {
        std::vector<SpriteDraw> draws = {
            make(0, 1, TEX_A), make(1, 2, TEX_A),
            make(2, 3, TEX_B), make(3, 4, TEX_B),
            make(4, 5, TEX_A),
        };
        const auto batches = sortAndBatch(draws);
        check(batches.size() == 3, "A,A,B,B,A → 3 runs (adjacency, not global texture grouping)");
        if (batches.size() == 3)
            check(batches[0].count == 2 && batches[1].count == 2 && batches[2].count == 1,
                  "run sizes are 2,2,1");
    }

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}

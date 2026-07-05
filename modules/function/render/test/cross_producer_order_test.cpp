// ============================================================================
//  cross_producer_order_test.cpp — R2-03 (cpu): sortAndBatch puts a SCRAMBLE of
//  draws from different producers/layers into a single deterministic painter order
//  (the full DrawOrderKey order), so cross-producer interleave "just works".
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

    SpriteDraw make(std::int16_t layer, std::uint32_t producer, std::uint64_t stable, std::uint32_t tex = 0)
    {
        SpriteDraw s{};
        s.key = DrawOrderKey{layer, 0, 0, producer, stable};
        s.texture_bindless = tex;
        return s;
    }
    bool ascending(std::span<const SpriteDraw> v)
    {
        for (std::size_t i = 1; i < v.size(); ++i)
            if (v[i].key < v[i - 1].key) return false;   // strictly non-decreasing
        return true;
    }
}

int main()
{
    std::printf("=== cross_producer_order_test (R2-03) ===\n");

    // The motivating interleave (design §3.3): three producers, layered
    // tile-bg < pixel-field < tile-fg < sprite < pixel-fg. Producer id in DrawOrderKey.
    const SpriteDraw tile_bg     = make(0, /*producer=*/1, 100);
    const SpriteDraw pixel_field = make(1, /*producer=*/3, 200);
    const SpriteDraw tile_fg     = make(2, /*producer=*/1, 101);
    const SpriteDraw sprite      = make(3, /*producer=*/2, 300);
    const SpriteDraw pixel_fg    = make(4, /*producer=*/3, 201);

    // Submitted in a scrambled, producer-clustered order (as three bridges would).
    std::vector<SpriteDraw> draws = { sprite, tile_bg, pixel_fg, tile_fg, pixel_field };
    (void)sortAndBatch(draws);   // this test cares about the sort side-effect, not the batches

    check(ascending(draws), "sorted list is in non-decreasing DrawOrderKey order");
    check(draws[0].key == tile_bg.key,     "layer 0 tile-bg first");
    check(draws[1].key == pixel_field.key, "layer 1 pixel-field second");
    check(draws[2].key == tile_fg.key,     "layer 2 tile-fg third");
    check(draws[3].key == sprite.key,      "layer 3 sprite fourth");
    check(draws[4].key == pixel_fg.key,    "layer 4 pixel-fg last");

    // Within one layer, order/producer/stable break ties deterministically.
    std::vector<SpriteDraw> same_layer = {
        make(5, /*producer=*/9, 3),
        make(5, /*producer=*/2, 7),
        make(5, /*producer=*/2, 1),
    };
    (void)sortAndBatch(same_layer);
    check(ascending(same_layer), "same-layer draws ordered by producer then stable_id");
    check(same_layer[0].key.producer_order == 2 && same_layer[0].key.stable_id == 1,
          "lowest (producer=2, stable=1) sorts first within the layer");
    check(same_layer[2].key.producer_order == 9, "highest producer sorts last within the layer");

    // Stability: equal keys keep submission order (stable_sort). Two draws with the SAME
    // full key retain their relative order (distinguished here by texture tag a/b).
    std::vector<SpriteDraw> tie = { make(6, 0, 1, /*tex=*/0xAA), make(6, 0, 1, /*tex=*/0xBB) };
    (void)sortAndBatch(tie);
    check(tie[0].texture_bindless == 0xAA && tie[1].texture_bindless == 0xBB,
          "equal keys preserve submission order (stable sort)");

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}

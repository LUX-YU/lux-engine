// ============================================================================
//  draw_order_comparator_test.cpp — R2-00: DrawOrderKey::operator< is a STRICT TOTAL
//  ORDER (design §3.3), so the Canvas2DFeature can express arbitrary cross-producer
//  layering with ONE stable sort — something coarse ERenderStage z-buckets cannot do.
//
//  Verifies the three strict-weak-order axioms plus totality by brute force over a set
//  of keys that varies every field independently, then pins the concrete motivating
//  example: tile-bg < pixel-field < tile-fg < sprite < pixel-fg.
//
//  Self-checking: 0 = PASS, 1 = FAIL.
// ============================================================================

#include <lux/engine/render/renderer/features/canvas2d/Canvas2DOperation.hpp>

#include <algorithm>
#include <cstdio>
#include <random>
#include <vector>

using lux::render::DrawOrderKey;

namespace
{
    int g_failures = 0;
    void check(bool c, const char* m) { std::printf("[%s] %s\n", c ? " ok " : "FAIL", m); if (!c) ++g_failures; }

    // A pool of keys that varies EACH field independently (including equal-in-higher-
    // fields pairs) so the axiom sweep actually exercises every tie-break level.
    std::vector<DrawOrderKey> makeKeys()
    {
        std::vector<DrawOrderKey> ks;
        const std::int16_t  layers[]    = {-2, 0, 5};
        const std::uint16_t sublayers[] = {0, 7};
        const std::int32_t  orders[]    = {-100, 0, 42};
        const std::uint32_t producers[] = {0u, 3u};
        const std::uint64_t stables[]   = {1ull, 9999ull};
        for (auto l : layers)
            for (auto sl : sublayers)
                for (auto o : orders)
                    for (auto pr : producers)
                        for (auto st : stables)
                            ks.push_back(DrawOrderKey{l, sl, o, pr, st});
        return ks;
    }
}

int main()
{
    std::printf("=== draw_order_comparator_test (R2-00) ===\n");

    const auto keys = makeKeys();

    // ── Axiom sweep over all ordered pairs / triples ──
    bool irreflexive = true, asymmetric = true, total = true, eq_consistent = true;
    for (const auto& a : keys)
    {
        if (a < a) irreflexive = false;                      // irreflexivity: !(a < a)
        for (const auto& b : keys)
        {
            const bool ab = a < b, ba = b < a, eq = (a == b);
            if (ab && ba) asymmetric = false;                // asymmetry: not both
            // totality/trichotomy: exactly one of a<b, b<a, a==b
            const int trues = int(ab) + int(ba) + int(eq);
            if (trues != 1) total = false;
            // == agrees with the order (equal iff neither strictly precedes)
            if (eq != (!ab && !ba)) eq_consistent = false;
        }
    }
    check(irreflexive,   "irreflexive: !(a < a) for every key");
    check(asymmetric,    "asymmetric: never both a < b and b < a");
    check(total,         "total: exactly one of a<b, b<a, a==b holds (trichotomy)");
    check(eq_consistent, "operator== agrees with the order (equal iff incomparable-by-<)");

    bool transitive = true;
    for (const auto& a : keys)
        for (const auto& b : keys)
            for (const auto& c : keys)
                if ((a < b) && (b < c) && !(a < c)) transitive = false;
    check(transitive, "transitive: a<b and b<c ⇒ a<c");

    // ── Each field is a tie-break at its own priority level ──
    const DrawOrderKey base{0, 0, 0, 0, 0};
    check(base < DrawOrderKey{1,0,0,0,0},  "layer dominates");
    check(base < DrawOrderKey{0,1,0,0,0},  "sublayer breaks a layer tie");
    check(base < DrawOrderKey{0,0,1,0,0},  "order breaks a sublayer tie");
    check(base < DrawOrderKey{0,0,0,1,0},  "producer_order breaks an order tie");
    check(base < DrawOrderKey{0,0,0,0,1},  "stable_id is the final tie-break");
    // A higher-priority field wins even when every lower field disagrees.
    check(DrawOrderKey{1,0,0,0,0} < DrawOrderKey{2,99,-999,0,0}, "layer outranks all lower fields");
    check(DrawOrderKey{0,0,5,9,9} < DrawOrderKey{0,1,0,0,0},     "sublayer outranks order/producer/stable");

    // ── The motivating example: cross-producer interleave in ONE sort ──
    //  A background tile layer, a pixel field on top of it, a foreground tile layer, a
    //  sprite above that, then a foreground pixel effect — three DIFFERENT producers
    //  interleaved by layer. Coarse per-feature z-buckets can't express this; a single
    //  key sort can.
    const DrawOrderKey tile_bg    {0, 0, 0, /*producer*/1, 100};
    const DrawOrderKey pixel_field{1, 0, 0, /*producer*/3, 200};
    const DrawOrderKey tile_fg    {2, 0, 0, /*producer*/1, 101};
    const DrawOrderKey sprite     {3, 0, 0, /*producer*/2, 300};
    const DrawOrderKey pixel_fg   {4, 0, 0, /*producer*/3, 201};

    std::vector<DrawOrderKey> scrambled = { sprite, tile_bg, pixel_fg, tile_fg, pixel_field };
    std::sort(scrambled.begin(), scrambled.end());     // uses operator<
    const std::vector<DrawOrderKey> expected = { tile_bg, pixel_field, tile_fg, sprite, pixel_fg };
    check(scrambled == expected, "stable sort yields tile-bg < pixel-field < tile-fg < sprite < pixel-fg");

    // A well-formed frame has NO duplicate keys — the stable_id tie-break guarantees a
    // deterministic order even when layer..producer_order all collide.
    const DrawOrderKey twinA{5, 5, 5, 5, 41};
    const DrawOrderKey twinB{5, 5, 5, 5, 42};
    check((twinA < twinB) && !(twinB < twinA) && !(twinA == twinB),
          "stable_id deterministically separates otherwise-identical keys");

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}

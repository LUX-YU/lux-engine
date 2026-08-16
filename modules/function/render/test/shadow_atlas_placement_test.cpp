// ============================================================================
//  shadow_atlas_placement_test.cpp — PERMANENT (cpu tier): locks the shadow
//  atlas placement invariant.
//
//      a caster's atlas tile is a function of its identity and of which casters
//      are present, NEVER of the order they were scored in
//
//  Why this test exists: the atlas packer is a shelf allocator, so a tile's
//  position is literally "the Nth allocation of this size". Feeding it casters
//  in score order therefore makes every light's tile a function of how far the
//  camera happens to be from it. An orbiting camera over equal-parameter lights
//  then permutes the whole assignment several times a second, and each
//  permutation has to be picked up in the SAME frame by the caster pass, the
//  EVSM blur and the lighting lookup — whoever lags renders another light's
//  tile. On screen that is regions losing their shadows for a few milliseconds
//  at a time.
//
//  That bug produced NO validation error and NO synchronization hazard: it is a
//  data-consistency defect, not a memory race. Nothing in the toolchain would
//  have caught a regression, hence this test.
//
//  No Vulkan, no device — orderCastersForPlacement and ShadowAtlasPacker are
//  pure CPU code in ShadowSliceMath.hpp.
// ============================================================================

#include <lux/engine/render/renderer/features/shadow/ShadowSliceMath.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <numeric>
#include <random>
#include <vector>

using namespace lux::render;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                            \
        }                                                                        \
    } while (0)

namespace
{
    constexpr uint32_t kPageRes    = 4096;
    constexpr uint32_t kPageCount  = 4;
    constexpr uint32_t kSliceBudget = 128;

    /// identity -> the tile it was placed in (layer/x/y/resolution).
    using Placement = std::map<uint64_t, ShadowTileAllocation>;

    /// Run the real pipeline: order, then allocate in that order, exactly the way
    /// ShadowMapFeature::buildSlicesForView does.
    Placement placeAll(std::vector<ShadowCasterRequest> requests)
    {
        ShadowAtlasPacker packer(kPageRes, kPageCount);
        const auto ordered = orderCastersForPlacement(std::move(requests), packer,
                                                      kSliceBudget, /*already_used=*/0);
        Placement out;
        for (const auto& r : ordered)
        {
            if (r.tile_count == 6u)
            {
                std::array<ShadowTileAllocation, 6> tiles{};
                if (!allocateCubeWithFallback(packer, r.resolution, tiles)) continue;
                out[r.identity] = tiles[0];          // base face pins the whole cube
            }
            else
            {
                ShadowTileAllocation tile{};
                if (!allocateTileWithFallback(packer, r.resolution, tile)) continue;
                out[r.identity] = tile;
            }
        }
        return out;
    }

    bool sameTile(const ShadowTileAllocation& a, const ShadowTileAllocation& b)
    {
        return a.layer == b.layer && a.x == b.x && a.y == b.y && a.resolution == b.resolution;
    }

    /// Six identical point lights on a ring — the deferred_stress_test shape that
    /// exposed the bug. Scores are what an orbiting camera produces.
    std::vector<ShadowCasterRequest> makeRingOfPointLights(uint32_t count, uint32_t resolution)
    {
        std::vector<ShadowCasterRequest> v;
        v.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
            v.push_back({/*identity=*/(1ull << 32) | i, /*score=*/0.0f, /*tile_count=*/6u, resolution});
        return v;
    }
}

int main()
{
    std::setbuf(stdout, nullptr);
    std::puts("=== shadow_atlas_placement_test ===");

    // ── 1. Placement is invariant under score permutation ──────────────────
    //
    // Same six lights, 200 different score orderings (what an orbiting camera
    // walks through). Every one must produce the identical identity -> tile map.
    {
        auto base = makeRingOfPointLights(6, 2048);
        for (uint32_t i = 0; i < 6; ++i) base[i].score = 100.0f - static_cast<float>(i);
        const Placement reference = placeAll(base);
        CHECK(reference.size() == 6);

        std::mt19937 rng(12345);
        std::vector<float> scores(6);
        std::iota(scores.begin(), scores.end(), 1.0f);
        for (int trial = 0; trial < 200; ++trial)
        {
            std::shuffle(scores.begin(), scores.end(), rng);
            auto shuffled = makeRingOfPointLights(6, 2048);
            for (uint32_t i = 0; i < 6; ++i) shuffled[i].score = scores[i];

            const Placement got = placeAll(shuffled);
            CHECK(got.size() == reference.size());
            for (const auto& [identity, tile] : reference)
            {
                const auto it = got.find(identity);
                CHECK(it != got.end());
                if (!sameTile(it->second, tile))
                {
                    std::fprintf(stderr,
                        "FAIL trial %d: light %llu moved (%u,%u,%u,res %u) -> (%u,%u,%u,res %u)\n",
                        trial, static_cast<unsigned long long>(identity),
                        tile.layer, tile.x, tile.y, tile.resolution,
                        it->second.layer, it->second.x, it->second.y, it->second.resolution);
                    return 1;
                }
            }
        }
        std::puts("  [ok] placement invariant under 200 score permutations");
    }

    // ── 2. Score still decides MEMBERSHIP when the atlas is full ───────────
    //
    // The canonicalisation must not cost the near-light preference: with more
    // casters than fit, the survivors are the high scorers — not the low ids.
    {
        // 2048-px cubes: one page holds 4 tiles, so 4 pages hold 24 = four cubes.
        // Ask for eight and give the LAST ones the highest scores.
        const uint32_t kCount = 8;
        auto requests = makeRingOfPointLights(kCount, 2048);
        for (uint32_t i = 0; i < kCount; ++i)
            requests[i].score = static_cast<float>(i);        // id 7 scores highest

        ShadowAtlasPacker packer(kPageRes, kPageCount);
        const auto ordered = orderCastersForPlacement(requests, packer, kSliceBudget, 0);
        CHECK(!ordered.empty());
        CHECK(ordered.size() < kCount);                        // genuinely under pressure

        // Survivors must be a suffix of the id range (the top scorers), and the
        // returned order must be ascending by identity (placement order).
        for (size_t i = 1; i < ordered.size(); ++i)
            CHECK(ordered[i - 1].identity < ordered[i].identity);

        float lowest_surviving_score = ordered.front().score;
        for (const auto& r : ordered)
            lowest_surviving_score = std::min(lowest_surviving_score, r.score);
        for (const auto& r : requests)
        {
            const bool survived = std::any_of(ordered.begin(), ordered.end(),
                [&](const ShadowCasterRequest& s) { return s.identity == r.identity; });
            if (!survived)
                CHECK(r.score <= lowest_surviving_score);      // only lower scorers dropped
        }
        std::printf("  [ok] %zu/%u survived a full atlas, and they are the top scorers\n",
                    ordered.size(), kCount);
    }

    // ── 3. Spot and point identities never collide ─────────────────────────
    //
    // Identity packs (kind << 32) | slot. A spot and a point light in the SAME
    // slot are different casters and must both get a tile.
    {
        std::vector<ShadowCasterRequest> mixed = {
            {/*spot  slot 0*/ (0ull << 32) | 0u, 10.0f, 1u, 1024},
            {/*point slot 0*/ (1ull << 32) | 0u,  5.0f, 6u, 1024},
        };
        const Placement got = placeAll(mixed);
        CHECK(got.size() == 2);
        CHECK(!sameTile(got.at((0ull << 32) | 0u), got.at((1ull << 32) | 0u)));
        std::puts("  [ok] spot/point identities are disjoint");
    }

    // ── 4. A caster that leaves frees its space, deterministically ─────────
    //
    // Removing a member legitimately shifts everyone after it (a shelf allocator
    // packs in sequence). What must hold is that the outcome depends only on the
    // remaining SET — re-running with the same set reproduces it exactly.
    {
        auto five = makeRingOfPointLights(6, 2048);
        five.erase(five.begin() + 2);                          // light 2 drops out
        for (uint32_t i = 0; i < five.size(); ++i) five[i].score = static_cast<float>(i);
        const Placement a = placeAll(five);

        std::reverse(five.begin(), five.end());                 // same set, other order
        const Placement b = placeAll(five);
        CHECK(a.size() == b.size());
        for (const auto& [identity, tile] : a)
            CHECK(sameTile(b.at(identity), tile));
        std::puts("  [ok] placement depends on the member set, not its order");
    }

    // ── 5. The returned set ALWAYS survives the real allocation ────────────
    //
    // Shelf packing is order-dependent: the membership probe walks in score
    // order, the caller allocates in identity order. Before the verify pass a
    // set the probe accepted could fail for real — a silently dropped tile.
    // Contract under test: allocating the returned requests IN THE RETURNED
    // ORDER against a fresh packer never fails. Fuzzed sizes/scores hunt the
    // fragmentation shapes a hand-written case would miss.
    {
        std::mt19937 rng(777);
        std::uniform_int_distribution<uint32_t> res_pick(0, 2);
        std::uniform_int_distribution<int>      kind_pick(0, 1);
        std::uniform_real_distribution<float>   score_pick(0.1f, 100.0f);
        constexpr uint32_t kResChoices[3] = {1024, 2048, 4096};

        for (int trial = 0; trial < 500; ++trial)
        {
            std::vector<ShadowCasterRequest> reqs;
            const uint32_t n = 4 + (rng() % 24);
            reqs.reserve(n);
            for (uint32_t i = 0; i < n; ++i)
            {
                const bool spot = kind_pick(rng) == 0;
                reqs.push_back({(static_cast<uint64_t>(spot ? 0 : 1) << 32) | i,
                                score_pick(rng), spot ? 1u : 6u,
                                kResChoices[res_pick(rng)]});
            }
            ShadowAtlasPacker packer(kPageRes, kPageCount);
            const auto ordered = orderCastersForPlacement(reqs, packer, kSliceBudget, 0);
            uint32_t used = 0;
            for (const auto& r : ordered)
            {
                bool fit;
                if (r.tile_count == 6u)
                {
                    std::array<ShadowTileAllocation, 6> t{};
                    fit = allocateCubeWithFallback(packer, r.resolution, t);
                }
                else
                {
                    ShadowTileAllocation t{};
                    fit = allocateTileWithFallback(packer, r.resolution, t);
                }
                if (!fit)
                {
                    std::fprintf(stderr,
                        "FAIL trial %d: ordered set failed the REAL allocation "
                        "(identity %llu, res %u)\n", trial,
                        static_cast<unsigned long long>(r.identity), r.resolution);
                    return 1;
                }
                used += std::max(r.tile_count, 1u);
            }
            CHECK(used <= kSliceBudget);
        }
        std::puts("  [ok] 500 fuzz trials: the returned set always allocates for real");
    }

    // ── 6. Sticky hysteresis: last build's holders resist marginal newcomers ─
    //
    // The churn driver on a saturated atlas: an edge light's score (a continuous
    // function of camera distance) crosses a rival's, membership flips, and the
    // whole table renumbers. With the sticky boost a newcomer must beat the
    // holder by kStickyScoreBoost; within the band the set must not change.
    {
        // Saturated shape (same as case 2): more cubes than fit, genuine
        // competition. Derive holder/loser from the FIRST build's actual result
        // — fallback downgrades make hand-computed capacities unreliable.
        const uint32_t kCount = 8;
        auto first = makeRingOfPointLights(kCount, 2048);
        for (uint32_t i = 0; i < kCount; ++i)
            first[i].score = static_cast<float>(i + 1);         // 1..8
        ShadowAtlasPacker packer(kPageRes, kPageCount);
        const auto gen1 = orderCastersForPlacement(first, packer, kSliceBudget, 0);
        CHECK(!gen1.empty());
        CHECK(gen1.size() < kCount);                            // under real pressure

        std::vector<uint64_t> sticky;
        for (const auto& r : gen1) sticky.push_back(r.identity);
        std::sort(sticky.begin(), sticky.end());
        const auto isHolder = [&](uint64_t id)
        { return std::binary_search(sticky.begin(), sticky.end(), id); };

        // Weakest holder + strongest loser, by the ORIGINAL scores.
        size_t weak_holder = SIZE_MAX, strong_loser = SIZE_MAX;
        for (size_t i = 0; i < first.size(); ++i)
        {
            if (isHolder(first[i].identity))
            { if (weak_holder == SIZE_MAX || first[i].score < first[weak_holder].score) weak_holder = i; }
            else
            { if (strong_loser == SIZE_MAX || first[i].score > first[strong_loser].score) strong_loser = i; }
        }
        CHECK(weak_holder != SIZE_MAX && strong_loser != SIZE_MAX);

        // In-band flip: the loser edges past the weakest holder on raw score,
        // but stays under holder_score * kStickyScoreBoost — the set must hold.
        auto second = first;
        second[strong_loser].score =
            second[weak_holder].score * (kStickyScoreBoost - 0.1f);
        CHECK(second[strong_loser].score > second[weak_holder].score);
        const auto gen2 = orderCastersForPlacement(second, packer, kSliceBudget, 0, sticky);
        CHECK(gen2.size() == gen1.size());
        for (size_t i = 0; i < gen2.size(); ++i)
            CHECK(gen2[i].identity == gen1[i].identity);

        // Out-of-band: the newcomer clears the band — it MUST get in
        // (hysteresis is a damper, not permanent tenure).
        auto third = first;
        third[strong_loser].score =
            third[weak_holder].score * kStickyScoreBoost + 1.0f;
        const auto gen3 = orderCastersForPlacement(third, packer, kSliceBudget, 0, sticky);
        const bool newcomer_in = std::any_of(gen3.begin(), gen3.end(),
            [&](const ShadowCasterRequest& r) { return r.identity == third[strong_loser].identity; });
        CHECK(newcomer_in);
        if (gen3.size() == gen1.size())   // seats conserved -> someone made way
            CHECK(std::none_of(gen3.begin(), gen3.end(),
                [&](const ShadowCasterRequest& r)
                { return r.identity == third[weak_holder].identity; }));
        std::puts("  [ok] sticky hysteresis: in-band flips damped, out-of-band flips honoured");
    }

    std::puts("shadow_atlas_placement_test PASSED");
    return 0;
}

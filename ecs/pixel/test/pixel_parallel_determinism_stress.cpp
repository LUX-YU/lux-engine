// ============================================================================
//  pixel_parallel_determinism_stress.cpp — PERMANENT stress test (I2-02).
//
//  The one promise parallel chunk stepping makes: the CA is a pure function of
//  (cells, step index, schedule) — NEVER of the thread count. This runs the
//  same seeded multi-chunk world under 1/2/4/8 worker threads and demands
//  bit-identical determinism hashes at every checkpoint. It stays in-tree
//  permanently (test policy 2026-07-06): thread-schedule races are exactly the
//  class of bug that regresses silently under later rule changes, and the
//  four-colour write-reach invariant (static_assert in PixelFieldRuntime.cpp)
//  is only as good as this empirical check.
//
//  World: 768×480 (3×2 chunks of 256²) — chunk seams in both axes; terrain
//  shelves + sand columns + water pools arranged to keep matter flowing ACROSS
//  seams for hundreds of steps; mid-run StampRect commands keep waking it.
// ============================================================================

#include <lux/engine/ecs/pixel/systems/PixelFieldRuntime.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <vector>

using namespace lux::ecs;

namespace
{
    constexpr std::int32_t kOriginX = -256;
    constexpr std::int32_t kOriginY = -256;
    constexpr int kSteps      = 600;    ///< total CA steps per run
    constexpr int kCheckEvery = 50;     ///< hash checkpoint cadence

    struct Run
    {
        std::vector<std::uint64_t> hashes;
        double                     ms{0.0};
        std::uint64_t              warm_scratch_growth{0u};
        std::uint64_t              final_scratch_growth{0u};
    };

    /// Deterministic content seeding through the public command API only.
    void seed(PixelFieldRuntime& rt, PixelFieldHandle h,
              MaterialId stone, MaterialId sand, MaterialId water)
    {
        auto stamp = [&](int x, int y, int w, int hh, MaterialId m)
        {
            PixelFieldCommand c{};
            c.field = h;
            c.minimum = {kOriginX + x, kOriginY + y};
            c.extent = {
                static_cast<std::uint32_t>(w),
                static_cast<std::uint32_t>(hh)};
            c.material = m;
            rt.enqueue(c);
        };
        // Staggered stone shelves crossing the x=256/512 and y=256 seams.
        for (int i = 0; i < 6; ++i)
            stamp(40 + i * 120, 60 + (i % 3) * 90, 180, 6, stone);
        // Tall sand columns above the shelves (drain across seams as they topple).
        for (int i = 0; i < 10; ++i)
            stamp(70 + i * 68, 300, 22, 140, sand);
        // Water pools poured high on both sides of the x seams.
        stamp(200, 420, 140, 40, water);
        stamp(460, 420, 140, 40, water);
        rt.applyCommands();
    }

    Run runWorld(std::uint32_t threads)
    {
        PixelFieldRuntime rt{
            PixelFieldRuntimeConfig{.parallelism = threads}};
        const MaterialId stone = rt.materials().add({EMaterialPhase::SOLID, 255, 0xFF888888u});
        const MaterialId sand  = rt.materials().add({EMaterialPhase::POWDER, 200, 0xFFC2B280u});
        const MaterialId water = rt.materials().add({EMaterialPhase::LIQUID, 100, 0xFF3060C0u});

        const auto h = rt.create({
            lux::ecs::PixelFieldId{uuids::uuid::from_string(
                "70000000-0000-4000-8000-000000000001").value()},
            EPixelFieldExtent::BOUNDED,
            {{-1, -1}, {1, 0}},
            0u});
        for (std::int64_t y = -1; y <= 0; ++y)
            for (std::int64_t x = -1; x <= 1; ++x)
                if (!rt.loadChunk(
                        h,
                        PixelChunkLoad{
                            {x, y},
                            std::vector<MaterialId>(
                                PixelFieldRuntime::kChunkCellCount,
                                kEmptyMaterial),
                            {},
                            {},
                            {},
                            0u,
                            {},
                            true,
                            true}))
                    std::abort();
        seed(rt, h, stone, sand, water);

        Run out;
        const auto t0 = std::chrono::steady_clock::now();
        for (int s = 1; s <= kSteps; ++s)
        {
            // Periodic mid-run edits (deterministic schedule) so activity never
            // fully settles and the command path is in the loop too.
            if (s % 97 == 0)
            {
                PixelFieldCommand c{};
                c.field = h;
                c.minimum = {
                    kOriginX + 30 + (s * 7) % 600,
                    kOriginY + 8};
                c.extent = {24u, 4u};
                c.material = kEmptyMaterial;   // punch drain holes near the floor
                rt.enqueue(c);
            }
            rt.applyCommands();
            rt.step();
            if (s == 1)
                out.warm_scratch_growth = rt.scratchGrowthCount();
            if (s % kCheckEvery == 0)
                out.hashes.push_back(rt.determinismHash(h));
        }
        out.final_scratch_growth = rt.scratchGrowthCount();
        out.ms = std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - t0).count();
        return out;
    }
} // namespace

int main()
{
    const std::uint32_t thread_counts[] = {1u, 2u, 4u, 8u};

    Run baseline{};
    int failures = 0;
    for (std::uint32_t n : thread_counts)
    {
        const Run r = runWorld(n);
        std::printf("threads=%u  steps=%d  %.1f ms  (%.3f ms/step)  final=%016llx\n",
                    n, kSteps, r.ms, r.ms / kSteps,
                    static_cast<unsigned long long>(r.hashes.back()));
        if (r.final_scratch_growth != r.warm_scratch_growth)
        {
            std::printf(
                "FAIL threads=%u: WorkItem scratch grew after warm-up "
                "(%llu -> %llu)\n",
                n,
                static_cast<unsigned long long>(r.warm_scratch_growth),
                static_cast<unsigned long long>(r.final_scratch_growth));
            ++failures;
        }
        if (n == 1u) { baseline = r; continue; }
        if (r.hashes.size() != baseline.hashes.size())
        {
            std::printf("FAIL threads=%u: checkpoint count mismatch\n", n);
            ++failures;
            continue;
        }
        for (std::size_t i = 0; i < r.hashes.size(); ++i)
            if (r.hashes[i] != baseline.hashes[i])
            {
                std::printf("FAIL threads=%u: hash diverges at step %d "
                            "(%016llx != %016llx)\n",
                            n, static_cast<int>((i + 1) * kCheckEvery),
                            static_cast<unsigned long long>(r.hashes[i]),
                            static_cast<unsigned long long>(baseline.hashes[i]));
                ++failures;
                break;
            }
    }

    if (failures) { std::printf("%d FAILURES\n", failures); return 1; }
    std::puts("pixel_parallel_determinism_stress: 1 == N for N in {2,4,8} — ALL OK");
    return 0;
}

/// @file world_streaming_test.cpp
/// Headless CPU test for WorldStreamingSystem (大世界① 增量2 W2a). Plain
/// assert() — no GPU, no render module, no external test framework.
///
/// Covers:
///   - pure cell math: cellCoord floor incl. negatives; cellMinDist2 (empty →
///     0, single + multi source min).
///   - functional gating: near entity stays active (no RenderDormantComponent),
///     far entity goes dormant; moving the camera flips them.
///   - monotonicity: a fixed grid of entities + a camera flying away yields a
///     monotonically non-increasing active count, reaching 0 (mirrors the W1
///     render-side stress 384→0).
///   - hysteresis: the SAME camera position inside the [load,unload] band
///     yields active-when-approached-from-active vs dormant-when-approached-
///     from-dormant — proving the band keeps state and won't thrash.
///   - disabled / empty-sources → everything active, every tag cleared.

#include <lux/engine/gameplay/3d/world/systems/WorldStreamingSystem.hpp>
#include <lux/engine/gameplay/3d/world/components/WorldTransformComponent.hpp>
#include <lux/engine/gameplay/3d/world/components/MeshComponent.hpp>
#include <lux/engine/gameplay/3d/world/components/RenderDormantComponent.hpp>

#include <lux/engine/meta/LuxObject.hpp>   // EntityRegistry / entt

#include <Eigen/Core>

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <vector>

using namespace lux::gameplay;   // registry / entity helpers
using namespace lux::gameplay::d3;      // WorldStreamingSystem + streaming/mesh/transform components
using lux::meta::EntityRegistry;

namespace
{
    using Source = std::array<float, 3>;

    // Place an entity on the GROUND plane (engine is Y-up → streaming uses the
    // X-Z plane: world (0,3)/(2,3); Y/(1,3)=height is spanned). The second arg
    // is the world Z. (This MUST match the system's X-Z cell math — testing in
    // X-Y is what hid the "loads as a strip down the view axis" bug.)
    entt::entity makeMeshAt(EntityRegistry& reg, float x, float z)
    {
        const auto e = reg.create();
        auto& wt = reg.emplace<WorldTransformComponent>(e);
        wt.world = Eigen::Matrix4f::Identity();
        wt.world(0, 3) = x;
        wt.world(1, 3) = 0.f;   // height — spanned, irrelevant to streaming
        wt.world(2, 3) = z;
        reg.emplace<MeshComponent>(e);
        return e;
    }

    std::span<const Source> one(const Source& s)
    {
        return std::span<const Source>(&s, 1);
    }
}

int main()
{
    std::printf("=== WorldStreamingSystem test ===\n");

    // ---- 1. pure cell math --------------------------------------------------
    {
        int32_t cx = 0, cy = 0;
        WorldStreamingSystem::cellCoord(0.f, 0.f, 100.f, cx, cy);
        assert(cx == 0 && cy == 0);
        WorldStreamingSystem::cellCoord(150.f, 250.f, 100.f, cx, cy);
        assert(cx == 1 && cy == 2);
        // floor must round toward -inf, not toward zero.
        WorldStreamingSystem::cellCoord(-1.f, -100.f, 100.f, cx, cy);
        assert(cx == -1 && cy == -1);
        WorldStreamingSystem::cellCoord(-101.f, -201.f, 100.f, cx, cy);
        assert(cx == -2 && cy == -3);

        // cellMinDist2: empty → 0.
        assert(WorldStreamingSystem::cellMinDist2(0, 0, 100.f, std::span<const Source>{}) == 0.f);
        // cell (0,0) center = (50,50); single source at origin → 50^2+50^2 = 5000.
        const Source s0{0.f, 0.f, 0.f};
        assert(std::abs(WorldStreamingSystem::cellMinDist2(0, 0, 100.f, one(s0)) - 5000.f) < 1.f);
        // two sources → min; second sits on the cell center (X=50,Z=50) → ~0.
        const Source srcs2[]{{0.f, 0.f, 0.f}, {50.f, 0.f, 50.f}};
        assert(WorldStreamingSystem::cellMinDist2(0, 0, 100.f, std::span<const Source>(srcs2, 2)) < 1.f);
        std::printf("  [OK] cell math (cellCoord floor incl. negatives, cellMinDist2)\n");
    }

    // ---- 2. functional gating + flip ---------------------------------------
    {
        EntityRegistry reg;
        WorldStreamingSystem ws;
        WorldStreamingSystem::Params p;
        p.cell_size = 100.f; p.load_range = 250.f; p.unload_range = 400.f; p.enabled = true;
        ws.setParams(p);

        const auto near_e = makeMeshAt(reg, 0.f, 0.f);
        const auto far_e  = makeMeshAt(reg, 1000.f, 0.f);

        ws.update(reg, one(Source{0.f, 0.f, 0.f}));   // camera at origin
        assert(!reg.all_of<RenderDormantComponent>(near_e) && "near active");
        assert(reg.all_of<RenderDormantComponent>(far_e)   && "far dormant");
        assert(ws.activeCount() == 1 && ws.dormantCount() == 1 && ws.totalCount() == 2);

        ws.update(reg, one(Source{1000.f, 0.f, 0.f}));  // camera flies to the far one
        assert(reg.all_of<RenderDormantComponent>(near_e)  && "near now dormant");
        assert(!reg.all_of<RenderDormantComponent>(far_e)  && "far now active");
        assert(ws.activeCount() == 1 && ws.dormantCount() == 1);
        std::printf("  [OK] near/far gating + camera-move flip\n");
    }

    // ---- 3. monotonic active count as camera recedes (W1-style 384→0) ------
    {
        EntityRegistry reg;
        WorldStreamingSystem ws;
        WorldStreamingSystem::Params p;
        p.cell_size = 100.f; p.load_range = 300.f; p.unload_range = 350.f; p.enabled = true;
        ws.setParams(p);

        for (int i = -10; i <= 10; ++i)
            for (int j = -10; j <= 10; ++j)
                makeMeshAt(reg, static_cast<float>(i) * 100.f, static_cast<float>(j) * 100.f);
        const uint32_t total = 21u * 21u;

        uint32_t prev = std::numeric_limits<uint32_t>::max();
        for (const float cam_x : {0.f, 500.f, 1000.f, 2000.f, 5000.f, 20000.f})
        {
            ws.update(reg, one(Source{cam_x, 0.f, 0.f}));
            assert(ws.totalCount() == total);
            assert(ws.activeCount() <= prev && "active count monotonically non-increasing as camera recedes");
            prev = ws.activeCount();
        }
        assert(prev == 0 && "all entities dormant once camera is far past the whole grid");
        std::printf("  [OK] monotonic active-count decay to 0 (grid %u entities)\n", total);
    }

    // ---- 4. hysteresis: same band position, different result by history -----
    {
        EntityRegistry reg;
        WorldStreamingSystem ws;
        WorldStreamingSystem::Params p;
        // cell 100 → entity (0,0) cell center (50,50). load 200 / unload 400.
        // band d2 ∈ (40000,160000). camera (350,0): d2=(50-350)^2+50^2=92500 → in band.
        p.cell_size = 100.f; p.load_range = 200.f; p.unload_range = 400.f; p.enabled = true;
        ws.setParams(p);
        const auto e = makeMeshAt(reg, 0.f, 0.f);

        ws.update(reg, one(Source{50.f, 0.f, 0.f}));    // d2=2500 < load2 → wake
        assert(!reg.all_of<RenderDormantComponent>(e) && "starts active");

        ws.update(reg, one(Source{350.f, 0.f, 0.f}));   // in band, prev active → stays active
        assert(!reg.all_of<RenderDormantComponent>(e) && "in-band from active side stays ACTIVE");

        ws.update(reg, one(Source{1000.f, 0.f, 0.f}));  // d2 huge > unload2 → sleep
        assert(reg.all_of<RenderDormantComponent>(e) && "goes dormant beyond unload");

        ws.update(reg, one(Source{350.f, 0.f, 0.f}));   // SAME band position, prev dormant → stays dormant
        assert(reg.all_of<RenderDormantComponent>(e) && "in-band from dormant side stays DORMANT");
        std::printf("  [OK] hysteresis: identical band position yields active-vs-dormant by history\n");
    }

    // ---- 5. disabled / empty sources → all active, tags cleared ------------
    {
        EntityRegistry reg;
        WorldStreamingSystem ws;
        WorldStreamingSystem::Params p;
        p.cell_size = 100.f; p.load_range = 50.f; p.unload_range = 60.f; p.enabled = true;
        ws.setParams(p);
        const auto a = makeMeshAt(reg, 0.f, 0.f);
        const auto b = makeMeshAt(reg, 10000.f, 0.f);

        ws.update(reg, one(Source{0.f, 0.f, 0.f}));     // b far → dormant under tiny range
        assert(reg.all_of<RenderDormantComponent>(b) && "b dormant while enabled+near-range");

        ws.setEnabled(false);
        ws.update(reg, one(Source{0.f, 0.f, 0.f}));     // disabled → wipe every tag
        assert(!reg.all_of<RenderDormantComponent>(a) && !reg.all_of<RenderDormantComponent>(b)
               && "disabled clears all dormant tags");
        assert(ws.dormantCount() == 0 && ws.activeCount() == 2);

        ws.setEnabled(true);
        ws.update(reg, std::span<const Source>{});      // empty sources → all active
        assert(!reg.all_of<RenderDormantComponent>(a) && !reg.all_of<RenderDormantComponent>(b)
               && "empty source set keeps everything active");
        assert(ws.dormantCount() == 0 && ws.activeCount() == 2);
        std::printf("  [OK] disabled / empty-sources → all active, tags cleared\n");
    }

    std::printf("=== PASS ===\n");
    return 0;
}

// ============================================================================
//  spatial_cull_grid_stress_test.cpp — W1 coarse-cull numerical verification (headless GPU)
//
//  Proves the coarse-cull PAYOFF end-to-end on a real GPU mask buffer:
//    - bring up a minimal headless render DeviceContext (VMA),
//    - lay out a large grid of instances (one per cell, ~25k),
//    - run SpatialCullGrid::update for a camera that recedes from the grid,
//    - assert the active-instance count is NON-INCREASING and falls to 0 as the
//      camera flies away (= "activity drops as the world streams out"), and
//    - read BACK the GPU active-mask buffer and assert dormant instances are 0,
//      the instance under the camera is 1, and the 1-count == activeInstanceCount.
//
//  Uses the InstanceResources-free SpatialCullGrid::update overload (direct
//  InstanceXY feed), so no full instance storage / descriptor arena is needed.
//
//  Self-checking: 0 = PASS (or skip if no Vulkan device), 1 = FAIL.
// ============================================================================

#include <lux/engine/render/scene/SpatialCullGrid.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <memory>
#include <vector>

using namespace lux::render;

int main()
{
    std::setbuf(stdout, nullptr);
    std::printf("=== spatial_cull_grid_stress_test ===\n");

    // ── Minimal headless render device (VMA allocator only; no window/surface) ──
    std::unique_ptr<InstanceContext> inst;
    try { inst = std::make_unique<InstanceContext>(std::vector<const char*>{}, nullptr); }
    catch (...) { std::printf("No Vulkan instance. Skipping.\n"); return 0; }

    DeviceContext dev{*inst};
    if (auto r = dev.init(EPhysicalDeviceSelectionPolicy::INTEGRATED_GPU_PREFERRED); !r)
    {
        std::printf("No Vulkan device. Skipping.\n");
        return 0;
    }

    int fails = 0;
    auto check = [&](bool cond, const char* name)
    { std::printf(cond ? "  [PASS] %s\n" : "  [FAIL] %s\n", name); if (!cond) ++fails; };

    // ── Build the world partition + a large instance grid ──────────────────
    constexpr uint32_t G    = 160;          // 160x160 = 25600 instances (~25k)
    constexpr uint32_t kN   = G * G;
    constexpr float    kCell = 100.0f;
    constexpr float    kRange = 1000.0f;    // eff = kRange + kCell = 1100
    const float        kCenter = 0.5f * G * kCell;  // grid centre = 8000

    SpatialCullGrid wp;
    SpatialCullGrid::InitInfo wi{};
    wi.device_context   = &dev;
    wi.deferred_queue   = nullptr;          // headless: inline destroy on grow/shutdown
    wi.frames_in_flight = 2;
    wi.initial_capacity = 64;               // tiny → forces the mask ring to grow to kN
    wi.cell_size        = kCell;
    wi.cull_distance    = kRange;
    wp.init(wi);

    // One instance per cell, placed at the cell centre. slot = gx*G + gy.
    std::vector<SpatialCullGrid::InstanceXY> alive;
    alive.reserve(kN);
    for (uint32_t gx = 0; gx < G; ++gx)
        for (uint32_t gy = 0; gy < G; ++gy)
            alive.push_back(SpatialCullGrid::InstanceXY{
                gx * G + gy, gx * kCell + 0.5f * kCell, gy * kCell + 0.5f * kCell});

    auto runFrame = [&](uint64_t serial, float cam_z) -> uint32_t
    {
        // Streaming is on the ground X-Z plane (Y/height spanned). Keep the
        // camera at grid-centre X and fly along Z. cams = {x, y, z}.
        const std::array<std::array<float, 3>, 1> cams{{ {{kCenter, 0.0f, cam_z}} }};
        wp.update(serial, cams, alive, kN);
        return wp.activeInstanceCount();
    };

    // ── 1. Active count is non-increasing as the camera recedes (+Z) ───────
    // Camera starts at the grid centre, then flies straight out past the edge.
    const float ys[] = { kCenter, 14000.f, 16000.f, 17000.f, 19000.f, 1.0e6f };
    std::vector<uint32_t> actives;
    for (uint32_t i = 0; i < std::size(ys); ++i)
        actives.push_back(runFrame(1 + i, ys[i]));

    std::printf("  active by camera-Z:");
    for (uint32_t i = 0; i < actives.size(); ++i)
        std::printf(" %.0f->%u", ys[i], actives[i]);
    std::printf(" (total=%u)\n", kN);

    bool non_increasing = true;
    for (uint32_t i = 1; i < actives.size(); ++i)
        if (actives[i] > actives[i - 1]) non_increasing = false;
    check(non_increasing, "active count is non-increasing as camera recedes");
    check(actives.front() > 0 && actives.front() < kN,
          "camera inside grid -> partial active (coarse cull engaged, not all/none)");
    check(actives.back() == 0, "camera far beyond grid -> 0 active (whole world dormant)");
    check(actives.front() > actives.back(), "active drops from inside to far");
    // Somewhere between centre and far the count must strictly fall (not flat).
    check(actives[2] < actives[0] || actives[3] < actives[0] || actives[4] < actives[0],
          "active strictly drops as the camera nears/exits the edge");

    // ── 2. Read BACK the GPU mask buffer for a camera at the grid centre ───
    const uint32_t active_centre = runFrame(100, kCenter);
    const uint32_t* mask = wp.gpuMaskMappedForTest();
    check(mask != nullptr, "GPU mask readback is non-null");
    if (mask != nullptr)
    {
        uint32_t ones = 0;
        for (uint32_t i = 0; i < kN; ++i)
            if (mask[i] == 1u) ++ones;
        check(ones == active_centre, "GPU mask 1-count == activeInstanceCount (upload matches stats)");

        // Far corner (gx=0,gy=0) → (50,50), ~11.2k from centre >> 1100 → dormant.
        check(mask[0] == 0u, "far-corner instance is masked dormant (0)");
        // Cell under the camera (gx=80,gy=80) → (8050,8050), ~70 from centre → active.
        check(mask[80u * G + 80u] == 1u, "instance under the camera is active (1)");
    }

    wp.shutdown();
    // dev / inst torn down by their destructors (device idle — no GPU work submitted).

    std::printf("=== spatial_cull_grid_stress_test %s (fails=%d) ===\n",
                fails == 0 ? "PASSED" : "FAILED", fails);
    return fails == 0 ? 0 : 1;
}

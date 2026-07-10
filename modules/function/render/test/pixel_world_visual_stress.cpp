// ============================================================================
//  pixel_world_visual_stress.cpp — VISUAL (interactive tier): the ENTIRE 2D
//  stack alive in one window, as busy as we can make it. The full-stack sibling
//  of canvas2d_visual_stress (sprites-only) — this one drives the REAL gameplay
//  world: ECS + fixed-step CA + all three bridges over the real wire.
//
//  What is on screen / what it proves:
//    - a 384×240-cell PIXEL WORLD (Noita-style): stone terrain with a water
//      bowl and platforms; SAND rains from the sky and piles up; WATER drips
//      into the bowl (semi-transparent premultiplied — the sprites BELOW show
//      through both water and every empty cell);
//    - every ~10 s a DIG carves a hole under a platform → the piles collapse
//      (erase commands + sleep/wake cascades);
//    - ~200 dim background sprites BELOW the field + a deep "sun", a 512-sprite
//      swirl ring ABOVE it, all on ONE priority axis (two kinds, run-switched
//      per frame);
//    - a band of background sprites POPS through the field every ~2.5 s
//      (priority flip across the field's depth → cross-kind order rebuild);
//    - the CAMERA breathes (pan + zoom) through the real Camera2D entity;
//    - console: fps + CA/upload stats once per second.
//
//  NOT self-checking — for eyeballing. `interactive` tier. Exit 0 without Vulkan.
//
//  SOAK MODE (X2-02): `--soak <seconds>` runs unattended for that long and then
//  exits 0, printing a stats line every 10 s (fps, CA numbers, uploadedRevision,
//  dropped events) — the Slice D "no unbounded growth" evidence run.
// ============================================================================

#include "DeviceRenderFixture.hpp"

#include <lux/engine/render/renderer/features/canvas2d/Canvas2DFeatureOps.hpp>
#include <lux/engine/render/renderer/features/view_camera/ViewCameraOperation.hpp>
#include <lux/engine/render/comm/server/FeatureRegistry.hpp>

#include <lux/engine/gameplay/2d/Scene2D.hpp>
#include <lux/engine/gameplay/2d/pixel/PixelFieldRuntime.hpp>
#include <lux/engine/gameplay/2d/pixel/PixelField2DComponent.hpp>
#include <lux/engine/gameplay/2d/world/components/Transform2DComponent.hpp>
#include <lux/engine/gameplay/2d/world/components/SpriteComponent.hpp>
#include <lux/engine/gameplay/2d/world/components/Camera2DComponent.hpp>
#include <lux/engine/gameplay/world/World.hpp>
#include <lux/engine/gameplay/render_bridge/RenderableSystem.hpp>
#include <lux/engine/asset/AssetManager.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

using namespace lux::render;
namespace d2 = lux::gameplay::d2;

namespace
{
    struct EmptyConfig {};
    constexpr std::uint32_t W = 1280, H = 800;
    // C2-00: a CROSS-CHUNK world (768×480 = 3×2 chunks of 256) — sand rains,
    // piles and avalanches straight across the chunk borders on screen, the
    // permanent visual evidence for the chunked runtime. Aspect stays 1.6.
    constexpr std::uint32_t CW = 768, CH = 480;
    constexpr int SC = 2;   // terrain layout scale vs the original 384×240 layout
    constexpr float kPi = 3.14159265f;

    /// Deterministic tiny LCG (rain placement).
    struct Lcg
    {
        std::uint32_t s{0x12345678u};
        std::uint32_t next() { s = s * 1664525u + 1013904223u; return s >> 8; }
        float unit() { return static_cast<float>(next() & 0xFFFF) / 65535.f; }
    };

    std::uint32_t rainbow(float h01)
    {
        const float h = (h01 - std::floor(h01)) * 6.f;
        const float x = 1.f - std::fabs(std::fmod(h, 2.f) - 1.f);
        float r = 0, g = 0, b = 0;
        switch (static_cast<int>(h))
        {
        case 0: r = 1; g = x; break;  case 1: r = x; g = 1; break;
        case 2: g = 1; b = x; break;  case 3: g = x; b = 1; break;
        case 4: r = x; b = 1; break;  default: r = 1; b = x; break;
        }
        const auto u = [](float v) { return static_cast<std::uint32_t>(v * 255.f + 0.5f); };
        return 0xFF000000u | (u(b) << 16) | (u(g) << 8) | u(r);
    }
}

int main(int argc, char** argv)
{
    std::setbuf(stdout, nullptr);
    float soak_seconds = 0.f;   // 0 = interactive (loop until the window closes)
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string_view(argv[i]) == "--soak")
            soak_seconds = std::strtof(argv[i + 1], nullptr);
    std::printf("=== pixel_world_visual_stress ===  (a living pixel world; %s)\n",
                soak_seconds > 0.f ? "soak mode — exits by itself" : "close the window to exit");

    lux::rendertest::DeviceRenderFixture fx(W, H, "Pixel World Visual Stress — CA field + sprites, one depth axis");
    if (!fx.ok()) { std::printf("No Vulkan device. Skipping.\n"); return 0; }

    const auto sv = fx.makeSceneWithSwapchainView("PixelWorld", "main");

    const auto cam_reg = fx.await(fx.session().registerFeatureType(kStandardViewCameraFeatureFactory));
    fx.await(fx.session().addFeature(sv.scene_id, cam_reg.feature_type_id, EmptyConfig{}));
    const auto canvas_reg = fx.await(fx.session().registerFeatureType(kCanvas2DFeatureFactory));
    fx.await(fx.session().addFeature(sv.scene_id, canvas_reg.feature_type_id, EmptyConfig{}));

    FeatureRegistry features;
    features.injectForTest("StandardViewCamera",
                           std::span<const TypeId>{cam_reg.ops, cam_reg.op_count});
    features.injectForTest("Canvas2D",
                           std::span<const TypeId>{canvas_reg.ops, canvas_reg.op_count});

    // ── gameplay world ──
    d2::PixelFieldRuntime runtime;
    const auto stone = runtime.materials().add({d2::EMaterialPhase::Solid,  255, 0xFF6A6A6Au});   // grey
    const auto sand  = runtime.materials().add({d2::EMaterialPhase::Powder, 200, 0xFF29C5F0u});   // golden
    const auto water = runtime.materials().add({d2::EMaterialPhase::Liquid, 100, 0xC0AE6024u});   // 75% premul blue

    lux::gameplay::World world;
    auto plan = d2::traditional2DPlan();
    plan.enablePixelSimulation();
    const auto installed = d2::install(world, &runtime, plan);
    if (installed.simulation == nullptr) { std::printf("install failed\n"); return 1; }

    // Camera: origin-centred, 1 world unit tall (field spans 1.6×1.0), breathing.
    const auto cam = world.createEntity();
    world.emplace<d2::Transform2DComponent>(cam);
    auto& cc = world.emplace<d2::Camera2DComponent>(cam);
    cc.units_per_view_height = 1.05f;
    cc.aspect = static_cast<float>(W) / static_cast<float>(H);
    cc.y_flip = true;
    world.emplace<d2::ActiveCamera2DTag>(cam);

    // The field: min corner at (-0.8, -0.5).
    const auto field_e = world.createEntity();
    world.emplace<d2::Transform2DComponent>(field_e).position = Eigen::Vector2f(-0.8f, -0.5f);
    auto& fcomp = world.emplace<d2::PixelField2DComponent>(field_e);
    fcomp.field     = runtime.create({CW, CH, 0});
    fcomp.cell_size = 1.f / CH;
    fcomp.priority  = 0.f;

    // Terrain: floor, a water bowl (left), three staggered platforms (right).
    const auto stampRect = [&](int x, int y, int w, int h, d2::MaterialId m)
    {
        d2::PixelFieldCommand c{};
        c.field = fcomp.field; c.min = {x, y}; c.size = {w, h}; c.material = m;
        runtime.enqueue(c);
    };
    stampRect(0, 0, CW, 8*SC, stone);         // floor
    stampRect(16*SC, 8*SC, 6*SC, 70*SC, stone);   // bowl left wall
    stampRect(150*SC, 8*SC, 6*SC, 70*SC, stone);  // bowl right wall
    stampRect(40*SC, 40*SC, 60*SC, 5*SC, water);  // dropped water settles into the bowl
    stampRect(200*SC, 70*SC, 70*SC, 6*SC, stone); // platform 1 (straddles the x=512 border)
    stampRect(290*SC, 120*SC, 70*SC, 6*SC, stone);// platform 2
    stampRect(230*SC, 170*SC, 70*SC, 6*SC, stone);// platform 3
    stampRect(210*SC, 90*SC, 30*SC, 20*SC, sand); // starter pile on platform 1

    // ── sprites, all on the ONE priority axis the field shares ──
    Lcg rng;
    // deep background "sun" (priority -8) + ~200 dim stars (priority -5).
    {
        const auto sun = world.createEntity();
        world.emplace<d2::Transform2DComponent>(sun).position = Eigen::Vector2f(0.45f, 0.28f);
        auto& sp = world.emplace<d2::SpriteComponent>(sun);
        sp.size = Eigen::Vector2f(0.28f, 0.28f);
        sp.tint = 0xFF20D0FFu;   // warm
        sp.priority = -8.f;
    }
    std::vector<lux::meta::entity_id> stars;
    stars.reserve(200);
    for (int i = 0; i < 200; ++i)
    {
        const auto e = world.createEntity();
        world.emplace<d2::Transform2DComponent>(e).position =
            Eigen::Vector2f(rng.unit() * 1.6f - 0.8f, rng.unit() * 1.0f - 0.5f);
        auto& sp = world.emplace<d2::SpriteComponent>(e);
        const float s = 0.004f + 0.008f * rng.unit();
        sp.size = Eigen::Vector2f(s, s);
        sp.tint = rainbow(rng.unit());
        sp.priority = -5.f;
        stars.push_back(e);
    }
    // the swirl ring ABOVE the field (priority +10): 512 sprites, ECS-animated —
    // direct field writes each frame; the value-compare bridge turns exactly the
    // moved ones into ONE transform bulk.
    std::vector<lux::meta::entity_id> swirl;
    swirl.reserve(512);
    for (int i = 0; i < 512; ++i)
    {
        const auto e = world.createEntity();
        world.emplace<d2::Transform2DComponent>(e);
        auto& sp = world.emplace<d2::SpriteComponent>(e);
        sp.size = Eigen::Vector2f(0.012f, 0.012f);
        sp.tint = 0xFFFFFFFFu;
        sp.priority = 10.f;
        swirl.push_back(e);
    }

    lux::asset::AssetManager assets;
    lux::gameplay::RenderableSystem rs(fx.session(), assets, sv.scene_id, sv.view);
    rs.setFeatures(features);
    d2::registerBridges(rs, &runtime, plan);

    std::printf("world up: %u k cells simulating, %zu sprites on one depth axis. Close to exit.\n",
                CW * CH / 1000u, stars.size() + swirl.size() + 1);

    const auto t0  = std::chrono::steady_clock::now();
    auto fps_mark  = t0;
    auto last_time = t0;
    int  fps_frames = 0;
    int  frame_no   = 0;
    bool band_up    = false;
    std::vector<d2::PixelFieldEvent> events_scratch;

    // Soak evidence (X2-02): sampled every 10 s so growth trends are visible.
    auto  soak_mark = t0;
    std::uint64_t soak_last_rev = 0;

    while (fx.running())
    {
        const auto now = std::chrono::steady_clock::now();
        const float t  = std::chrono::duration<float>(now - t0).count();
        if (soak_seconds > 0.f)
        {
            if (t >= soak_seconds)
            {
                std::printf("soak done: %.0f s, %d frames, eventsDropped=%llu — exiting 0\n",
                            t, frame_no, static_cast<unsigned long long>(runtime.eventsDropped()));
                return 0;
            }
            if (now - soak_mark >= std::chrono::seconds(10))
            {
                const auto rev = runtime.uploadedRevision(fcomp.field);
                std::printf("soak t=%.0fs | tiles=%u scanned=%u | upRev=%llu (+%llu) | evDropped=%llu\n",
                            t,
                            runtime.activeTiles(fcomp.field),
                            runtime.cellsScannedLastStep(fcomp.field),
                            static_cast<unsigned long long>(rev),
                            static_cast<unsigned long long>(rev - soak_last_rev),
                            static_cast<unsigned long long>(runtime.eventsDropped()));
                soak_last_rev = rev;
                soak_mark = now;
            }
        }
        float dt = std::chrono::duration<float>(now - last_time).count();
        last_time = now;
        if (dt > 0.1f) dt = 0.1f;   // window drag hiccup → clamp banked time

        // camera breathes: pan + zoom through the REAL camera entity.
        world.registry().get<d2::Transform2DComponent>(cam).position =
            Eigen::Vector2f(0.06f * std::sin(t * 0.21f), 0.04f * std::sin(t * 0.13f));
        cc.units_per_view_height = 1.05f + 0.12f * std::sin(t * 0.17f);

        // sand rain across the sky + water drip into the bowl (deterministic).
        if ((frame_no % 3) == 0)
        {
            const int x = 8 + static_cast<int>(rng.unit() * (CW - 16));
            stampRect(x, CH - 6, 3, 3, sand);
        }
        if ((frame_no % 7) == 0)
            stampRect(70*SC + static_cast<int>(rng.unit() * 30.f*SC), CH - 4, 2, 2, water);

        // every ~10 s: DIG platform 1 clean away (slab + the pile's bottom rows)
        // → everything on it avalanches to the floor; patch the slab back later.
        if ((frame_no % 600) == 300) stampRect(195*SC, 68*SC, 80*SC, 14*SC, d2::kEmptyMaterial);
        if ((frame_no % 600) == 599) stampRect(200*SC, 70*SC, 70*SC, 6*SC, stone);
        // every ~20 s: a big CRATER swallows the accumulated sand near the floor
        // (recycles the world so it never silts up; watch the piles pour in).
        if ((frame_no % 1200) == 900) stampRect(170*SC, 8*SC, 200*SC, 26*SC, d2::kEmptyMaterial);

        // every ~2.5 s: a band of stars pops THROUGH the field (cross-kind order flip).
        if ((frame_no % 150) == 0)
        {
            band_up = !band_up;
            const float prio = band_up ? 12.f : -5.f;
            for (std::size_t i = 0; i < stars.size(); i += 4)
                world.registry().get<d2::SpriteComponent>(stars[i]).priority = prio;
        }

        // the swirl orbits the whole world (direct ECS field writes).
        for (std::size_t i = 0; i < swirl.size(); ++i)
        {
            const float a = t * 0.8f + static_cast<float>(i) * (2.f * kPi / swirl.size());
            const float r = 0.34f + 0.10f * std::sin(t * 0.6f + i * 0.03f);
            world.registry().get<d2::Transform2DComponent>(swirl[i]).position =
                Eigen::Vector2f(r * 1.4f * std::cos(a), r * std::sin(a));
        }

        world.tick(dt);
        rs.update(world.registry(), dt);
        fx.flush();

        // Drain the fact stream (the F2-06 consumer contract; unbounded growth
        // was a real leak before the cap+drain fix).
        events_scratch.clear();
        runtime.drainEvents(events_scratch);

        ++frame_no;
        ++fps_frames;
        if (now - fps_mark >= std::chrono::seconds(1))
        {
            std::printf("fps=%d | tiles=%u scanned=%u moved=%u step=%.2fms | upRev=%llu | sprites=%zu\n",
                        fps_frames,
                        runtime.activeTiles(fcomp.field),
                        runtime.cellsScannedLastStep(fcomp.field),
                        runtime.movedCellsLastStep(fcomp.field),
                        runtime.stepMillisLast(fcomp.field),
                        static_cast<unsigned long long>(runtime.uploadedRevision(fcomp.field)),
                        stars.size() + swirl.size() + 1);
            fps_frames = 0;
            fps_mark   = now;
        }
    }
    return 0;
}

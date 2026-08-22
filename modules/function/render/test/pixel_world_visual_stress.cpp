// ============================================================================
//  pixel_world_visual_stress.cpp — VISUAL (interactive tier): the ENTIRE 2D
//  stack alive in one window, as busy as we can make it. The full-stack sibling
//  of canvas2d_visual_stress (images-only) — this one drives the REAL gameplay
//  world: ECS + fixed-step CA + all three bridges over the real wire.
//
//  What is on screen / what it proves:
//    - a 384×240-cell PIXEL WORLD (Noita-style): stone terrain with a water
//      bowl and platforms; SAND rains from the sky and piles up; WATER drips
//      into the bowl (semi-transparent premultiplied — the images BELOW show
//      through both water and every empty cell);
//    - every ~10 s a DIG carves a hole under a platform → the piles collapse
//      (erase commands + sleep/wake cascades);
//    - ~200 dim background images BELOW the field + a deep "sun", a 512-image
//      swirl ring ABOVE it, all on ONE priority axis (two kinds, run-switched
//      per frame);
//    - a band of background images POPS through the field every ~2.5 s
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
#include "VisualSoak.hpp"

#include <lux/engine/function/render/client/genops/Canvas2DOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ViewCameraOperation.ops.hpp>
#include <lux/engine/function/render/client/FeatureCatalog.hpp>

#include <lux/engine/ecs/render/RenderStage.hpp>
#include <lux/engine/ecs/render/systems/CameraViewSystem.hpp>
#include <lux/engine/ecs/render/subsystems/ResidencySubsystem.hpp>
#include <lux/engine/ecs/render/systems/RenderSystem.hpp>
#include <lux/engine/runtime/render/scene/ResidencyAssembly.hpp>
#include <lux/engine/runtime/render/scene/testing/AsyncTestServices.hpp>
#include <lux/engine/ecs/render/components/2d/Image2DComponent.hpp>
#include <lux/engine/ecs/integration/presentation2d/InstallPresentation2DSystems.hpp>
#include <lux/engine/ecs/physics/InstallSimulationSystems.hpp>
#include <lux/engine/ecs/transform/InstallTransformSystems.hpp>
#include <lux/engine/ecs/physics/systems/Simulation2DSystem.hpp>
#include <lux/engine/ecs/physics/FixedStepConfig.hpp>
#include <lux/engine/ecs/render/components/RenderViewBindingComponent.hpp>
#include <lux/engine/ecs/render/components/PrimaryCameraTag.hpp>
#include <lux/engine/ecs/pixel/systems/PixelFieldRuntime.hpp>
#include <lux/engine/ecs/pixel/components/PixelField2DComponent.hpp>
#include <lux/engine/ecs/pixel/components/PixelFieldBindingComponent.hpp>
#include <lux/engine/ecs/components/Transform2DComponent.hpp>
#include <lux/engine/ecs/render/components/2d/Image2DComponent.hpp>
#include <lux/engine/ecs/render/components/2d/Camera2DComponent.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/PersistentEntityIndex.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/meta/Meta.hpp>

#include <chrono>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <uuid.h>
#include <vector>

using namespace lux::render;
namespace d2 = lux::ecs;

namespace
{
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
    lux::rendertest::VisualSoak soak;
    if (!lux::rendertest::VisualSoak::parse(argc, argv, soak))
        return 2;
    std::printf("=== pixel_world_visual_stress ===  (a living pixel world; %s)\n",
                soak.enabled() ? "soak mode — exits by itself" : "close the window to exit");

    lux::rendertest::DeviceRenderFixture fx(W, H, "Pixel World Visual Stress — CA field + images, one depth axis");
    if (!fx.ok()) { std::printf("No Vulkan device. Skipping.\n"); return 0; }

    const auto sv = fx.makeSceneWithSwapchainView("PixelWorld", "main");

    const auto cam_reg = fx.awaitControl(fx.control().registerFeatureType(kViewCameraFeatureFactory));
    fx.awaitControl(fx.control().addFeature(sv.scene_id, cam_reg.feature_type_id, lux::render::ViewCameraCommTag{}));
    const auto canvas_reg = fx.awaitControl(fx.control().registerFeatureType(kCanvas2DFeatureFactory));
    fx.awaitControl(fx.control().addFeature(sv.scene_id, canvas_reg.feature_type_id, lux::render::Canvas2DCommConfig{}));

    FeatureCatalog features;
    features.injectForTest("StandardViewCamera",
                           std::span<const TypeId>{cam_reg.ops, cam_reg.op_count});
    features.injectForTest("Canvas2D",
                           std::span<const TypeId>{canvas_reg.ops, canvas_reg.op_count});

    // ── gameplay world ──
    d2::PixelFieldRuntime runtime;
    const auto stone = runtime.materials().add({d2::EMaterialPhase::SOLID,  255, 0xFF6A6A6Au});   // grey
    const auto sand  = runtime.materials().add({d2::EMaterialPhase::POWDER, 200, 0xFF29C5F0u});   // golden
    const auto water = runtime.materials().add({d2::EMaterialPhase::LIQUID, 100, 0xC0AE6024u});   // 75% premul blue

    lux::asset::AssetManager assets{
        lux::asset::runtimeAssetCodecCatalog()};
    // 驻留三件套(裁决二):声明在渲染绑定之前 —— 逆序析构。
    lux::runtime::testing::AsyncTestServices async(
        assets,
        fx.upload(),
        fx.sync(),
        lux::exec::AsyncRuntimeConfig{
            .blocking_io_threads = 1,
            .background_cpu_concurrency = 1}
    );
    if (!async.valid())
        return 1;
    lux::runtime::ResidencyAssembly residency(
        fx.control(),
        async.uploadClient(),
        assets,
        features,
        async.assetClient(),
        async.runtime(),
        {}
    );
    // Registry owner must outlive every observer that detaches from it.
    lux::ecs::World world;
    lux::ecs::PersistentEntityIndex persistent_entities{world.registry()};
    // 资产归驻留胶水:asset_id 经三件套解析成 GPU 句柄写进 cache 组件。
    // 驻留胶水是一个普通节点(批 B1)。本 demo 手工驱动(不走
    // Schedule::tick),所以 Schedule 自动做的三件事在这里手动做:
    // 连信号、每帧 drain、登记成服务供包的 resolve* 声明取用。
    auto residency_glue = std::make_unique<lux::ecs::ResidencySubsystem>(assets);
    residency_glue->setCallbacks(residency.makeCallbacks());
    auto* const residency_owner = residency_glue.get();
    std::vector<std::unique_ptr<lux::ecs::RenderStage>> render_stages;
    std::vector<std::string_view> render_feature_roots;

    // Reverse destruction is intentional: builder → schedule systems → owned
    // services → residency observers → World registry. Longer-lived borrowed
    // render/asset resources remain below that graph.
    lux::ecs::SceneServices   services;
    lux::ecs::Schedule        schedule{world};
    lux::ecs::ScheduleBuilder assembly{schedule, services};

    // Every assembly dependency enters the unpublished overlay. Resolver
    // declarations may mutate residency during plan commit, but cannot reach
    // a pre-existing live service.
    auto& staged = assembly.services();
    auto render_binding = staged.emplace<lux::ecs::SceneRenderBinding>(
        fx.session(),
        fx.control(),
        async.uploadClient(),
        sv.scene_id);
    auto active_view = staged.emplace<lux::ecs::ActiveRenderView>();
    if (!render_binding || !active_view)
    {
        std::printf("Failed to stage the scene render binding.\n");
        return 1;
    }
    (void)(*render_binding)->seal(features);
    if (!staged.adopt(runtime) ||
        !staged.adopt(persistent_entities))
    {
        std::printf("Failed to stage 2D scene services.\n");
        return 1;
    }

    // This executable is the host boundary for the linked component sidecars.
    // Drain their registrars before the pack inspects reflected component data.
    lux::ecs::ComponentTypeCatalog components;
    if (!lux::ecs::initializeGeneratedMetadata(components))
        return 1;

    if (!lux::ecs::installSpatial2DTransformSystems(
            assembly, components) ||
        !assembly.add(
            std::move(residency_glue), lux::ecs::kPhasePreRender) ||
        !lux::ecs::installSimulation2DSystems(
            assembly, components) ||
        !lux::ecs::installPresentation2DSystems(
            assembly, components) ||
        !lux::ecs::installPresentation2DRendering(
            assembly,
            components,
            render_stages,
            render_feature_roots,
            *residency_owner))
    {
        std::printf("2D system assembly failed\n");
        return 1;
    }
    auto render_system = std::make_unique<lux::ecs::RenderSystem>(
        **render_binding,
        **active_view,
        fx.control().adoptScene(sv.scene_id),
        std::move(render_stages));
    if (!assembly.add(std::move(render_system), lux::ecs::kPhaseRender) ||
        !assembly.add(
            std::make_unique<lux::ecs::CameraViewSystem>(
                **render_binding,
                **active_view),
            lux::ecs::kPhaseRender))
    {
        std::printf("RenderSystem installation failed\n");
        return 1;
    }
    if (const auto committed = assembly.commit(); !committed)
    {
        std::printf("schedule commit failed\n");
        return 1;
    }

    // 连信号:包的 resolve* 声明此刻已全部注册(Schedule 走 onAdded,
    // 手工驱动就在这里)。
    residency_owner->attach(world.registry());
    if (services.get<d2::Simulation2DSystem>() == nullptr)
    { std::printf("install failed\n"); return 1; }

    // Camera: origin-centred, 1 world unit tall (field spans 1.6×1.0), breathing.
    const auto cam = world.createEntity();
    world.emplace<d2::Transform2DComponent>(cam);
    auto& cc = world.emplace<d2::Camera2DComponent>(cam);
    cc.units_per_view_height = 1.05f;
    cc.aspect = static_cast<float>(W) / static_cast<float>(H);
    world.emplace<lux::ecs::PrimaryCameraTag>(cam);
    // camera → view wiring is DATA now (RenderViewBinding): this camera drives
    // the demo's swapchain view; unbound cameras are inert.
    world.emplace<lux::ecs::RenderViewBindingComponent>(cam,
        fx.control().adoptView(sv.scene_id, sv.view));

    // The field: min corner at (-0.8, -0.5).
    const auto field_e = world.createEntity();
    world.emplace<d2::Transform2DComponent>(field_e).position = {-0.8, -0.5};
    auto& fcomp = world.emplace<d2::PixelField2DComponent>(field_e);
    fcomp.definition = uuids::uuid::from_string(
        "89b15d75-4c61-45f8-8654-0d9a239df52b").value();
    const auto field = runtime.create({
        d2::PixelFieldId{fcomp.definition},
        d2::EPixelFieldExtent::BOUNDED,
        {{0, 0}, {
            static_cast<std::int64_t>((CW - 1u) >>
                d2::PixelFieldRuntime::kChunkShift),
            static_cast<std::int64_t>((CH - 1u) >>
                d2::PixelFieldRuntime::kChunkShift)}},
        0u});
    world.emplace<d2::PixelFieldBindingComponent>(
        field_e,
        d2::PixelFieldBindingComponent{field, false});
    for (std::int64_t chunk_y = 0;
         chunk_y <= static_cast<std::int64_t>((CH - 1u) >>
             d2::PixelFieldRuntime::kChunkShift);
         ++chunk_y)
    {
        for (std::int64_t chunk_x = 0;
             chunk_x <= static_cast<std::int64_t>((CW - 1u) >>
                 d2::PixelFieldRuntime::kChunkShift);
             ++chunk_x)
        {
            d2::PixelChunkLoad load;
            load.coordinate = {chunk_x, chunk_y};
            load.materials.assign(
                d2::PixelFieldRuntime::kChunkCellCount,
                d2::kEmptyMaterial);
            load.simulation_active = true;
            if (!runtime.loadChunk(field, std::move(load)))
                return 1;
        }
    }
    fcomp.cell_size = 1.f / CH;
    fcomp.draw_priority = 0;

    // Terrain: floor, a water bowl (left), three staggered platforms (right).
    const auto stampRect = [&](int x, int y, int w, int h, d2::MaterialId m)
    {
        d2::PixelFieldCommand c{};
        c.field = field;
        c.minimum = {x, y};
        c.extent = {
            static_cast<std::uint32_t>(w),
            static_cast<std::uint32_t>(h)};
        c.material = m;
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

    // ── images, all on the ONE priority axis the field shares ──
    Lcg rng;
    // deep background "sun" (priority -8) + ~200 dim stars (priority -5).
    {
        const auto sun = world.createEntity();
        world.emplace<d2::Transform2DComponent>(sun).position = {0.45, 0.28};
        auto& sp = world.emplace<d2::Image2DComponent>(sun);
        sp.size = Eigen::Vector2f(0.28f, 0.28f);
        sp.tint = 0xFF20D0FFu;   // warm
        sp.priority = -8.f;
    }
    std::vector<lux::ecs::Entity> stars;
    stars.reserve(200);
    for (int i = 0; i < 200; ++i)
    {
        const auto e = world.createEntity();
        world.emplace<d2::Transform2DComponent>(e).position = {
            static_cast<double>(rng.unit() * 1.6f - 0.8f),
            static_cast<double>(rng.unit() * 1.0f - 0.5f)};
        auto& sp = world.emplace<d2::Image2DComponent>(e);
        const float s = 0.004f + 0.008f * rng.unit();
        sp.size = Eigen::Vector2f(s, s);
        sp.tint = rainbow(rng.unit());
        sp.priority = -5.f;
        stars.push_back(e);
    }
    // the swirl ring ABOVE the field (priority +10): 512 images, ECS-animated —
    // direct field writes each frame; the value-compare bridge turns exactly the
    // moved ones into ONE transform bulk.
    std::vector<lux::ecs::Entity> swirl;
    swirl.reserve(512);
    for (int i = 0; i < 512; ++i)
    {
        const auto e = world.createEntity();
        world.emplace<d2::Transform2DComponent>(e);
        auto& sp = world.emplace<d2::Image2DComponent>(e);
        sp.size = Eigen::Vector2f(0.012f, 0.012f);
        sp.tint = 0xFFFFFFFFu;
        sp.priority = 10.f;
        swirl.push_back(e);
    }

    std::printf("world up: %u k cells simulating, %zu images on one depth axis. Close to exit.\n",
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
        if (soak.enabled())
        {
            if (now - soak_mark >= std::chrono::seconds(10))
            {
                const auto rev = runtime.uploadedRevision(field);
                std::printf("soak t=%.0fs | tiles=%u scanned=%u | upRev=%llu (+%llu) | evDropped=%llu\n",
                            t,
                            runtime.activeTiles(field),
                            runtime.cellsScannedLastStep(field),
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
        // Transform mutation must go through patch so observers see it.
        world.registry().patch<d2::Transform2DComponent>(cam, [&](auto& tc)
        {
            tc.position = {
                static_cast<double>(0.06f * std::sin(t * 0.21f)),
                static_cast<double>(0.04f * std::sin(t * 0.13f))};
        });
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
            // 改组件字段走 `patch` —— Image2D 的抽取是变更驱动的,裸
            // `get<T>().field = x` 不发 `on_update`,那次改动送不到 GPU
            // (症状:星带不翻层)。由抽取 oracle 抓出。
            for (std::size_t i = 0; i < stars.size(); i += 4)
                world.registry().patch<d2::Image2DComponent>(
                    stars[i], [prio](auto& sp) { sp.priority = prio; });
        }

        // the swirl orbits the whole world (direct ECS field writes).
        for (std::size_t i = 0; i < swirl.size(); ++i)
        {
            const float a = t * 0.8f + static_cast<float>(i) * (2.f * kPi / swirl.size());
            const float r = 0.34f + 0.10f * std::sin(t * 0.6f + i * 0.03f);
            world.registry().patch<d2::Transform2DComponent>(swirl[i], [&](auto& tc)
            {
                tc.position = {
                    static_cast<double>(r * 1.4f * std::cos(a)),
                    static_cast<double>(r * std::sin(a))};
            });
        }

        async.drainMainThreadCompletions();   // 驻留管道主线程会合
        schedule.tick(dt);
        residency_owner->drainResolvers(world.registry());
        fx.flush();

        // Drain the fact stream (the F2-06 consumer contract; unbounded growth
        // was a real leak before the cap+drain fix).
        events_scratch.clear();
        runtime.drainEvents(events_scratch);

        ++frame_no;
        ++fps_frames;
        const auto soak_now = std::chrono::steady_clock::now();
        if (soak.reached(t0, soak_now))
        {
            std::printf(
                "soak final: eventsDropped=%llu\n",
                static_cast<unsigned long long>(runtime.eventsDropped())
            );
            soak.reportGracefulTeardown(
                t0,
                soak_now,
                static_cast<std::uint64_t>(frame_no)
            );
            break;
        }
        if (now - fps_mark >= std::chrono::seconds(1))
        {
            std::printf("fps=%d | tiles=%u scanned=%u moved=%u step=%.2fms | upRev=%llu | images=%zu\n",
                        fps_frames,
                        runtime.activeTiles(field),
                        runtime.cellsScannedLastStep(field),
                        runtime.movedCellsLastStep(field),
                        runtime.stepMillisLast(field),
                        static_cast<unsigned long long>(runtime.uploadedRevision(field)),
                        stars.size() + swirl.size() + 1);
            fps_frames = 0;
            fps_mark   = now;
        }
    }
    schedule.requestClose();
    while (!schedule.closeState().complete)
    {
        if (!fx.control().waitAndPumpReplies())
            return 1;
        fx.pumpReplies();
        schedule.tick(0.0f);
    }
    const auto residency_close =
        lux::runtime::testing::detail::closeResidency(
            residency,
            async.runtime());
    if (!residency_close.clean())
        return 1;
    async.close();
    return 0;
}

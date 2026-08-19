// ============================================================================
//  tilemap_visual_stress.cpp — VISUAL (interactive tier): the A2-02 tilemap
//  path under load. One 256×128-tile map (32k tiles, ONE canvas instance +
//  ONE R16 index texture) drawn through the real Tilemap2DSubsystem over the real
//  wire, with a procedural 4×4 tileset atlas.
//
//  What is on screen / what it proves:
//    - a rolling-hills tile terrain (grass caps, dirt fill, stone depths,
//      scattered ore sparkle tiles) spanning 12.8×6.4 world units;
//    - the CAMERA auto-scrolls the full map width and breathes its zoom —
//      moving the camera is ONE view op, the 32k-tile map itself is zero
//      wire traffic while untouched (GPU-resident index texture);
//    - a "mining wave" sweeps across the map carving and re-filling tile
//      columns every frame → per-frame setTile edits become SMALL dirty-rect
//      region uploads (never the whole map);
//    - every ~15 s the whole terrain REGENERATES with a new seed (one fill →
//      one full-map upload, the worst case, visibly instant);
//    - images BELOW (parallax stars) and ABOVE (drifting clouds) the map on
//      the same priority axis — tile runs interleave with image runs.
//    - console: fps + map revision once per second.
//
//  NOT self-checking — for eyeballing. `interactive` tier. Exit 0 without Vulkan.
//  `--soak <seconds>` runs the same windowed workload unattended and then
//  enters the normal RAII teardown path.
// ============================================================================

#include "DeviceRenderFixture.hpp"
#include "VisualSoak.hpp"

#include <lux/engine/function/render/client/genops/Canvas2DOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ViewCameraOperation.ops.hpp>
#include <lux/engine/function/render/client/FeatureCatalog.hpp>

#include <lux/engine/ecs/render/RenderSystemBuilder.hpp>
#include <lux/engine/ecs/render/subsystems/CameraViewSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/ResidencySubsystem.hpp>
#include <lux/engine/ecs/render/systems/RenderSystem.hpp>
#include <lux/engine/runtime/render/scene/ResidencyAssembly.hpp>
#include <lux/engine/runtime/render/scene/testing/AsyncTestServices.hpp>
#include <lux/engine/ecs/render/components/2d/Image2DComponent.hpp>
#include <lux/engine/scene/SceneFeatureId.hpp>
#include <lux/engine/runtime/packs/spatial2d/Presentation2DContribution.hpp>
#include <lux/engine/runtime/packs/spatial2d/Simulation2DContribution.hpp>
#include <lux/engine/runtime/packs/spatial2d/Transform2DContribution.hpp>
#include <lux/engine/ecs/render/components/RenderViewBindingComponent.hpp>
#include <lux/engine/ecs/render/components/PrimaryCameraTag.hpp>
#include <lux/engine/ecs/components/Transform2DComponent.hpp>
#include <lux/engine/ecs/render/components/2d/Image2DComponent.hpp>
#include <lux/engine/ecs/tilemap/components/TilemapComponent.hpp>
#include <lux/engine/ecs/tilemap/components/TilemapBindingComponent.hpp>
#include <lux/engine/ecs/tilemap/systems/TilemapRuntime.hpp>
#include <lux/engine/ecs/render/components/2d/Camera2DComponent.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>

#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/TextureAsset.hpp>
#include <lux/engine/description/Texture.hpp>
#include <lux/engine/meta/Meta.hpp>

#include <uuid.h>

#include <chrono>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

using namespace lux::render;
namespace d2 = lux::ecs;

namespace
{
    constexpr std::uint32_t W = 1280, H = 800;
    constexpr std::uint32_t MAP_W = 256, MAP_H = 128;   // 32k tiles, one instance
    constexpr float kTile = 0.05f;                       // map = 12.8 × 6.4 world units

    struct Lcg
    {
        std::uint32_t s{0xC0FFEE01u};
        std::uint32_t next() { s = s * 1664525u + 1013904223u; return s >> 8; }
        float unit() { return static_cast<float>(next() & 0xFFFF) / 65535.f; }
    };

    // Tileset ordinals (row-major in a 4×4 atlas, row 0 = TOP of the image).
    enum : std::uint16_t
    {
        kGrass = 0, kDirt = 1, kStone = 2, kOre = 3,
        kBrick = 4, kSand2 = 5, kIce = 6, kLava = 7,
    };

    /// Procedural 4×4 tileset atlas: 16 tiles of 16×16 px, each a flat colour
    /// with a darker 1-px border + a diagonal accent so tile boundaries and
    /// orientation are readable on screen.
    lux::asset::asset_id_t registerTilesetTexture(lux::asset::AssetManager& mgr)
    {
        constexpr std::uint32_t TS = 16, COLS = 4, ROWS = 4;
        constexpr std::uint32_t TW = TS * COLS, TH = TS * ROWS;
        static constexpr std::uint32_t kBase[16] = {
            0xFF3FA34Du, 0xFF275A8Cu, 0xFF6A6A6Au, 0xFF29C5F0u,   // grass dirt stone ore
            0xFF4444AAu, 0xFF52C5E6u, 0xFFD0B080u, 0xFF2020D0u,   // brick sand ice lava
            0xFF808020u, 0xFF208080u, 0xFF802080u, 0xFF404040u,
            0xFFC0C0C0u, 0xFF6090C0u, 0xFF90C060u, 0xFFC06090u,
        };
        std::vector<std::byte> px(static_cast<std::size_t>(TW) * TH * 4);
        for (std::uint32_t y = 0; y < TH; ++y)
            for (std::uint32_t x = 0; x < TW; ++x)
            {
                const std::uint32_t tile = (y / TS) * COLS + (x / TS);
                const std::uint32_t lx = x % TS, ly = y % TS;
                std::uint32_t c = kBase[tile];
                const bool border = lx == 0 || ly == 0 || lx == TS - 1 || ly == TS - 1;
                const bool accent = ((lx + ly) % TS) < 2;
                const auto dim = [](std::uint32_t v, std::uint32_t num) {
                    return ((((v >> 16) & 0xFF) * num / 256) << 16) |
                           ((((v >> 8) & 0xFF) * num / 256) << 8) |
                           (((v & 0xFF) * num / 256));
                };
                if (border)      c = 0xFF000000u | dim(c, 140);
                else if (accent) c = 0xFF000000u | dim(c, 210);
                const std::size_t o = (static_cast<std::size_t>(y) * TW + x) * 4;
                px[o + 0] = std::byte(c & 0xFF);
                px[o + 1] = std::byte((c >> 8) & 0xFF);
                px[o + 2] = std::byte((c >> 16) & 0xFF);
                px[o + 3] = std::byte(0xFF);
            }

        lux::rdesc::TextureInfo ti{};
        ti.width = TW; ti.height = TH; ti.channel = 4;
        ti.pixel_format = lux::rdesc::ETexturePixelFormat::RGBA8_UNORM;
        ti.mip_count = 1; ti.layers = 1;
        // Pixel-art atlas: NO mip chain (a minified average of a tileset is
        // meaningless — it bleeds neighbouring tiles into every seam).
        ti.flags = lux::rdesc::toUnderlying(lux::rdesc::ETextureAssetFlags::NO_MIPS);

        auto info  = std::make_unique<lux::asset::AssetInfo>();
        info->id   = uuids::uuid::from_string("aaaa1111-2222-3333-4444-555566667777").value();
        info->type = lux::asset::EAssetType::TEXTURE;
        const auto id = info->id;

        auto a = std::make_unique<lux::asset::TextureAsset>(std::move(info));
        auto texture = lux::rdesc::Texture::copyOf(ti, px);
        if (!texture)
            return {};
        a->setData(std::make_unique<lux::rdesc::Texture>(
            std::move(*texture)));
        mgr.registerAsset(std::move(a));
        return id;
    }

    /// Rolling-hills terrain into the map (one fill-shaped rebuild).
    void generateTerrain(
        d2::TilemapRuntime& runtime,
        d2::TilemapHandle tilemap,
        std::uint32_t seed)
    {
        Lcg rng{seed};
        for (std::uint32_t y = 0; y < MAP_H; ++y)
            for (std::uint32_t x = 0; x < MAP_W; ++x)
                (void)runtime.setTile(
                    tilemap,
                    {
                        static_cast<std::int64_t>(x),
                        static_cast<std::int64_t>(y)},
                    lux::rdesc::kEmptyTile);
        const float p0 = rng.unit() * 6.28f, p1 = rng.unit() * 6.28f;
        for (std::uint32_t x = 0; x < MAP_W; ++x)
        {
            const float fx = static_cast<float>(x);
            const auto ground = static_cast<std::uint32_t>(
                34.f + 16.f * std::sin(fx * 0.045f + p0) + 8.f * std::sin(fx * 0.11f + p1));
            for (std::uint32_t y = 0; y <= ground; ++y)
            {
                std::uint16_t id = kStone;
                if (y == ground)            id = kGrass;
                else if (y + 6 >= ground)   id = kDirt;
                else if ((rng.next() & 31u) == 0u) id = kOre;   // sparkle
                (void)runtime.setTile(
                    tilemap,
                    {
                        static_cast<std::int64_t>(x),
                        static_cast<std::int64_t>(y)},
                    id);
            }
            // occasional floating brick platform
            if ((x % 24u) == 12u)
            {
                const std::uint32_t py = ground + 10u + (rng.next() % 8u);
                for (std::uint32_t k = 0; k < 6 && x + k < MAP_W; ++k)
                    (void)runtime.setTile(
                        tilemap,
                        {
                            static_cast<std::int64_t>(x + k),
                            static_cast<std::int64_t>(py)},
                        kBrick);
            }
        }
    }
} // namespace

int main(int argc, char** argv)
{
    std::setbuf(stdout, nullptr);
    lux::rendertest::VisualSoak soak;
    if (!lux::rendertest::VisualSoak::parse(argc, argv, soak))
        return 2;
    std::printf(
        "=== tilemap_visual_stress ===  (32k-tile map, %s)\n",
        soak.enabled() ? "soak mode — exits by itself" : "close the window to exit"
    );

    lux::rendertest::DeviceRenderFixture fx(W, H, "Tilemap Visual Stress — one instance, 32k tiles");
    if (!fx.ok()) { std::printf("No Vulkan device. Skipping.\n"); return 0; }

    const auto sv = fx.makeSceneWithSwapchainView("TilemapWorld", "main");

    const auto cam_reg = fx.awaitControl(fx.control().registerFeatureType(kViewCameraFeatureFactory));
    fx.awaitControl(fx.control().addFeature(sv.scene_id, cam_reg.feature_type_id, lux::render::ViewCameraCommTag{}));
    const auto canvas_reg = fx.awaitControl(fx.control().registerFeatureType(kCanvas2DFeatureFactory));
    fx.awaitControl(fx.control().addFeature(sv.scene_id, canvas_reg.feature_type_id, lux::render::Canvas2DCommConfig{}));

    FeatureCatalog features;
    features.injectForTest("StandardViewCamera",
                           std::span<const TypeId>{cam_reg.ops, cam_reg.op_count});
    features.injectForTest("Canvas2D",
                           std::span<const TypeId>{canvas_reg.ops, canvas_reg.op_count});

    lux::asset::AssetManager assets{
        lux::asset::runtimeAssetCodecCatalog()};
    const auto tileset_id = registerTilesetTexture(assets);

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
    d2::TilemapRuntime tilemap_runtime;
    // 资产归驻留胶水:它把组件里的 asset_id 经三件套解析成 GPU 句柄写进
    // cache 组件。
    // 驻留胶水是一个普通节点(批 B1)。本 demo 手工驱动(不走
    // Schedule::tick),所以 Schedule 自动做的三件事在这里手动做:
    // 连信号、每帧 drain、登记成服务供包的 resolve* 声明取用。
    auto residency_glue = std::make_unique<lux::ecs::ResidencySubsystem>(assets);
    residency_glue->setCallbacks(residency.makeCallbacks());
    auto* const residency_owner = residency_glue.get();
    lux::ecs::RenderSystemBuilder render_builder;
    if (!render_builder.add(std::move(residency_glue)) ||
        !render_builder.add(std::make_unique<lux::ecs::CameraViewSubsystem>()))
    {
        std::printf("Failed to stage base render subsystems.\n");
        return 1;
    }

    // Reverse destruction is intentional: builder → schedule systems → owned
    // services → residency observers → World registry. Longer-lived borrowed
    // render/asset resources remain below that graph.
    lux::ecs::SceneServices   services;
    lux::ecs::Schedule        schedule{world};
    lux::ecs::ScheduleBuilder assembly{schedule, services};

    auto& staged = assembly.services();
    if (!staged.adopt(tilemap_runtime) ||
        !staged.adopt(*residency_owner) ||
        !staged.adopt(render_builder))
    {
        std::printf("Failed to stage 2D scene services.\n");
        return 1;
    }

    // This executable is the host boundary for the linked component sidecars.
    // Drain their registrars before the pack inspects reflected component data.
    lux::meta::meta_module_init();
    lux::ecs::ComponentTypeCatalog components;
    if (!lux::ecs::registerGeneratedComponents(components))
        return 1;

    lux::runtime::SceneContributionCatalog contributions;
    auto transform2d =
        lux::runtime::makeSpatial2DTransformContribution(components);
    auto simulation2d =
        lux::runtime::makeSimulation2DContribution(components);
    auto presentation2d =
        lux::runtime::makePresentation2DContribution(components);
    if (!transform2d || !simulation2d || !presentation2d)
        return 1;
    std::vector<lux::runtime::SceneContributionDescriptor> descriptors;
    descriptors.push_back(std::move(*transform2d));
    descriptors.push_back(std::move(*simulation2d));
    descriptors.push_back(std::move(*presentation2d));
    if (!contributions.addBatch(std::move(descriptors)))
        return 1;
    constexpr std::array selected{
        lux::scene::sceneFeatureId(
            lux::runtime::kPresentation2DContributionName)};
    if (!contributions.assembleDefaults(assembly, selected))
    {
        std::printf("scene feature assembly failed\n");
        return 1;
    }
    auto render_plan = std::move(render_builder).compile();
    if (!render_plan)
    {
        std::printf("render subsystem graph failed\n");
        return 1;
    }
    auto render_system = std::make_unique<lux::ecs::RenderSystem>(
        fx.session(),
        fx.control(),
        async.uploadClient(),
        fx.control().adoptScene(sv.scene_id),
        std::move(*render_plan));
    auto* const render_owner = render_system.get();
    render_system->setFeatures(features);
    if (!assembly.add(std::move(render_system), lux::ecs::kPhaseRender))
    {
        std::printf("RenderSystem installation failed\n");
        return 1;
    }
    if (const auto committed = assembly.commit(); !committed)
    {
        std::printf("schedule commit failed\n");
        return 1;
    }

    // 连信号:包的 resolve* 声明此刻已全部注册(Schedule 走 onAdded,手工驱动
    // 就在这里)。
    residency_owner->attach(world.registry());

    const auto cam = world.createEntity();
    world.emplace<d2::Transform2DComponent>(cam);
    auto& cc = world.emplace<d2::Camera2DComponent>(cam);
    cc.units_per_view_height = 3.4f;
    cc.aspect = static_cast<float>(W) / static_cast<float>(H);
    world.emplace<lux::ecs::PrimaryCameraTag>(cam);
    // camera → view wiring is DATA now (RenderViewBinding): this camera drives
    // the demo's swapchain view; unbound cameras are inert.
    world.emplace<lux::ecs::RenderViewBindingComponent>(cam,
        fx.control().adoptView(sv.scene_id, sv.view));

    // the map: min corner at the origin of its entity.
    const auto map_e = world.createEntity();
    world.emplace<d2::Transform2DComponent>(map_e).position = {0.0, 0.0};
    auto& tm = world.emplace<d2::TilemapComponent>(map_e);
    tm.id = d2::TilemapId{
        uuids::uuid::from_string(
            "72000000-0000-4000-8000-000000000001").value()};
    const auto tilemap = tilemap_runtime.create({tm.id});
    world.emplace<d2::TilemapBindingComponent>(
        map_e,
        d2::TilemapBindingComponent{tilemap});
    tm.tileset_texture = tileset_id;
    tm.tileset_cols = 4;
    tm.tileset_rows = 4;
    tm.tile_size    = kTile;
    tm.priority     = 0.f;
    for (std::int64_t chunk_x = 0;
         chunk_x * d2::TilemapRuntime::kChunkSizeTiles < MAP_W;
         ++chunk_x)
    {
        d2::TileChunkLoad chunk;
        chunk.coordinate = {chunk_x, 0};
        chunk.tiles.assign(
            d2::TilemapRuntime::kChunkTileCount,
            lux::rdesc::kEmptyTile);
        if (!tilemap_runtime.loadChunk(tilemap, std::move(chunk)))
            return 1;
    }
    generateTerrain(tilemap_runtime, tilemap, 1u);

    // images on the same priority axis: stars below, clouds above.
    Lcg rng;
    for (int i = 0; i < 150; ++i)
    {
        const auto e = world.createEntity();
        world.emplace<d2::Transform2DComponent>(e).position = {
            static_cast<double>(rng.unit() * MAP_W * kTile),
            static_cast<double>(rng.unit() * MAP_H * kTile)};
        auto& sp = world.emplace<d2::Image2DComponent>(e);
        const float s = 0.008f + 0.012f * rng.unit();
        sp.size = Eigen::Vector2f(s, s);
        sp.tint = 0xFF60E0FFu;
        sp.priority = -5.f;   // behind the map (shows through empty tiles)
    }
    std::vector<lux::meta::entity_id> clouds;
    for (int i = 0; i < 24; ++i)
    {
        const auto e = world.createEntity();
        world.emplace<d2::Transform2DComponent>(e).position = {
            static_cast<double>(rng.unit() * MAP_W * kTile),
            static_cast<double>(4.2f + rng.unit() * 1.8f)};
        auto& sp = world.emplace<d2::Image2DComponent>(e);
        sp.size = Eigen::Vector2f(0.5f + 0.4f * rng.unit(), 0.12f);
        sp.tint = 0x50FFFFFFu;   // translucent premultiplied white
        sp.priority = 5.f;       // above the map
        clouds.push_back(e);
    }


    std::printf("map up: %ux%u tiles (one canvas instance). Mining wave + regen every ~15 s.\n",
                MAP_W, MAP_H);

    const auto t0  = std::chrono::steady_clock::now();
    auto fps_mark  = t0;
    auto last_time = t0;
    int  fps_frames = 0;
    int  frame_no   = 0;
    std::uint32_t seed = 2u;

    while (fx.running())
    {
        const auto now = std::chrono::steady_clock::now();
        const float t  = std::chrono::duration<float>(now - t0).count();
        float dt = std::chrono::duration<float>(now - last_time).count();
        last_time = now;
        if (dt > 0.1f) dt = 0.1f;

        // camera: auto-scroll the full width + breathing zoom (view op only —
        // the untouched map is zero wire traffic).
        const float half_span = MAP_W * kTile * 0.5f;
        // Transform mutation must go through patch so observers see it.
        world.registry().patch<d2::Transform2DComponent>(cam, [&](auto& tc)
        {
            tc.position = {
                static_cast<double>(
                    half_span + (half_span - 2.2f) * std::sin(t * 0.15f)),
                3.0};
        });
        cc.units_per_view_height = 3.4f + 0.8f * std::sin(t * 0.23f);

        // mining wave: a column sweeps across the map — carve this column,
        // restore the trailing one (steady per-frame dirty-rect uploads).
        {
            const auto col  = static_cast<std::uint32_t>(t * 60.f) % MAP_W;
            const auto back = (col + MAP_W - 30u) % MAP_W;
            for (std::uint32_t y = 8; y < 60; ++y)
                (void)tilemap_runtime.setTile(
                    tilemap,
                    {
                        static_cast<std::int64_t>(col),
                        static_cast<std::int64_t>(y)},
                    lux::rdesc::kEmptyTile);
            for (std::uint32_t y = 8; y < 60; ++y)
                (void)tilemap_runtime.setTile(
                    tilemap,
                    {
                        static_cast<std::int64_t>(back),
                        static_cast<std::int64_t>(y)},
                    (y > 40u) ? kDirt : kStone);
        }
        // full regeneration every ~15 s (worst-case full-map upload, once).
        if (frame_no > 0 && (frame_no % 900) == 0)
        {
            generateTerrain(tilemap_runtime, tilemap, seed++);
            std::printf("t=%.0fs regenerated terrain (seed %u) — full-map upload\n", t, seed - 1);
        }

        // clouds drift
        for (std::size_t i = 0; i < clouds.size(); ++i)
        {
            world.registry().patch<d2::Transform2DComponent>(clouds[i], [&](auto& tc)
            {
                auto& p = tc.position;
                p.x += static_cast<double>(
                    dt * (0.05f + 0.05f * static_cast<float>(i % 3)));
                if (p.x > MAP_W * kTile + 0.5f) p.x = -0.5;
            });
        }

        async.drainMainThreadCompletions();   // 驻留管道主线程会合
        schedule.tick(dt);
        residency_owner->drainResolvers(world.registry());
        fx.flush();

        ++frame_no;
        ++fps_frames;
        const auto soak_now = std::chrono::steady_clock::now();
        if (soak.reached(t0, soak_now))
        {
            soak.reportGracefulTeardown(
                t0,
                soak_now,
                static_cast<std::uint64_t>(frame_no)
            );
            break;
        }
        if (now - fps_mark >= std::chrono::seconds(1))
        {
            const auto tile_stats = tilemap_runtime.stats();
            std::printf(
                "fps=%d | resident chunks=%llu | resident bytes=%llu\n",
                fps_frames,
                static_cast<unsigned long long>(
                    tile_stats.resident_chunks),
                static_cast<unsigned long long>(
                    tile_stats.resident_bytes));
            fps_frames = 0;
            fps_mark   = now;
        }
    }
    auto render_close = render_owner->close();
    while (render_close == lux::render::ERenderLeaseCloseStatus::Stopping)
    {
        if (!fx.control().waitAndPumpReplies())
            return 1;
        fx.pumpReplies();
        render_close = render_owner->close();
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

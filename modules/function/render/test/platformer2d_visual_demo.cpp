// ============================================================================
//  platformer2d_visual_demo.cpp — VISUAL (interactive tier): the traditional-2D
//  line playable in one window. One demo covers three slices end to end:
//    A2-01 frame animation  — the player runs a 4-frame walk clip (procedural
//                             atlas sheet → TextureAtlas + FlipbookClip assets,
//                             resolved by Flipbook2DResolver, sampled by the
//                             pure FlipbookAnimationSystem into uv_rect);
//    A2-02 tilemap          — the level ground/platforms are ONE 128×48 tilemap
//                             instance (R16 index texture + tileset atlas);
//    P2-01 controller       — WASD/arrows + Space drive a kinematic
//                             CharacterController2D (axis-separated swept AABB,
//                             gravity, grounded, ONE-WAY platform included)
//                             through the real Simulation2DSystem fixed step.
//
//  CONTROLS:  A/D or ←/→ = run       Space = jump (when grounded)
//             the middle platform is ONE-WAY — jump up through it, land on it.
//
//  NOT self-checking — for eyeballing. `interactive` tier. Exit 0 without Vulkan.
//  `--soak <seconds>` runs the autonomous portion unattended and then enters
//  the normal RAII teardown path; controls still require attended validation.
// ============================================================================

#include "DeviceRenderFixture.hpp"
#include "VisualSoak.hpp"

#include <lux/engine/function/render/client/genops/Canvas2DOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ViewCameraOperation.ops.hpp>
#include <lux/engine/function/render/client/FeatureCatalog.hpp>

#include <lux/engine/ecs/render/SceneRenderBinding.hpp>
#include <lux/engine/ecs/render/RenderSystemStages.hpp>
#include <lux/engine/ecs/render/subsystems/ResidencySubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/CameraViewSubsystem.hpp>
#include <lux/engine/ecs/render/systems/RenderSystem.hpp>
#include <lux/engine/runtime/render/scene/ResidencyAssembly.hpp>
#include <lux/engine/runtime/render/scene/testing/AsyncTestServices.hpp>
#include <lux/engine/ecs/render/components/2d/Image2DComponent.hpp>
#include <lux/engine/ecs/integration/presentation2d/InstallPresentation2DSystems.hpp>
#include <lux/engine/ecs/physics/InstallSimulationSystems.hpp>
#include <lux/engine/ecs/transform/InstallTransformSystems.hpp>
#include <lux/engine/resource/asset/AssetServices.hpp>
#include <lux/engine/ecs/physics/systems/Simulation2DSystem.hpp>
#include <lux/engine/ecs/physics/CollisionProbe2D.hpp>
#include <lux/engine/ecs/physics/FixedStepConfig.hpp>
#include <lux/engine/ecs/physics2d/Physics2DConfig.hpp>
#include <lux/engine/ecs/render/components/RenderViewBindingComponent.hpp>
#include <lux/engine/ecs/render/components/PrimaryCameraTag.hpp>
#include <lux/engine/ecs/components/Transform2DComponent.hpp>
#include <lux/engine/ecs/render/components/2d/Image2DComponent.hpp>
#include <lux/engine/ecs/animation/components/FlipbookAnimationComponent.hpp>
#include <lux/engine/ecs/tilemap/components/TilemapComponent.hpp>
#include <lux/engine/ecs/tilemap/components/TilemapBindingComponent.hpp>
#include <lux/engine/ecs/tilemap/systems/TilemapRuntime.hpp>
#include <lux/engine/ecs/render/components/2d/Camera2DComponent.hpp>
#include <lux/engine/ecs/physics2d/components/Physics2DComponents.hpp>
#include <lux/engine/ecs/physics2d/systems/Physics2DWorld.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>

#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>
#include <lux/engine/resource/asset/texture/TextureAtlasAssets.hpp>
#include <lux/engine/description/Texture.hpp>
#include <lux/engine/description/TextureAtlas.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/input/Input.hpp>

#include <uuid.h>

#include <chrono>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

using namespace lux::render;
namespace d2 = lux::ecs;

namespace
{
    constexpr std::uint32_t W = 1280, H = 800;
    constexpr std::uint32_t MAP_W = 128, MAP_H = 48;
    constexpr float kTile = 0.1f;   // level spans 12.8 × 4.8 world units

    enum : std::uint16_t { kGrass = 0, kDirt = 1, kStone = 2, kBrick = 4 };

    std::unique_ptr<lux::asset::AssetInfo> makeInfo(const char* uuid, lux::asset::EAssetType t)
    {
        auto info  = std::make_unique<lux::asset::AssetInfo>();
        info->id   = uuids::uuid::from_string(uuid).value();
        info->type = t;
        return info;
    }

    lux::asset::asset_id_t registerRgba8Texture(lux::asset::AssetManager& mgr, const char* uuid,
                                                std::uint32_t w, std::uint32_t h,
                                                const std::vector<std::uint32_t>& abgr)
    {
        std::vector<std::byte> px(static_cast<std::size_t>(w) * h * 4);
        for (std::size_t i = 0; i < abgr.size(); ++i)
        {
            const std::uint32_t c = abgr[i];
            px[i * 4 + 0] = std::byte(c & 0xFF);           // R
            px[i * 4 + 1] = std::byte((c >> 8) & 0xFF);    // G
            px[i * 4 + 2] = std::byte((c >> 16) & 0xFF);   // B
            px[i * 4 + 3] = std::byte((c >> 24) & 0xFF);   // A
        }
        lux::rdesc::TextureInfo ti{};
        ti.width = w; ti.height = h; ti.channel = 4;
        ti.pixel_format = lux::rdesc::ETexturePixelFormat::RGBA8_UNORM;
        ti.mip_count = 1; ti.layers = 1;
        // Pixel-art atlas: NO mip chain (a minified average of a tileset is
        // meaningless — it bleeds neighbouring tiles into every seam).
        ti.flags = lux::rdesc::toUnderlying(lux::rdesc::ETextureAssetFlags::NO_MIPS);
        auto info = makeInfo(uuid, lux::asset::EAssetType::TEXTURE);
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

    /// 4×4 tileset (same palette idea as tilemap_visual_stress, minimal).
    lux::asset::asset_id_t registerTileset(lux::asset::AssetManager& mgr)
    {
        constexpr std::uint32_t TS = 16, TW = TS * 4, TH = TS * 4;
        static constexpr std::uint32_t kBase[16] = {
            0xFF3FA34Du, 0xFF275A8Cu, 0xFF6A6A6Au, 0xFF29C5F0u,
            0xFF4444AAu, 0xFF52C5E6u, 0xFFD0B080u, 0xFF2020D0u,
            0xFF808020u, 0xFF208080u, 0xFF802080u, 0xFF404040u,
            0xFFC0C0C0u, 0xFF6090C0u, 0xFF90C060u, 0xFFC06090u,
        };
        std::vector<std::uint32_t> abgr(static_cast<std::size_t>(TW) * TH);
        for (std::uint32_t y = 0; y < TH; ++y)
            for (std::uint32_t x = 0; x < TW; ++x)
            {
                const std::uint32_t tile = (y / TS) * 4u + (x / TS);
                const std::uint32_t lx = x % TS, ly = y % TS;
                std::uint32_t c = kBase[tile];
                if (lx == 0 || ly == 0 || lx == TS - 1 || ly == TS - 1)
                    c = 0xFF000000u | ((c >> 1) & 0x7F7F7Fu);
                abgr[static_cast<std::size_t>(y) * TW + x] = c;
            }
        return registerRgba8Texture(mgr, "bbbb1111-2222-3333-4444-555566667777", TW, TH, abgr);
    }

    /// The player atlas sheet: 4 frames of 16×16 (64×16). A little runner —
    /// head + body constant, legs stride per frame, so the walk cycle reads
    /// clearly even at this fidelity. Frame 0 doubles as the idle pose.
    lux::asset::asset_id_t registerPlayerSheet(lux::asset::AssetManager& mgr)
    {
        constexpr std::uint32_t FS = 16, FRAMES = 4, TW = FS * FRAMES, TH = FS;
        std::vector<std::uint32_t> abgr(static_cast<std::size_t>(TW) * TH, 0x00000000u);
        const auto put = [&](std::uint32_t f, int x, int y, std::uint32_t c)
        {
            if (x < 0 || y < 0 || x >= static_cast<int>(FS) || y >= static_cast<int>(TH)) return;
            // texture row 0 = TOP; draw with y=0 at the bottom of the frame.
            abgr[static_cast<std::size_t>(TH - 1 - y) * TW + f * FS + x] = c;
        };
        constexpr std::uint32_t kSkin = 0xFF7DB4F0u, kBody = 0xFF2222CCu, kLeg = 0xFF223355u;
        static constexpr int kStride[FRAMES][2] = {{0, 0}, {2, -1}, {0, 0}, {-1, 2}};
        for (std::uint32_t f = 0; f < FRAMES; ++f)
        {
            for (int y = 12; y < 16; ++y)                       // head
                for (int x = 6; x < 10; ++x) put(f, x, y, kSkin);
            for (int y = 5; y < 12; ++y)                        // body
                for (int x = 5; x < 11; ++x) put(f, x, y, kBody);
            const int l = kStride[f][0], r = kStride[f][1];     // striding legs
            for (int y = 0; y < 5; ++y)
            {
                put(f, 6 + (l * y) / 5, y, kLeg); put(f, 7 + (l * y) / 5, y, kLeg);
                put(f, 8 + (r * y) / 5, y, kLeg); put(f, 9 + (r * y) / 5, y, kLeg);
            }
        }
        return registerRgba8Texture(mgr, "cccc1111-2222-3333-4444-555566667777", TW, TH, abgr);
    }

    /// Atlas (4 frames over the sheet) + idle/walk clips — the A2-00/A2-01
    /// asset pair, authored in memory exactly as an importer would.
    struct AnimAssets { lux::asset::asset_id_t idle, walk; };
    AnimAssets registerAnimAssets(lux::asset::AssetManager& mgr, lux::asset::asset_id_t sheet)
    {
        auto atlas = std::make_unique<lux::rdesc::TextureAtlas>();
        atlas->name = "player";
        atlas->texture_uuid = lux::asset::opaqueFromAssetId(sheet);
        for (int i = 0; i < 4; ++i)
            atlas->frames.push_back({"run" + std::to_string(i),
                                     Eigen::Vector4f(i * 0.25f, 0.f, 0.25f, 1.f),
                                     Eigen::Vector2f(0.5f, 0.f)});   // pivot at the feet
        auto atlas_info = makeInfo("dddd1111-2222-3333-4444-555566667777",
                                   lux::asset::EAssetType::TEXTURE_ATLAS);
        const auto atlas_id = atlas_info->id;
        mgr.registerAsset(std::make_unique<lux::asset::TextureAtlasAsset>(
            std::move(atlas_info), std::move(atlas)));

        const auto makeClip = [&](const char* uuid, const char* name,
                                  std::vector<lux::rdesc::FlipbookFrame> frames)
        {
            auto clip = std::make_unique<lux::rdesc::FlipbookClip>();
            clip->name = name;
            clip->atlas_uuid = lux::asset::opaqueFromAssetId(atlas_id);
            clip->frames = std::move(frames);
            clip->loop = true;
            auto info = makeInfo(uuid, lux::asset::EAssetType::FLIPBOOK_CLIP);
            const auto id = info->id;
            mgr.registerAsset(std::make_unique<lux::asset::FlipbookClipAsset>(
                std::move(info), std::move(clip)));
            return id;
        };
        AnimAssets out{};
        out.idle = makeClip("eeee1111-2222-3333-4444-555566667777", "idle", {{0u, 0.5f}});
        out.walk = makeClip("ffff1111-2222-3333-4444-555566667777", "walk",
                            {{0u, 0.11f}, {1u, 0.11f}, {2u, 0.11f}, {3u, 0.11f}});
        return out;
    }

    /// One solid rect of the level: painted as tiles AND mirrored as a static
    /// collider (tile coords; both sides derive from the same numbers, so the
    /// visual and the physical world cannot drift apart).
    struct Rect { std::int32_t x, y, w, h; std::uint16_t tile; bool one_way; };
} // namespace

int main(int argc, char** argv)
{
    std::setbuf(stdout, nullptr);
    lux::rendertest::VisualSoak soak;
    if (!lux::rendertest::VisualSoak::parse(argc, argv, soak))
        return 2;
    std::printf("=== platformer2d_visual_demo ===\n"
                "    A/D or arrows = run, Space = jump. The cyan platform is ONE-WAY.\n"
                "    mode: %s\n",
                soak.enabled()
                    ? "soak — exits by itself"
                    : "interactive — close the window to exit");

    lux::rendertest::DeviceRenderFixture fx(W, H, "Platformer2D — tilemap + frame anim + controller");
    if (!fx.ok()) { std::printf("No Vulkan device. Skipping.\n"); return 0; }

    const auto sv = fx.makeSceneWithSwapchainView("Platformer", "main");

    const auto cam_reg = fx.awaitControl(fx.control().registerFeatureType(kViewCameraFeatureFactory));
    fx.awaitControl(fx.control().addFeature(sv.scene_id, cam_reg.feature_type_id, lux::render::ViewCameraCommTag{}));
    const auto canvas_reg = fx.awaitControl(fx.control().registerFeatureType(kCanvas2DFeatureFactory));
    fx.awaitControl(fx.control().addFeature(sv.scene_id, canvas_reg.feature_type_id, lux::render::Canvas2DCommConfig{}));

    FeatureCatalog features;
    features.injectForTest("StandardViewCamera",
                           std::span<const TypeId>{cam_reg.ops, cam_reg.op_count});
    features.injectForTest("Canvas2D",
                           std::span<const TypeId>{canvas_reg.ops, canvas_reg.op_count});

    // ── assets (all procedural, in-memory) ──
    lux::asset::AssetManager assets{
        lux::asset::runtimeAssetCodecCatalog()};
    const auto tileset_id = registerTileset(assets);
    const auto sheet_id   = registerPlayerSheet(assets);
    const auto anims      = registerAnimAssets(assets, sheet_id);

    // 240 Hz fixed step: at 60 Hz a 2.4 u/s runner advances ~0.04 world
    // (≈11 px at this zoom) every ~2.7 display frames — visible judder against
    // the per-frame-smooth camera. 240 Hz cuts the step to ~2.7 px and lands
    // 1–2 substeps on EVERY frame, which is below perception at this scale.
    // (The engine-level cure — fixed-step render interpolation — is a future
    // item; a demo just picks a rate its content can afford.)
    // 配置经 services 交给要它的那个条目（批 5：plan 这个中间人没了）。
    d2::FixedStepConfig fixed{};
    fixed.fixed_dt = 1.f / 240.f;
    d2::Physics2DConfig gravity{0.f, -30.f};

    // This executable is the host boundary for the linked component sidecars.
    // Drain registrars before contribution schema validation.
    // reflected component catalogue.
    lux::meta::meta_module_init();
    lux::ecs::ComponentTypeCatalog components;
    if (!lux::ecs::registerGeneratedComponents(components))
        return 1;

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
    // 资产归驻留胶水:asset_id 经三件套解析成 GPU 句柄写进 cache 组件。
    // 驻留胶水是一个普通节点(批 B1)。本 demo 手工驱动(不走
    // Schedule::tick),所以 Schedule 自动做的三件事在这里手动做:
    // 连信号、每帧 drain、登记成服务供包的 resolve* 声明取用。
    auto residency_glue = std::make_unique<lux::ecs::ResidencySubsystem>(assets);
    residency_glue->setCallbacks(residency.makeCallbacks());
    auto* const residency_owner = residency_glue.get();
    lux::ecs::RenderSystemStages render_builder;
    if (!render_builder.add(std::move(residency_glue)) ||
        !render_builder.add(std::make_unique<lux::ecs::CameraViewSubsystem>()))
    {
        std::printf("Failed to stage base render subsystems.\n");
        return 1;
    }

    // Reverse destruction is intentional: builder → schedule systems → owned
    // services → residency observers → World registry. Longer-lived borrowed
    // render/asset resources remain below that graph.
    lux::asset::AssetServices asset_services{
        assets,
        async.assetClient()};
    lux::ecs::SceneServices   services;
    lux::ecs::Schedule        schedule{world};
    lux::ecs::ScheduleBuilder assembly{schedule, services};

    auto& staged = assembly.services();
    if (!staged.adopt(fixed) ||
        !staged.adopt(gravity) ||
        !staged.adopt(asset_services) ||
        !staged.adopt(tilemap_runtime) ||
        !staged.adopt(*residency_owner) ||
        !staged.adopt(render_builder))
    {
        std::printf("Failed to stage 2D scene services.\n");
        return 1;
    }

    if (!lux::ecs::installSpatial2DTransformSystems(
            assembly, components) ||
        !lux::ecs::installSimulation2DSystems(
            assembly, components) ||
        !lux::ecs::installPresentation2DSystems(
            assembly, components) ||
        !staged.emplace<d2::Physics2DWorld>(gravity))
    {
        std::printf("2D system assembly failed\n");
        return 1;
    }
    auto* const simulation = staged.borrow<d2::Simulation2DSystem>();
    auto* const physics = staged.borrow<d2::Physics2DWorld>();
    auto* const probes = staged.borrow<d2::CollisionProbes2D>();
    if (!simulation || !physics || !probes)
        return 1;
    for (d2::ICollision2DProbe* probe : probes->probes)
        physics->addProbe(probe);
    simulation->setPhase(
        d2::Simulation2DSystem::Phase::SimulatePhysics,
        [physics](lux::ecs::Registry& registry, float dt)
        {
            physics->step(registry, dt);
        });
    auto render_plan = render_builder.freeze();
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
        std::move(render_builder));
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

    // 连信号:包的 resolve* 声明此刻已全部注册(Schedule 走 onAdded,
    // 手工驱动就在这里)。
    residency_owner->attach(world.registry());
    if (services.get<d2::Simulation2DSystem>() == nullptr ||
        services.get<d2::Physics2DWorld>() == nullptr)
    { std::printf("install failed\n"); return 1; }

    const auto cam = world.createEntity();
    world.emplace<d2::Transform2DComponent>(cam);
    auto& cc = world.emplace<d2::Camera2DComponent>(cam);
    cc.units_per_view_height = 3.0f;
    cc.aspect = static_cast<float>(W) / static_cast<float>(H);
    world.emplace<lux::ecs::PrimaryCameraTag>(cam);
    // camera → view wiring is DATA now (RenderViewBinding): this camera drives
    // the demo's swapchain view; unbound cameras are inert.
    world.emplace<lux::ecs::RenderViewBindingComponent>(cam,
        fx.control().adoptView(sv.scene_id, sv.view));

    // ── the level: tiles + colliders from ONE rect list ──
    const auto map_e = world.createEntity();
    world.emplace<d2::Transform2DComponent>(map_e).position = {0.0, 0.0};
    auto& tm = world.emplace<d2::TilemapComponent>(map_e);
    tm.id = d2::TilemapId{
        uuids::uuid::from_string(
            "73000000-0000-4000-8000-000000000001").value()};
    const auto tilemap = tilemap_runtime.create({tm.id});
    world.emplace<d2::TilemapBindingComponent>(
        map_e,
        d2::TilemapBindingComponent{tilemap});
    tm.tileset_texture = tileset_id;
    tm.tileset_cols = 4; tm.tileset_rows = 4;
    tm.tile_size = kTile;
    tm.priority  = 0.f;
    d2::TileChunkLoad level_chunk;
    level_chunk.coordinate = {0, 0};
    level_chunk.tiles.assign(
        d2::TilemapRuntime::kChunkTileCount,
        lux::rdesc::kEmptyTile);
    if (!tilemap_runtime.loadChunk(
            tilemap,
            std::move(level_chunk)))
    {
        return 1;
    }

    const Rect level[] = {
        {0, 0, MAP_W, 3, kStone, false},          // floor
        {0, 3, 3, MAP_H - 3, kStone, false},      // left wall
        {MAP_W - 3, 3, 3, MAP_H - 3, kStone, false},   // right wall
        {14, 3, 12, 1, kGrass, false},            // low ledge
        {34, 6, 10, 1, kBrick, false},            // brick platform
        {52, 9, 10, 1, 6 /*ice-cyan*/, true},     // ONE-WAY platform
        {70, 6, 10, 1, kBrick, false},
        {90, 10, 14, 1, kGrass, false},           // high ledge
        {30, 3, 6, 2, kDirt, false},              // step
    };
    for (const Rect& r : level)
    {
        for (std::uint32_t y = r.y; y < r.y + r.h; ++y)
            for (std::uint32_t x = r.x; x < r.x + r.w; ++x)
                (void)tilemap_runtime.setTile(
                    tilemap,
                    {
                        static_cast<std::int64_t>(x),
                        static_cast<std::int64_t>(y)},
                    r.tile);
        const auto e = world.createEntity();
        world.emplace<d2::Transform2DComponent>(e).position = {
            static_cast<double>((r.x + r.w * 0.5f) * kTile),
            static_cast<double>((r.y + r.h * 0.5f) * kTile)};
        auto& col = world.emplace<d2::Collider2DComponent>(e);
        col.half_extents = Eigen::Vector2f(r.w * 0.5f * kTile, r.h * 0.5f * kTile);
        col.one_way = r.one_way;
    }

    // backdrop stars behind the map
    for (int i = 0; i < 120; ++i)
    {
        const auto e = world.createEntity();
        world.emplace<d2::Transform2DComponent>(e).position = {
            static_cast<double>((static_cast<float>((i * 73) % 128)) * kTile),
            static_cast<double>(0.5f +
                (static_cast<float>((i * 37) % 40)) * kTile)};
        auto& sp = world.emplace<d2::Image2DComponent>(e);
        sp.size = Eigen::Vector2f(0.015f, 0.015f);
        sp.tint = 0xFF505030u;
        sp.priority = -5.f;
    }

    // ── the player ──
    const auto player = world.createEntity();
    auto& pt = world.emplace<d2::Transform2DComponent>(player);
    pt.position = {1.2, 1.5};
    auto& psp = world.emplace<d2::Image2DComponent>(player);
    psp.texture  = sheet_id;
    psp.size     = Eigen::Vector2f(0.24f, 0.24f);
    psp.priority = 10.f;
    auto& pan = world.emplace<d2::FlipbookAnimationComponent>(player);
    pan.clip = anims.idle;
    auto& pcol = world.emplace<d2::Collider2DComponent>(player);
    pcol.half_extents = Eigen::Vector2f(0.07f, 0.12f);
    pcol.offset       = Eigen::Vector2f(0.f, 0.12f);   // image pivot is at the feet
    world.emplace<d2::CharacterController2DComponent>(player);


    std::printf("level up. Run right — the cyan platform at mid-height is jump-through.\n");

    const auto t0  = std::chrono::steady_clock::now();
    auto last_time = t0;
    auto fps_mark  = t0;
    int  fps_frames = 0;
    std::uint64_t total_frames = 0;
    bool was_walking = false;
    lux::input::Input input;

    while (fx.running())
    {
        const auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last_time).count();
        last_time = now;
        if (dt > 0.1f) dt = 0.1f;

        // ── input → controller intent (velocity.y stays owned by physics) ──
        input.sample(fx.window());
        const auto& snapshot = input.snapshot();
        auto& ctl = world.registry().get<d2::CharacterController2DComponent>(player);
        const bool left = snapshot.isKeyHeld(lux::input::EKey::KEY_A) ||
            snapshot.isKeyHeld(lux::input::EKey::KEY_LEFT);
        const bool right = snapshot.isKeyHeld(lux::input::EKey::KEY_D) ||
            snapshot.isKeyHeld(lux::input::EKey::KEY_RIGHT);
        const bool jump = snapshot.isKeyHeld(lux::input::EKey::KEY_SPACE);
        ctl.velocity.x() = (right ? 2.4f : 0.f) + (left ? -2.4f : 0.f);
        if (jump && ctl.grounded)
            ctl.velocity.y() = 9.5f;

        // ── animation clip + facing from intent ──
        auto& anim = world.registry().get<d2::FlipbookAnimationComponent>(player);
        const bool walking = left != right;
        if (walking != was_walking)
        {
            anim.clip = walking ? anims.walk : anims.idle;
            anim.time = 0.f;
            was_walking = walking;
        }
        // Transform mutation must go through patch so observers see it.
        if (walking)
            world.registry().patch<d2::Transform2DComponent>(player, [&](auto& tc)
            {
                tc.scale.x() = right ? 1.f : -1.f;
            });

        // camera follows (smoothed), clamped inside the level
        const auto pp = world.registry().get<d2::Transform2DComponent>(player).position;
        const double target_x = std::min(
            std::max(pp.x, 2.3),
            static_cast<double>(MAP_W * kTile - 2.3f));
        world.registry().patch<d2::Transform2DComponent>(cam, [&](auto& tc)
        {
            auto& cam_p = tc.position;
            cam_p.x += (target_x - cam_p.x) *
                static_cast<double>(std::min(1.f, dt * 6.f));
            cam_p.y += (pp.y + 0.9 - cam_p.y) *
                static_cast<double>(std::min(1.f, dt * 3.f));
        });

        async.drainMainThreadCompletions();                       // 驻留管道主线程会合
        schedule.tick(dt);                       // demand + animation + transforms
        residency_owner->drainResolvers(world.registry());
        fx.flush();

        ++total_frames;
        ++fps_frames;
        const auto soak_now = std::chrono::steady_clock::now();
        if (soak.reached(t0, soak_now))
        {
            soak.reportGracefulTeardown(t0, soak_now, total_frames);
            break;
        }
        if (now - fps_mark >= std::chrono::seconds(1))
        {
            std::printf("fps=%d | pos=(%.2f, %.2f) grounded=%d\n",
                        fps_frames, pp.x, pp.y, ctl.grounded ? 1 : 0);
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

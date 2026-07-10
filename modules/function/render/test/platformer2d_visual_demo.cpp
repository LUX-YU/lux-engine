// ============================================================================
//  platformer2d_visual_demo.cpp — VISUAL (interactive tier): the traditional-2D
//  line playable in one window. One demo covers three slices end to end:
//    A2-01 frame animation  — the player runs a 4-frame walk clip (procedural
//                             spritesheet → SpriteAtlas + SpriteAnimClip assets,
//                             resolved by SpriteAnim2DResolver, sampled by the
//                             pure SpriteAnimationSystem into uv_rect);
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
// ============================================================================

#include "DeviceRenderFixture.hpp"

#include <lux/engine/render/renderer/features/canvas2d/Canvas2DFeatureOps.hpp>
#include <lux/engine/render/renderer/features/view_camera/ViewCameraOperation.hpp>
#include <lux/engine/render/comm/server/FeatureRegistry.hpp>

#include <lux/pack/d2/Scene2D.hpp>
#include <lux/pack/d2/world/components/Transform2DComponent.hpp>
#include <lux/pack/d2/world/components/SpriteComponent.hpp>
#include <lux/pack/d2/world/components/SpriteAnimationComponent.hpp>
#include <lux/pack/d2/world/components/TilemapComponent.hpp>
#include <lux/pack/d2/world/components/Camera2DComponent.hpp>
#include <lux/pack/d2/world/systems/SpriteAnim2DResolver.hpp>
#include <lux/pack/physics2d_demo/Physics2DComponents.hpp>
#include <lux/pack/physics2d_demo/Physics2DWorld.hpp>
#include <lux/pack/physics2d_demo/Physics2DDemoPack.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/render_bridge/RenderableSystem.hpp>

#include <lux/engine/asset/AssetManager.hpp>
#include <lux/engine/asset/TextureAsset.hpp>
#include <lux/engine/asset/Sprite2DAssets.hpp>
#include <lux/engine/description/Texture.hpp>
#include <lux/engine/description/Sprite2D.hpp>

#include <uuid.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

using namespace lux::render;
namespace d2 = lux::pack;
using lux::window::KeyEnum;
using lux::window::KeyState;

namespace
{
    struct EmptyConfig {};
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
        ti.copy = true; ti.owns_data = true;
        // Pixel-art atlas: NO mip chain (a minified average of a tileset is
        // meaningless — it bleeds neighbouring tiles into every seam).
        ti.flags = lux::rdesc::toUnderlying(lux::rdesc::ETextureAssetFlags::NO_MIPS);
        auto info = makeInfo(uuid, lux::asset::EAssetType::TEXTURE);
        const auto id = info->id;
        auto a = std::make_unique<lux::asset::TextureAsset>(std::move(info));
        a->setData(std::make_unique<lux::rdesc::Texture>(ti, px.data(), px.size()));
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

    /// The player spritesheet: 4 frames of 16×16 (64×16). A little runner —
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
        auto atlas = std::make_unique<lux::rdesc::SpriteAtlas>();
        atlas->name = "player";
        atlas->texture_uuid = lux::asset::opaqueFromAssetId(sheet);
        for (int i = 0; i < 4; ++i)
            atlas->frames.push_back({"run" + std::to_string(i),
                                     Eigen::Vector4f(i * 0.25f, 0.f, 0.25f, 1.f),
                                     Eigen::Vector2f(0.5f, 0.f)});   // pivot at the feet
        auto atlas_info = makeInfo("dddd1111-2222-3333-4444-555566667777",
                                   lux::asset::EAssetType::SPRITE_ATLAS);
        const auto atlas_id = atlas_info->id;
        mgr.registerAsset(std::make_unique<lux::asset::SpriteAtlasAsset>(
            std::move(atlas_info), std::move(atlas)));

        const auto makeClip = [&](const char* uuid, const char* name,
                                  std::vector<lux::rdesc::SpriteAnimFrame> frames)
        {
            auto clip = std::make_unique<lux::rdesc::SpriteAnimClip>();
            clip->name = name;
            clip->atlas_uuid = lux::asset::opaqueFromAssetId(atlas_id);
            clip->frames = std::move(frames);
            clip->loop = true;
            auto info = makeInfo(uuid, lux::asset::EAssetType::SPRITE_ANIM_CLIP);
            const auto id = info->id;
            mgr.registerAsset(std::make_unique<lux::asset::SpriteAnimClipAsset>(
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

int main()
{
    std::setbuf(stdout, nullptr);
    std::printf("=== platformer2d_visual_demo ===\n"
                "    A/D or arrows = run, Space = jump. The cyan platform is ONE-WAY.\n");

    lux::rendertest::DeviceRenderFixture fx(W, H, "Platformer2D — tilemap + frame anim + controller");
    if (!fx.ok()) { std::printf("No Vulkan device. Skipping.\n"); return 0; }

    const auto sv = fx.makeSceneWithSwapchainView("Platformer", "main");

    const auto cam_reg = fx.await(fx.session().registerFeatureType(kStandardViewCameraFeatureFactory));
    fx.await(fx.session().addFeature(sv.scene_id, cam_reg.feature_type_id, EmptyConfig{}));
    const auto canvas_reg = fx.await(fx.session().registerFeatureType(kCanvas2DFeatureFactory));
    fx.await(fx.session().addFeature(sv.scene_id, canvas_reg.feature_type_id, EmptyConfig{}));

    FeatureRegistry features;
    features.injectForTest("StandardViewCamera",
                           std::span<const TypeId>{cam_reg.ops, cam_reg.op_count});
    features.injectForTest("Canvas2D",
                           std::span<const TypeId>{canvas_reg.ops, canvas_reg.op_count});

    // ── assets (all procedural, in-memory) ──
    lux::asset::AssetManager assets;
    const auto tileset_id = registerTileset(assets);
    const auto sheet_id   = registerPlayerSheet(assets);
    const auto anims      = registerAnimAssets(assets, sheet_id);

    // ── gameplay world: traditional + animation + physics + controller ──
    lux::render_bridge::SceneServices services;   // must outlive the World
    lux::ecs::World world;
    // 240 Hz fixed step: at 60 Hz a 2.4 u/s runner advances ~0.04 world
    // (≈11 px at this zoom) every ~2.7 display frames — visible judder against
    // the per-frame-smooth camera. 240 Hz cuts the step to ~2.7 px and lands
    // 1–2 substeps on EVERY frame, which is below perception at this scale.
    // (The engine-level cure — fixed-step render interpolation — is a future
    // item; a demo just picks a rate its content can afford.)
    d2::FixedStepConfig fixed{};
    fixed.fixed_dt = 1.f / 240.f;
    const auto plan = d2::traditional2DPlan()
                          .enableSpriteAnimation()
                          .enablePhysics({0.f, -30.f})
                          .enableCharacterController()
                          .setFixedStep(fixed);
    // The demo assembles its own registry: the d2 pack + THE demo physics
    // pack — physics is externally backed (ADR v3), the engine ships no solver.
    lux::render_bridge::ScenePackRegistry packs;
    d2::addD2Pack(packs);
    d2::addPhysics2DDemoPack(packs);
    const auto installed = d2::install(world, services, packs, plan);
    if (!installed.ok || installed.simulation == nullptr ||
        services.get<d2::Physics2DWorld>() == nullptr)
    { std::printf("install failed\n"); return 1; }

    d2::SpriteAnim2DResolver resolver(assets);   // app-level, runs before tick

    const auto cam = world.createEntity();
    world.emplace<d2::Transform2DComponent>(cam);
    auto& cc = world.emplace<d2::Camera2DComponent>(cam);
    cc.units_per_view_height = 3.0f;
    cc.aspect = static_cast<float>(W) / static_cast<float>(H);
    cc.y_flip = true;
    world.emplace<d2::ActiveCamera2DTag>(cam);

    // ── the level: tiles + colliders from ONE rect list ──
    const auto map_e = world.createEntity();
    world.emplace<d2::Transform2DComponent>(map_e).position = Eigen::Vector2f(0.f, 0.f);
    auto& tm = world.emplace<d2::TilemapComponent>(map_e);
    tm.tileset_texture = tileset_id;
    tm.tileset_cols = 4; tm.tileset_rows = 4;
    tm.tile_size = kTile;
    tm.priority  = 0.f;
    tm.resize(MAP_W, MAP_H);

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
        tm.fill(r.x, r.y, r.w, r.h, r.tile);
        const auto e = world.createEntity();
        world.emplace<d2::Transform2DComponent>(e).position = Eigen::Vector2f(
            (r.x + r.w * 0.5f) * kTile, (r.y + r.h * 0.5f) * kTile);
        auto& col = world.emplace<d2::Collider2DComponent>(e);
        col.half_extents = Eigen::Vector2f(r.w * 0.5f * kTile, r.h * 0.5f * kTile);
        col.one_way = r.one_way;
    }

    // backdrop stars behind the map
    for (int i = 0; i < 120; ++i)
    {
        const auto e = world.createEntity();
        world.emplace<d2::Transform2DComponent>(e).position = Eigen::Vector2f(
            (static_cast<float>((i * 73) % 128)) * kTile,
            0.5f + (static_cast<float>((i * 37) % 40)) * kTile);
        auto& sp = world.emplace<d2::SpriteComponent>(e);
        sp.size = Eigen::Vector2f(0.015f, 0.015f);
        sp.tint = 0xFF505030u;
        sp.priority = -5.f;
    }

    // ── the player ──
    const auto player = world.createEntity();
    auto& pt = world.emplace<d2::Transform2DComponent>(player);
    pt.position = Eigen::Vector2f(1.2f, 1.5f);
    auto& psp = world.emplace<d2::SpriteComponent>(player);
    psp.texture  = sheet_id;
    psp.size     = Eigen::Vector2f(0.24f, 0.24f);
    psp.priority = 10.f;
    auto& pan = world.emplace<d2::SpriteAnimationComponent>(player);
    pan.clip = anims.idle;
    auto& pcol = world.emplace<d2::Collider2DComponent>(player);
    pcol.half_extents = Eigen::Vector2f(0.07f, 0.12f);
    pcol.offset       = Eigen::Vector2f(0.f, 0.12f);   // sprite pivot is at the feet
    world.emplace<d2::CharacterController2DComponent>(player);

    lux::render_bridge::RenderableSystem rs(fx.session(), assets, sv.scene_id, sv.view);
    rs.setFeatures(features);
    d2::registerBridges(rs, services, packs, plan);

    std::printf("level up. Run right — the cyan platform at mid-height is jump-through.\n");

    const auto t0  = std::chrono::steady_clock::now();
    auto last_time = t0;
    auto fps_mark  = t0;
    int  fps_frames = 0;
    bool was_walking = false;

    while (fx.running())
    {
        const auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last_time).count();
        last_time = now;
        if (dt > 0.1f) dt = 0.1f;

        // ── input → controller intent (velocity.y stays owned by physics) ──
        auto& ctl = world.registry().get<d2::CharacterController2DComponent>(player);
        const bool left  = fx.window().queryKey(KeyEnum::KEY_A) == KeyState::PRESS ||
                           fx.window().queryKey(KeyEnum::KEY_LEFT) == KeyState::PRESS;
        const bool right = fx.window().queryKey(KeyEnum::KEY_D) == KeyState::PRESS ||
                           fx.window().queryKey(KeyEnum::KEY_RIGHT) == KeyState::PRESS;
        const bool jump  = fx.window().queryKey(KeyEnum::KEY_SPACE) == KeyState::PRESS;
        ctl.velocity.x() = (right ? 2.4f : 0.f) + (left ? -2.4f : 0.f);
        if (jump && ctl.grounded)
            ctl.velocity.y() = 9.5f;

        // ── animation clip + facing from intent ──
        auto& anim = world.registry().get<d2::SpriteAnimationComponent>(player);
        const bool walking = left != right;
        if (walking != was_walking)
        {
            anim.clip = walking ? anims.walk : anims.idle;
            anim.time = 0.f;
            was_walking = walking;
        }
        if (walking)
            world.registry().get<d2::Transform2DComponent>(player).scale.x() =
                right ? 1.f : -1.f;

        // camera follows (smoothed), clamped inside the level
        auto& cam_p = world.registry().get<d2::Transform2DComponent>(cam).position;
        const auto& pp = world.registry().get<d2::Transform2DComponent>(player).position;
        const float target_x = std::min(std::max(pp.x(), 2.3f), MAP_W * kTile - 2.3f);
        cam_p.x() += (target_x - cam_p.x()) * std::min(1.f, dt * 6.f);
        cam_p.y() += (pp.y() + 0.9f - cam_p.y()) * std::min(1.f, dt * 3.f);

        resolver.update(world.registry(), dt);   // A2-01: assets → cache, pre-tick
        world.tick(dt);                          // fixed-step physics + anim + transforms
        rs.update(world.registry(), dt);
        fx.flush();

        ++fps_frames;
        if (now - fps_mark >= std::chrono::seconds(1))
        {
            std::printf("fps=%d | pos=(%.2f, %.2f) grounded=%d\n",
                        fps_frames, pp.x(), pp.y(), ctl.grounded ? 1 : 0);
            fps_frames = 0;
            fps_mark   = now;
        }
    }
    return 0;
}

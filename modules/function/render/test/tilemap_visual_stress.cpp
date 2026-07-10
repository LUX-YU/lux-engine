// ============================================================================
//  tilemap_visual_stress.cpp — VISUAL (interactive tier): the A2-02 tilemap
//  path under load. One 256×128-tile map (32k tiles, ONE canvas instance +
//  ONE R16 index texture) drawn through the real Tilemap2DBridge over the real
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
//    - sprites BELOW (parallax stars) and ABOVE (drifting clouds) the map on
//      the same priority axis — tile runs interleave with sprite runs.
//    - console: fps + map revision once per second.
//
//  NOT self-checking — for eyeballing. `interactive` tier. Exit 0 without Vulkan.
// ============================================================================

#include "DeviceRenderFixture.hpp"

#include <lux/engine/render/renderer/features/canvas2d/Canvas2DFeatureOps.hpp>
#include <lux/engine/render/renderer/features/view_camera/ViewCameraOperation.hpp>
#include <lux/engine/render/comm/server/FeatureRegistry.hpp>

#include <lux/engine/gameplay/2d/Scene2D.hpp>
#include <lux/engine/gameplay/2d/world/components/Transform2DComponent.hpp>
#include <lux/engine/gameplay/2d/world/components/SpriteComponent.hpp>
#include <lux/engine/gameplay/2d/world/components/TilemapComponent.hpp>
#include <lux/engine/gameplay/2d/world/components/Camera2DComponent.hpp>
#include <lux/engine/gameplay/world/World.hpp>
#include <lux/engine/gameplay/render_bridge/RenderableSystem.hpp>

#include <lux/engine/asset/AssetManager.hpp>
#include <lux/engine/asset/TextureAsset.hpp>
#include <lux/engine/description/Texture.hpp>

#include <uuid.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

using namespace lux::render;
namespace d2 = lux::gameplay::d2;

namespace
{
    struct EmptyConfig {};
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
        ti.copy = true; ti.owns_data = true;

        auto info  = std::make_unique<lux::asset::AssetInfo>();
        info->id   = uuids::uuid::from_string("aaaa1111-2222-3333-4444-555566667777").value();
        info->type = lux::asset::EAssetType::TEXTURE;
        const auto id = info->id;

        auto a = std::make_unique<lux::asset::TextureAsset>(std::move(info));
        a->setData(std::make_unique<lux::rdesc::Texture>(ti, px.data(), px.size()));
        mgr.registerAsset(std::move(a));
        return id;
    }

    /// Rolling-hills terrain into the map (one fill-shaped rebuild).
    void generateTerrain(d2::TilemapComponent& tm, std::uint32_t seed)
    {
        Lcg rng{seed};
        tm.fill(0, 0, static_cast<std::int32_t>(MAP_W), static_cast<std::int32_t>(MAP_H),
                d2::kEmptyTile);
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
                tm.setTile(x, y, id);
            }
            // occasional floating brick platform
            if ((x % 24u) == 12u)
            {
                const std::uint32_t py = ground + 10u + (rng.next() % 8u);
                for (std::uint32_t k = 0; k < 6 && x + k < MAP_W; ++k)
                    tm.setTile(x + k, py, kBrick);
            }
        }
    }
} // namespace

int main()
{
    std::setbuf(stdout, nullptr);
    std::printf("=== tilemap_visual_stress ===  (32k-tile map, close the window to exit)\n");

    lux::rendertest::DeviceRenderFixture fx(W, H, "Tilemap Visual Stress — one instance, 32k tiles");
    if (!fx.ok()) { std::printf("No Vulkan device. Skipping.\n"); return 0; }

    const auto sv = fx.makeSceneWithSwapchainView("TilemapWorld", "main");

    const auto cam_reg = fx.await(fx.session().registerFeatureType(kStandardViewCameraFeatureFactory));
    fx.await(fx.session().addFeature(sv.scene_id, cam_reg.feature_type_id, EmptyConfig{}));
    const auto canvas_reg = fx.await(fx.session().registerFeatureType(kCanvas2DFeatureFactory));
    fx.await(fx.session().addFeature(sv.scene_id, canvas_reg.feature_type_id, EmptyConfig{}));

    FeatureRegistry features;
    features.injectForTest("StandardViewCamera",
                           std::span<const TypeId>{cam_reg.ops, cam_reg.op_count});
    features.injectForTest("Canvas2D",
                           std::span<const TypeId>{canvas_reg.ops, canvas_reg.op_count});

    lux::asset::AssetManager assets;
    const auto tileset_id = registerTilesetTexture(assets);

    // ── gameplay world (traditional plan: tilemap rides SpriteRendering) ──
    lux::gameplay::World world;
    const auto plan = d2::traditional2DPlan();
    (void)d2::install(world, nullptr, plan);

    const auto cam = world.createEntity();
    world.emplace<d2::Transform2DComponent>(cam);
    auto& cc = world.emplace<d2::Camera2DComponent>(cam);
    cc.units_per_view_height = 3.4f;
    cc.aspect = static_cast<float>(W) / static_cast<float>(H);
    cc.y_flip = true;
    world.emplace<d2::ActiveCamera2DTag>(cam);

    // the map: min corner at the origin of its entity.
    const auto map_e = world.createEntity();
    world.emplace<d2::Transform2DComponent>(map_e).position = Eigen::Vector2f(0.f, 0.f);
    auto& tm = world.emplace<d2::TilemapComponent>(map_e);
    tm.tileset_texture = tileset_id;
    tm.tileset_cols = 4;
    tm.tileset_rows = 4;
    tm.tile_size    = kTile;
    tm.priority     = 0.f;
    tm.resize(MAP_W, MAP_H);
    generateTerrain(tm, 1u);

    // sprites on the same priority axis: stars below, clouds above.
    Lcg rng;
    for (int i = 0; i < 150; ++i)
    {
        const auto e = world.createEntity();
        world.emplace<d2::Transform2DComponent>(e).position =
            Eigen::Vector2f(rng.unit() * MAP_W * kTile, rng.unit() * MAP_H * kTile);
        auto& sp = world.emplace<d2::SpriteComponent>(e);
        const float s = 0.008f + 0.012f * rng.unit();
        sp.size = Eigen::Vector2f(s, s);
        sp.tint = 0xFF60E0FFu;
        sp.priority = -5.f;   // behind the map (shows through empty tiles)
    }
    std::vector<lux::meta::entity_id> clouds;
    for (int i = 0; i < 24; ++i)
    {
        const auto e = world.createEntity();
        world.emplace<d2::Transform2DComponent>(e).position =
            Eigen::Vector2f(rng.unit() * MAP_W * kTile, 4.2f + rng.unit() * 1.8f);
        auto& sp = world.emplace<d2::SpriteComponent>(e);
        sp.size = Eigen::Vector2f(0.5f + 0.4f * rng.unit(), 0.12f);
        sp.tint = 0x50FFFFFFu;   // translucent premultiplied white
        sp.priority = 5.f;       // above the map
        clouds.push_back(e);
    }

    lux::gameplay::RenderableSystem rs(fx.session(), assets, sv.scene_id, sv.view);
    rs.setFeatures(features);
    d2::registerBridges(rs, nullptr, plan);

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
        world.registry().get<d2::Transform2DComponent>(cam).position = Eigen::Vector2f(
            half_span + (half_span - 2.2f) * std::sin(t * 0.15f), 3.0f);
        cc.units_per_view_height = 3.4f + 0.8f * std::sin(t * 0.23f);

        // mining wave: a column sweeps across the map — carve this column,
        // restore the trailing one (steady per-frame dirty-rect uploads).
        {
            const auto col  = static_cast<std::uint32_t>(t * 60.f) % MAP_W;
            const auto back = (col + MAP_W - 30u) % MAP_W;
            for (std::uint32_t y = 8; y < 60; ++y) tm.setTile(col, y, d2::kEmptyTile);
            for (std::uint32_t y = 8; y < 60; ++y)
                tm.setTile(back, y, (y > 40u) ? kDirt : kStone);
        }
        // full regeneration every ~15 s (worst-case full-map upload, once).
        if (frame_no > 0 && (frame_no % 900) == 0)
        {
            generateTerrain(tm, seed++);
            std::printf("t=%.0fs regenerated terrain (seed %u) — full-map upload\n", t, seed - 1);
        }

        // clouds drift
        for (std::size_t i = 0; i < clouds.size(); ++i)
        {
            auto& p = world.registry().get<d2::Transform2DComponent>(clouds[i]).position;
            p.x() += dt * (0.05f + 0.05f * static_cast<float>(i % 3));
            if (p.x() > MAP_W * kTile + 0.5f) p.x() = -0.5f;
        }

        world.tick(dt);
        rs.update(world.registry(), dt);
        fx.flush();

        ++frame_no;
        ++fps_frames;
        if (now - fps_mark >= std::chrono::seconds(1))
        {
            std::printf("fps=%d | map revision=%llu | dirty=%s\n",
                        fps_frames,
                        static_cast<unsigned long long>(tm.revision),
                        tm.hasDirty() ? "pending" : "clean");
            fps_frames = 0;
            fps_mark   = now;
        }
    }
    return 0;
}

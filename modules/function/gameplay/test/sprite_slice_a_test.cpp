// ============================================================================
//  sprite_slice_a_test.cpp — Slice A / S2-01: the full traditional-2D sprite chain,
//  ECS → Sprite2DBridge → Canvas2D submit op, over a real RenderSession command channel
//  against the GPU-free fake render server (HeadlessBridgeFixture).
//
//  Stands up a neutral World with 2D Core (Transform2DSystem + Camera2DSystem via
//  d2::install), a sprite entity, and a RenderableSystem wired with d2::registerBridges
//  (Camera2DUploadBridge + Sprite2DBridge). One tick drives the bridge; the fake server
//  records the submitted SpriteDraw[]. Asserts the ECS → SpriteDraw mapping (world
//  transform, size scaling, tint, DrawOrderKey) and that an invisible sprite submits
//  nothing. (The submit→GPU-draw half of the chain is covered on a real device by
//  render/test's graph_dump_stability_test.)
//
//  Self-checking: 0 = PASS, 1 = FAIL.
// ============================================================================

#include "HeadlessBridgeFixture.hpp"

#include <lux/engine/gameplay/2d/Scene2D.hpp>                                   // install / registerBridges / D2ScenePlan
#include <lux/engine/gameplay/2d/world/components/Transform2DComponent.hpp>
#include <lux/engine/gameplay/2d/world/components/SpriteComponent.hpp>
#include <lux/engine/gameplay/2d/world/components/Camera2DComponent.hpp>        // Camera2DComponent + ActiveCamera2DTag
#include <lux/engine/gameplay/world/World.hpp>
#include <lux/engine/gameplay/render_bridge/RenderableSystem.hpp>
#include <lux/engine/render/renderer/features/canvas2d/Canvas2DOperation.hpp>   // SpriteDraw / DrawOrderKey
#include <lux/engine/meta/LuxObject.hpp>                                        // entt::to_integral

#include <Eigen/Core>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

using lux::gameplay::World;
using lux::gameplay::RenderableSystem;
namespace d2 = lux::gameplay::d2;

namespace
{
    int g_failures = 0;
    void check(bool c, const char* m) { std::printf("[%s] %s\n", c ? " ok " : "FAIL", m); if (!c) ++g_failures; }
}

int main()
{
    std::printf("=== sprite_slice_a_test (S2-01) ===\n");

    lux::bridgetest::HeadlessBridgeFixture fix;
    fix.registerCanvas2DOps();

    // Neutral World with traditional 2D (Core: Transform2DSystem → WorldTransform2D;
    // Camera2DSystem — plus SpriteRendering, so registerBridges adds the sprite producer).
    World world;
    const auto plan = d2::traditional2DPlan();
    d2::install(world, /*runtime=*/nullptr, plan);

    // An active camera (the Camera2DUploadBridge gracefully no-ops here — the fixture has
    // no StandardViewCamera feature — proving a 2D scene without that feature still runs).
    const auto cam = world.createEntity();
    world.emplace<d2::Transform2DComponent>(cam);
    auto& cc = world.emplace<d2::Camera2DComponent>(cam);
    cc.units_per_view_height = 10.f;
    cc.aspect = 1.f;
    world.emplace<d2::ActiveCamera2DTag>(cam);

    // One sprite at world (1,2), size (3,4), green tint, layer 5.
    const auto spr = world.createEntity();
    auto& tc = world.emplace<d2::Transform2DComponent>(spr);
    tc.position = Eigen::Vector2f(1.f, 2.f);
    auto& sp = world.emplace<d2::SpriteComponent>(spr);
    sp.size  = Eigen::Vector2f(3.f, 4.f);
    sp.tint  = 0xFF00FF00u;
    sp.layer = 5;

    world.tick(1.f / 60.f);   // compose WorldTransform2D + Camera2DCache

    // RenderableSystem wired to the fake scene + the 2D bridge set.
    RenderableSystem rs(fix.session(), fix.assetMgr(), fix.scene(), fix.view());
    rs.setFeatures(fix.features());
    d2::registerBridges(rs, /*runtime=*/nullptr, plan);

    // One tick: Sprite2DBridge reads the ECS and submits the batch; roundTrip lets the
    // fake server dispatch it into the recorder.
    fix.beginFrame();
    rs.update(world.registry(), 1.f / 60.f);
    fix.roundTrip();

    check(fix.recorder().count("Canvas2DSubmit") == 1, "Sprite2DBridge submitted exactly one batch");
    const auto& bytes = fix.recorder().by_op.at("Canvas2DSubmit").at(0);
    check(bytes.size() == sizeof(lux::render::SpriteDraw),
          "the batch holds exactly ONE sprite (the camera entity is not a sprite)");
    if (bytes.size() == sizeof(lux::render::SpriteDraw))
    {
        lux::render::SpriteDraw d{};
        std::memcpy(&d, bytes.data(), sizeof(d));
        check(d.tint == 0xFF00FF00u, "tint carried ECS → SpriteDraw");
        check(d.key.layer == 5, "SpriteComponent.layer → DrawOrderKey.layer");
        check(d.key.stable_id == static_cast<std::uint64_t>(entt::to_integral(spr)),
              "entity id → DrawOrderKey.stable_id (deterministic tie-break)");
        // WorldTransform2D of a sprite at (1,2), scale 1 → translation (1,2); the bridge
        // then scales column 0 by size.x=3 and column 1 by size.y=4 (column-major).
        check(std::abs(d.transform[0] - 3.f) < 1e-4f && std::abs(d.transform[5] - 4.f) < 1e-4f,
              "sprite size scales the quad basis (col0 *= w, col1 *= h)");
        check(std::abs(d.transform[12] - 1.f) < 1e-4f && std::abs(d.transform[13] - 2.f) < 1e-4f,
              "world position (1,2) carried into the transform translation");
        check(d.texture_bindless == lux::render::kNoTexture,
              "a null-texture sprite resolves to kNoTexture (tint-only, no bindless sample)");
    }

    // Pivot: a non-centred pivot shifts the quad so the pivot point sits at the world
    // origin. With pivot (0,0) and size (3,4) at (1,2), the translation offsets by
    // (col0*0.5 + col1*0.5) = (1.5, 2) → (2.5, 4). (Default pivot 0.5,0.5 = zero offset.)
    world.get<d2::SpriteComponent>(spr).pivot = Eigen::Vector2f(0.f, 0.f);
    world.tick(1.f / 60.f);
    fix.recorder().by_op.clear();
    rs.update(world.registry(), 1.f / 60.f);
    fix.roundTrip();
    if (fix.recorder().count("Canvas2DSubmit") == 1 &&
        fix.recorder().by_op.at("Canvas2DSubmit").at(0).size() == sizeof(lux::render::SpriteDraw))
    {
        lux::render::SpriteDraw dp{};
        std::memcpy(&dp, fix.recorder().by_op.at("Canvas2DSubmit").at(0).data(), sizeof(dp));
        check(std::abs(dp.transform[12] - 2.5f) < 1e-4f && std::abs(dp.transform[13] - 4.f) < 1e-4f,
              "non-centred pivot (0,0) shifts the quad so the pivot sits at the entity origin");
    }
    else
    {
        check(false, "pivot sprite still submitted after changing pivot");
    }

    // An invisible sprite is skipped → an empty batch → the proxy submits nothing.
    world.get<d2::SpriteComponent>(spr).visible = false;
    world.tick(1.f / 60.f);
    fix.recorder().by_op.clear();
    rs.update(world.registry(), 1.f / 60.f);   // frame already open (roundTrip re-opened it)
    fix.roundTrip();
    check(fix.recorder().count("Canvas2DSubmit") == 0, "an invisible sprite submits nothing");

    // Two-phase teardown (RenderableSystem contract — its dtor asserts shutdown was
    // completed after any update). The transient Sprite2DBridge has no pending work, so
    // the drain loop is empty. (A frame is already open from the last roundTrip.)
    rs.beginShutdown();
    fix.roundTrip();
    while (rs.hasPendingShutdownWork()) fix.roundTrip();
    check(rs.flushShutdownCleanup().has_value(), "flush completes the drained teardown");
    fix.roundTrip();

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}

// ============================================================================
//  sprite2d_offscreen_test.cpp — S2-03 (gpu tier): the Sprite Slice A visual gate.
//
//  graph_dump_stability_test proves submitting sprites doesn't recompile the graph;
//  this proves the sprites actually PRODUCE THE RIGHT PIXELS on a real device — the
//  "首个可见 Sprite" acceptance the checklist wants. Renders Canvas2D sprites to an
//  OFFSCREEN view through the real pipeline, reads the color back, and asserts what
//  landed where.
//
//  Framing is made deterministic with an IDENTITY view+proj (uploaded via
//  StandardViewCamera): the CPU expands each sprite onto a unit quad centred at ±0.5,
//  so with identity camera world XY == NDC — scale 2 fills the 64x64 view exactly,
//  scale 1 covers the centre half. Assertions are colour-at-pixel (centre vs corner),
//  which is symmetric so the Vulkan Y convention doesn't matter.
//
//    1. a full-screen (scale 2) opaque RED sprite   → every sampled pixel is red
//    2. a centred (scale 1) opaque GREEN sprite      → centre green, corner NOT green
//    3. an empty draw list                           → centre clears (no leftover sprite)
//
//  Uses the shared DeviceRenderFixture (no per-test server-init boilerplate).
//  Self-checking: 0 = PASS / skip (no Vulkan), 1 = FAIL.
// ============================================================================

#include "DeviceRenderFixture.hpp"

#include <lux/engine/render/renderer/features/canvas2d/Canvas2DFeatureOps.hpp>   // kCanvas2DFeatureFactory, Canvas2DProxy, Canvas2DOperationIds
#include <lux/engine/render/renderer/features/canvas2d/Canvas2DOperation.hpp>    // SpriteDraw / DrawOrderKey / Rect2D
#include <lux/engine/render/renderer/features/view_camera/ViewCameraOperation.hpp> // kStandardViewCameraFeatureFactory, ViewCameraProxy, ViewCameraOperationIds

#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

using namespace lux::render;

namespace
{
    struct EmptyConfig {};

    // Column-major identity: world XY passes straight through to NDC.
    constexpr float kIdentity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

    // Readback is BGRA8: px[i+0]=B, px[i+1]=G, px[i+2]=R, px[i+3]=A. Test opaque primaries
    // with generous thresholds so exact filtering/rounding doesn't matter.
    bool isRed  (const std::vector<std::uint8_t>& px, std::size_t i)
    { return px[i+2] > 200 && px[i+1] < 60 && px[i+0] < 60; }
    bool isGreen(const std::vector<std::uint8_t>& px, std::size_t i)
    { return px[i+1] > 200 && px[i+2] < 60 && px[i+0] < 60; }

    int g_fails = 0;
    void check(bool c, const char* m) { std::printf(c ? "  [PASS] %s\n" : "  [FAIL] %s\n", m); if (!c) ++g_fails; }
}

int main()
{
    std::setbuf(stdout, nullptr);
    std::printf("=== sprite2d_offscreen_test (S2-03) ===\n");

    constexpr std::uint32_t W = 64, H = 64;
    lux::rendertest::DeviceRenderFixture fx(W, H, "sprite2d_offscreen_test");
    if (!fx.ok()) { std::printf("No Vulkan device. Skipping.\n"); return 0; }

    const auto sv = fx.makeSceneWithView("Sprite2DScene", "main");

    // StandardViewCamera (writes the per-view view/proj the sprite shader samples) + Canvas2D.
    const auto cam_reg = fx.await(fx.session().registerFeatureType(kStandardViewCameraFeatureFactory));
    fx.await(fx.session().addFeature(sv.scene_id, cam_reg.feature_type_id, EmptyConfig{}));
    const auto canvas_reg = fx.await(fx.session().registerFeatureType(kCanvas2DFeatureFactory));
    fx.await(fx.session().addFeature(sv.scene_id, canvas_reg.feature_type_id, EmptyConfig{}));

    const auto cam_ops    = ViewCameraOperationIds::fromOps(cam_reg.ops, cam_reg.op_count);
    const auto canvas_ops = Canvas2DOperationIds::fromOps(canvas_reg.ops, canvas_reg.op_count);
    check(cam_ops.valid(),    "StandardViewCamera op ids resolved");
    check(canvas_ops.valid(), "Canvas2D op ids resolved");

    const float eye[3] = { 0.f, 0.f, 0.f };

    // Render the SAME content every frame for a few frames + the readback frame, so whatever
    // frame-in-flight slot the readback lands on holds it (Canvas2D draws are per-frame).
    auto renderAndReadback = [&](std::span<const SpriteDraw> sprites) -> std::vector<std::uint8_t>
    {
        for (int f = 0; f < 5; ++f)
        {
            ViewCameraProxy(fx.session(), cam_ops).update(sv.scene_id, sv.view, kIdentity, kIdentity, eye);
            if (!sprites.empty())
                Canvas2DProxy(fx.session(), canvas_ops).submitSprites(sv.scene_id, sprites);
            fx.flush();
        }
        // The readback frame carries the same content so its render (then copy) sees the sprite.
        ViewCameraProxy(fx.session(), cam_ops).update(sv.scene_id, sv.view, kIdentity, kIdentity, eye);
        if (!sprites.empty())
            Canvas2DProxy(fx.session(), canvas_ops).submitSprites(sv.scene_id, sprites);
        return fx.readback(sv.scene_id, sv.view);
    };

    const std::size_t center = (static_cast<std::size_t>(H/2) * W + W/2) * 4;   // pixel (32,32)
    const std::size_t corner = (static_cast<std::size_t>(2)   * W + 2)   * 4;   // pixel (2,2)

    // ── 1. full-screen (scale 2) opaque RED sprite fills the view ────────────────
    SpriteDraw full{};
    full.transform[0] = 2.f; full.transform[5] = 2.f; full.transform[10] = 1.f; full.transform[15] = 1.f;
    full.tint = 0xFF0000FFu;   // AABBGGRR: opaque red (premultiplied = itself, alpha 1)
    full.key  = DrawOrderKey{0, 0, 0, 0, 1};
    {
        const SpriteDraw one[] = { full };
        const auto px = renderAndReadback(one);
        check(fx.lastReadback().status == 0, "readback succeeded");
        check(fx.lastReadback().width == W && fx.lastReadback().height == H, "readback dimensions match the view");
        check(isRed(px, center) && isRed(px, corner), "a scale-2 sprite fills the whole view red");
    }

    // ── 2. centred (scale 1) opaque GREEN sprite covers only the centre half ─────
    SpriteDraw mid = full;
    mid.transform[0] = 1.f; mid.transform[5] = 1.f;
    mid.tint = 0xFF00FF00u;   // opaque green
    mid.key  = DrawOrderKey{0, 0, 0, 0, 2};
    {
        const SpriteDraw one[] = { mid };
        const auto px = renderAndReadback(one);
        check(isGreen(px, center), "a scale-1 sprite renders at the view centre");
        check(!isGreen(px, corner), "…and does NOT reach the corner (correct size + framing)");
    }

    // ── 3. empty draw list clears — proves the colour came from the sprite ───────
    {
        const auto px = renderAndReadback({});
        check(!isRed(px, center) && !isGreen(px, center), "an empty draw list leaves no leftover sprite");
    }

    std::printf(g_fails ? "\nFAILED (%d)\n" : "\nPASSED\n", g_fails);
    return g_fails ? 1 : 0;
}

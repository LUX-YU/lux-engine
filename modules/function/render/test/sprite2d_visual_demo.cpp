// ============================================================================
//  sprite2d_visual_demo.cpp — S2-03 VISUAL (interactive tier): actually SEE the
//  Canvas2D sprites on screen. Opens a real window, binds the view to the swapchain
//  (on-screen presentation, unlike sprite2d_offscreen_test's readback), and loops
//  rendering a set of sprites until you close the window:
//    - a row + grid of solid-colour sprites (red/green/blue/yellow/magenta/cyan/white),
//    - a premultiplied-alpha blend pair (50% red over a white base),
//    - one ANIMATED sprite that orbits + spins + pulses (proves it's live).
//
//  NOT self-checking — it's for eyeballing the 2D sprite pipeline. Registered as the
//  `interactive` tier (a window that loops), so CI's `ctest -LE interactive` skips it.
//  Skips with exit 0 when no Vulkan device is present.
//
//  Camera: view = identity, proj squashes x by 1/aspect so quads render SQUARE; the
//  world frame is x∈[-aspect,aspect], y∈[-1,1].
// ============================================================================

#include "DeviceRenderFixture.hpp"

#include <lux/engine/render/renderer/features/canvas2d/Canvas2DFeatureOps.hpp>
#include <lux/engine/render/renderer/features/canvas2d/Canvas2DOperation.hpp>
#include <lux/engine/render/renderer/features/view_camera/ViewCameraOperation.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace lux::render;

namespace
{
    struct EmptyConfig {};

    // Axis-aligned sprite: size s at (px,py), premultiplied RGBA8 tint (AABBGGRR), paint id.
    SpriteDraw sprite(float px, float py, float s, std::uint32_t tint,
                      std::uint64_t id, std::int16_t layer = 0)
    {
        SpriteDraw d{};
        d.transform[0] = s;  d.transform[5]  = s;
        d.transform[10] = 1.f; d.transform[15] = 1.f;
        d.transform[12] = px; d.transform[13] = py;
        d.tint = tint;
        d.key  = DrawOrderKey{layer, 0, 0, 0, id};
        return d;
    }

    // Rotated sprite (2D rotation baked into the column-major basis).
    SpriteDraw spriteRot(float px, float py, float s, float ang, std::uint32_t tint,
                         std::uint64_t id, std::int16_t layer = 0)
    {
        const float c = std::cos(ang), sn = std::sin(ang);
        SpriteDraw d{};
        d.transform[0] = s * c;  d.transform[1] = s * sn;    // col 0
        d.transform[4] = -s * sn; d.transform[5] = s * c;    // col 1
        d.transform[10] = 1.f; d.transform[15] = 1.f;
        d.transform[12] = px; d.transform[13] = py;
        d.tint = tint;
        d.key  = DrawOrderKey{layer, 0, 0, 0, id};
        return d;
    }
}

int main()
{
    std::setbuf(stdout, nullptr);
    std::printf("=== sprite2d_visual_demo ===  (close the window to exit)\n");

    constexpr std::uint32_t W = 960, H = 600;
    lux::rendertest::DeviceRenderFixture fx(W, H, "Sprite2D Visual Demo");
    if (!fx.ok()) { std::printf("No Vulkan device. Skipping.\n"); return 0; }

    const auto sv = fx.makeSceneWithSwapchainView("Sprite2DDemo", "main");

    const auto cam_reg = fx.await(fx.session().registerFeatureType(kStandardViewCameraFeatureFactory));
    fx.await(fx.session().addFeature(sv.scene_id, cam_reg.feature_type_id, EmptyConfig{}));
    const auto canvas_reg = fx.await(fx.session().registerFeatureType(kCanvas2DFeatureFactory));
    fx.await(fx.session().addFeature(sv.scene_id, canvas_reg.feature_type_id, EmptyConfig{}));

    const auto cam_ops    = ViewCameraOperationIds::fromOps(cam_reg.ops, cam_reg.op_count);
    const auto canvas_ops = Canvas2DOperationIds::fromOps(canvas_reg.ops, canvas_reg.op_count);

    const float aspect  = static_cast<float>(W) / static_cast<float>(H);
    const float view[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    const float proj[16] = { 1.f/aspect,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    const float eye[3]   = { 0,0,0 };

    std::printf("window up — you should see a colour grid + one orbiting sprite. Close to exit.\n");
    const auto t0 = std::chrono::steady_clock::now();

    while (fx.running())
    {
        const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();

        ViewCameraProxy(fx.session(), cam_ops).update(sv.scene_id, sv.view, view, proj, eye);

        std::vector<SpriteDraw> s;
        // top row — primaries
        s.push_back(sprite(-1.05f,  0.45f, 0.35f, 0xFF0000FFu, 1));   // red
        s.push_back(sprite(-0.35f,  0.45f, 0.35f, 0xFF00FF00u, 2));   // green
        s.push_back(sprite( 0.35f,  0.45f, 0.35f, 0xFFFF0000u, 3));   // blue
        s.push_back(sprite( 1.05f,  0.45f, 0.35f, 0xFF00FFFFu, 4));   // yellow
        // bottom row — secondaries + white
        s.push_back(sprite(-1.05f, -0.45f, 0.35f, 0xFFFF00FFu, 5));   // magenta
        s.push_back(sprite(-0.35f, -0.45f, 0.35f, 0xFFFFFF00u, 6));   // cyan
        s.push_back(sprite( 0.35f, -0.45f, 0.35f, 0xFFFFFFFFu, 7));   // white
        // premultiplied-alpha blend: 50% red drawn OVER a white base (see the tint mix)
        s.push_back(sprite( 1.05f, -0.45f, 0.35f, 0xFFFFFFFFu, 8, 0));   // white base
        s.push_back(sprite( 1.13f, -0.37f, 0.35f, 0x80000080u, 9, 1));   // 50% premul red on top
        // animated — orbits, spins, and pulses so it is obviously live
        s.push_back(spriteRot(0.55f * std::cos(t), 0.35f * std::sin(t),
                              0.22f + 0.05f * std::sin(t * 3.f), t * 1.6f,
                              0xFF20C0FFu, 100, 5));   // orange

        Canvas2DProxy(fx.session(), canvas_ops).submitSprites(sv.scene_id, s);
        fx.flush();
    }

    std::printf("window closed — bye.\n");
    return 0;
}

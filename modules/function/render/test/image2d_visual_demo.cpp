// ============================================================================
//  image2d_visual_demo.cpp — VISUAL (interactive tier): actually SEE the
//  GPU-driven Canvas2D v2 on screen. Opens a real window, binds the view to the
//  swapchain, and loops until you close it:
//    - a grid of solid-colour images (red/green/blue/yellow/magenta/cyan/white)
//      created ONCE — after creation they cost ZERO wire per frame (GPU-resident),
//    - a premultiplied-alpha blend pair (50% red over a white base — priorities
//      order them),
//    - one ANIMATED image that orbits + spins + pulses: exactly ONE transform
//      delta entry on the wire per frame (the whole delta model in one pixel).
//
//  NOT self-checking — for eyeballing the pipeline. `interactive` tier, so CI's
//  `ctest -LE interactive` skips it. Exit 0 when no Vulkan device is present.
//
//  Camera: view = identity, proj squashes x by 1/aspect so quads render SQUARE;
//  the world frame is x∈[-aspect,aspect], y∈[-1,1].
// ============================================================================

#include "DeviceRenderFixture.hpp"

#include <lux/engine/function/render/client/genops/Canvas2DOperation.ops.hpp>
#include <lux/engine/function/render/client/features/canvas2d/Canvas2DOperation.hpp>
#include <lux/engine/function/render/client/genops/ViewCameraOperation.ops.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>

using namespace lux::render;

namespace
{

    // Axis-aligned image record: size s at (px,py), premultiplied RGBA8 tint.
    Image2DInstanceData quad(float px, float py, float s, std::uint32_t tint)
    {
        Image2DInstanceData d{};
        d.m[0] = s; d.m[3] = s;
        d.m[4] = px; d.m[5] = py;
        d.tint = tint;
        return d;
    }
}

int main()
{
    std::setbuf(stdout, nullptr);
    std::printf("=== image2d_visual_demo (GPU-driven v2) ===  (close the window to exit)\n");

    constexpr std::uint32_t W = 960, H = 600;
    lux::rendertest::DeviceRenderFixture fx(W, H, "Image2D Visual Demo");
    if (!fx.ok()) { std::printf("No Vulkan device. Skipping.\n"); return 0; }

    const auto sv = fx.makeSceneWithSwapchainView("Image2DDemo", "main");

    const auto cam_reg = fx.awaitControl(fx.control().registerFeatureType(kViewCameraFeatureFactory));
    fx.awaitControl(fx.control().addFeature(sv.scene_id, cam_reg.feature_type_id, lux::render::ViewCameraCommTag{}));
    const auto canvas_reg = fx.awaitControl(fx.control().registerFeatureType(kCanvas2DFeatureFactory));
    fx.awaitControl(fx.control().addFeature(sv.scene_id, canvas_reg.feature_type_id, lux::render::Canvas2DCommConfig{}));

    const auto cam_ops    = ViewCameraOperationIds::fromOps(cam_reg.ops, cam_reg.op_count);
    const auto canvas_ops = Canvas2DOperationIds::fromOps(canvas_reg.ops, canvas_reg.op_count);
    Canvas2DProxy canvas(fx.session(), canvas_ops);

    // Static content is created ONCE — from here on it lives on the GPU and
    // costs nothing per frame (no heartbeat, no re-submit).
    // top row — primaries
    (void)addImage(canvas, sv.scene_id, quad(-1.05f,  0.45f, 0.35f, 0xFF0000FFu), 0.f);   // red
    (void)addImage(canvas, sv.scene_id, quad(-0.35f,  0.45f, 0.35f, 0xFF00FF00u), 0.f);   // green
    (void)addImage(canvas, sv.scene_id, quad( 0.35f,  0.45f, 0.35f, 0xFFFF0000u), 0.f);   // blue
    (void)addImage(canvas, sv.scene_id, quad( 1.05f,  0.45f, 0.35f, 0xFF00FFFFu), 0.f);   // yellow
    // bottom row — secondaries + white
    (void)addImage(canvas, sv.scene_id, quad(-1.05f, -0.45f, 0.35f, 0xFFFF00FFu), 0.f);   // magenta
    (void)addImage(canvas, sv.scene_id, quad(-0.35f, -0.45f, 0.35f, 0xFFFFFF00u), 0.f);   // cyan
    (void)addImage(canvas, sv.scene_id, quad( 0.35f, -0.45f, 0.35f, 0xFFFFFFFFu), 0.f);   // white
    // premultiplied-alpha blend: 50% red drawn OVER a white base (priority orders them)
    (void)addImage(canvas, sv.scene_id, quad( 1.05f, -0.45f, 0.35f, 0xFFFFFFFFu), 0.f);   // white base
    (void)addImage(canvas, sv.scene_id, quad( 1.13f, -0.37f, 0.35f, 0x80000080u), 1.f);   // 50% premul red on top

    // The animated image — the only per-frame wire traffic in this demo.
    const auto orbiter = fx.await(addImage(canvas,
        sv.scene_id, quad(0.55f, 0.f, 0.22f, 0xFF20C0FFu), /*priority=*/5.f));
    if (orbiter.status != ECanvas2DCreateStatus::Ok)
    { std::printf("addImage failed (%u)\n", static_cast<unsigned>(orbiter.status)); return 1; }

    const float aspect  = static_cast<float>(W) / static_cast<float>(H);
    const float view[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    const float proj[16] = { 1.f/aspect,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    const float eye[3]   = { 0,0,0 };

    std::printf("window up — colour grid (static, zero wire) + one orbiting image "
                "(one transform delta per frame). Close to exit.\n");
    const auto t0 = std::chrono::steady_clock::now();

    while (fx.running())
    {
        const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();

        viewCameraUpdateTransient(ViewCameraProxy(fx.session(), cam_ops), sv.scene_id, sv.view, view, proj, eye);

        // Orbit + spin + pulse, all baked into the 6-float affine: ONE delta entry.
        const float s = 0.22f + 0.05f * std::sin(t * 3.f);
        const float c = std::cos(t * 1.6f), sn = std::sin(t * 1.6f);
        const float m[6] = { s * c, s * sn, -s * sn, s * c,
                             0.55f * std::cos(t), 0.35f * std::sin(t) };
        const std::int32_t page_delta[2]{0, 0};
        updateTransform(
            canvas,
            sv.scene_id,
            orbiter.handle,
            m,
            page_delta);

        fx.flush();
    }
    return 0;
}

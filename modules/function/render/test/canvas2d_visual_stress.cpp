// ============================================================================
//  canvas2d_visual_stress.cpp — VISUAL (interactive tier): the GPU-driven
//  Canvas2D v2 under load, ON SCREEN (the 2D counterpart of deferred_stress_test).
//
//  What you see / what it proves:
//    - 50,000 GPU-RESIDENT images (a rainbow galaxy disc) created ONCE — after
//      creation they cost ZERO wire per frame;
//    - the camera orbits + zooms the whole time: the ENTIRE field moves with
//      ONE camera op per frame (nothing per-image — the GPU-driven money shot);
//    - a ring of 1,024 DYNAMIC images swirls on top: exactly one 1,024-entry
//      transform bulk per frame (wire ∝ change, ~40 KB);
//    - every ~2 s a colour band's PRIORITY flips: the layer visibly pops in
//      front/behind — a full 50k order rebuild, done in ~0.1 ms server-side;
//    - the console prints fps + live/dynamic counts once per second.
//
//  NOT self-checking — for eyeballing. `interactive` tier (ctest -LE interactive
//  skips it). Exit 0 when no Vulkan device is present.
// ============================================================================

#include "DeviceRenderFixture.hpp"

#include <lux/engine/function/render/client/genops/Canvas2DOperation.ops.hpp>
#include <lux/engine/function/render/client/features/canvas2d/Canvas2DOperation.hpp>
#include <lux/engine/function/render/client/genops/ViewCameraOperation.ops.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace lux::render;

namespace
{

    constexpr std::uint32_t W = 1280, H = 800;
    constexpr std::uint32_t kStaticImages  = 50'000;
    constexpr std::uint32_t kDynamicImages = 1'024;   // one bulk/frame, inside the ring budget
    constexpr std::uint32_t kWaveBand       = 512;     // priority-flip band per wave
    constexpr float         kPi             = 3.14159265f;

    // Cheap HSV(h,1,1) → premultiplied opaque RGBA8 (AABBGGRR byte order R,G,B,A).
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

int main()
{
    std::setbuf(stdout, nullptr);
    std::printf("=== canvas2d_visual_stress (GPU-driven v2) ===  (close the window to exit)\n");

    lux::rendertest::DeviceRenderFixture fx(W, H, "Canvas2D Visual Stress — 50k GPU-resident images");
    if (!fx.ok()) { std::printf("No Vulkan device. Skipping.\n"); return 0; }

    const auto sv = fx.makeSceneWithSwapchainView("Canvas2DVisualStress", "main");

    const auto cam_reg = fx.awaitControl(fx.control().registerFeatureType(kViewCameraFeatureFactory));
    fx.awaitControl(fx.control().addFeature(sv.scene_id, cam_reg.feature_type_id, lux::render::ViewCameraCommTag{}));
    const auto canvas_reg = fx.awaitControl(fx.control().registerFeatureType(kCanvas2DFeatureFactory));
    fx.awaitControl(fx.control().addFeature(sv.scene_id, canvas_reg.feature_type_id, lux::render::Canvas2DCommConfig{}));

    const auto cam_ops    = ViewCameraOperationIds::fromOps(cam_reg.ops, cam_reg.op_count);
    const auto canvas_ops = Canvas2DOperationIds::fromOps(canvas_reg.ops, canvas_reg.op_count);
    Canvas2DProxy canvas(fx.session(), canvas_ops);

    // ── Create the static galaxy ONCE (sunflower spiral, rainbow by angle) ──
    // Priorities cycle 0..5 by radial band, so the wave flip below visibly pops
    // one band above its neighbours.
    std::printf("creating %u static images...\n", kStaticImages);
    std::vector<Image2DHandle> band_handles;   // the band whose priority the wave flips
    band_handles.reserve(kWaveBand);
    {
        std::uint32_t created = 0, failed = 0;
        for (std::uint32_t i = 0; i < kStaticImages; ++i)
        {
            const float k = static_cast<float>(i);
            const float r = 0.95f * std::sqrt(k / kStaticImages);
            const float a = k * 2.39996f;   // golden angle
            Image2DInstanceData d{};
            const float s = 0.0035f + 0.004f * (1.f - r);
            d.m[0] = s; d.m[3] = s;
            d.m[4] = r * std::cos(a);
            d.m[5] = r * std::sin(a);
            d.tint = rainbow(a / (2.f * kPi));
            const float prio = static_cast<float>(i % 6);
            auto req = addImage(canvas, sv.scene_id, d, prio);
            if (band_handles.size() < kWaveBand && (i % 97u) == 0u)
                req.then([&band_handles](const Image2DSlotReply& rep)
                {
                    if (rep.status == ECanvas2DCreateStatus::Ok) band_handles.push_back(rep.handle);
                });
            ++created;
            (void)failed;
            if ((i % 512u) == 511u) fx.flush();   // keep the frame ring drained
        }
        fx.flush(4);
        std::printf("created %u (band tracked: %u)\n", created,
                    static_cast<unsigned>(band_handles.size()));
    }

    // ── The dynamic swirl ring ──
    std::vector<Image2DHandle> dyn;
    dyn.reserve(kDynamicImages);
    for (std::uint32_t i = 0; i < kDynamicImages; ++i)
    {
        Image2DInstanceData d{};
        d.m[0] = 0.012f; d.m[3] = 0.012f;
        d.tint = 0xFFFFFFFFu;
        auto req = addImage(canvas, sv.scene_id, d, /*priority=*/10.f);   // always on top
        req.then([&dyn](const Image2DSlotReply& r)
        {
            if (r.status == ECanvas2DCreateStatus::Ok) dyn.push_back(r.handle);
        });
        if ((i % 512u) == 511u) fx.flush();
    }
    for (int i = 0; i < 16 && dyn.size() < kDynamicImages; ++i) fx.flush();
    std::printf("dynamic ring: %u images — window up. Close to exit.\n",
                static_cast<unsigned>(dyn.size()));

    const float aspect = static_cast<float>(W) / static_cast<float>(H);
    const float eye[3] = {0, 0, 0};
    std::vector<Image2DTransformEntry> batch(dyn.size());

    const auto t0 = std::chrono::steady_clock::now();
    auto  fps_mark   = t0;
    int   fps_frames = 0;
    int   frame_no   = 0;
    bool  wave_up    = false;

    while (fx.running())
    {
        const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();

        // ── ONE camera op moves all 50k images: orbit + breathe-zoom ──
        const float zoom = 1.35f + 0.85f * std::sin(t * 0.35f);
        const float cx   = 0.30f * std::cos(t * 0.22f);
        const float cy   = 0.30f * std::sin(t * 0.17f);
        const float view[16] = { zoom,0,0,0,  0,zoom,0,0,  0,0,1,0,  -cx*zoom, -cy*zoom, 0, 1 };
        const float proj[16] = { 1.f/aspect,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,0,1 };
        viewCameraUpdateTransient(ViewCameraProxy(fx.session(), cam_ops), sv.scene_id, sv.view, view, proj, eye);

        // ── ONE bulk updates the swirl ring (wire ∝ change) ──
        for (std::size_t i = 0; i < dyn.size(); ++i)
        {
            const float a = t * 0.9f + static_cast<float>(i) * (2.f * kPi / dyn.size());
            const float r = 0.55f + 0.25f * std::sin(t * 0.7f + i * 0.02f);
            auto& e  = batch[i];
            e.scene  = sv.scene_id;
            e.handle = dyn[i];
            const float s = 0.010f + 0.006f * std::sin(t * 2.f + i * 0.1f);
            const float c = std::cos(a * 3.f), sn = std::sin(a * 3.f);
            e.m[0] = s * c; e.m[1] = s * sn; e.m[2] = -s * sn; e.m[3] = s * c;
            e.m[4] = r * std::cos(a);
            e.m[5] = r * std::sin(a);
        }
        updateTransforms(canvas, batch);

        // ── Every ~2 s: flip the tracked band's priority → visible layer pop
        //    (a full-arena order rebuild, ~0.1 ms server-side) ──
        if ((++frame_no % 120) == 0)
        {
            wave_up = !wave_up;
            const float prio = wave_up ? 8.f : 1.f;
            for (const auto& h : band_handles)
                updateKey(canvas, sv.scene_id, h, prio, true);
        }

        fx.flush();

        ++fps_frames;
        const auto now = std::chrono::steady_clock::now();
        if (now - fps_mark >= std::chrono::seconds(1))
        {
            std::printf("fps=%d | static=%u (zero wire) | dynamic=%u (one bulk ≈ %u KB) | wave=%s\n",
                        fps_frames, kStaticImages, static_cast<unsigned>(dyn.size()),
                        static_cast<unsigned>(dyn.size() * sizeof(Image2DTransformEntry) / 1024),
                        wave_up ? "up" : "down");
            fps_frames = 0;
            fps_mark   = now;
        }
    }
    return 0;
}

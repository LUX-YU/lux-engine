// ============================================================================
//  canvas2d_stress_test.cpp — PERMANENT (gpu tier): the GPU-driven Canvas2D v2
//  under load. Numbers over feelings:
//    A  bulk creation      — N sprites created through the command channel
//    B  static frames      — the v2 core claim: NO canvas commands at all are
//                            issued client-side, the server re-draws GPU-resident
//                            data (frame time = pure pipeline cost)
//    C  dynamic frames     — K dirty sprites per frame ride ONE transform bulk
//                            (wire ∝ change; K is bounded by the frame-ring
//                            payload budget, ~1.6k entries — same ceiling the 3D
//                            TransformBatch shares)
//    D  key churn          — priority rewrites on K sprites per frame force an
//                            order rebuild every frame (the worst case v2 pays)
//    E  churn storm        — remove/re-create waves (slot recycling + growth)
//  Self-checking for CORRECTNESS (creation Ok, pixels drawn, churn sane);
//  timings are PRINTED for the log, not asserted (machine-dependent).
//  Skips with exit 0 when no Vulkan device is present.
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

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                            \
        }                                                                        \
    } while (0)

namespace
{
    struct EmptyConfig {};

    constexpr std::uint32_t W = 256, H = 256;
    constexpr std::uint32_t kSprites       = 20'000;   // fits the default 65536 arena ceiling
    constexpr std::uint32_t kDirtyPerFrame = 1'024;    // ~40 KiB/frame bulk — inside the ring budget
    constexpr int           kStaticFrames  = 120;
    constexpr int           kDynamicFrames = 120;
    constexpr int           kChurnFrames   = 60;

    double msSince(std::chrono::steady_clock::time_point t0)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
}

int main()
{
    std::setbuf(stdout, nullptr);
    std::printf("=== canvas2d_stress_test (GPU-driven v2) ===\n");

    lux::rendertest::DeviceRenderFixture fx(W, H, "canvas2d_stress");
    if (!fx.ok()) { std::puts("No Vulkan device. Skipping."); return 0; }

    const auto sv = fx.makeSceneWithView("Canvas2DStress", "main");
    const auto cam_reg = fx.await(fx.session().registerFeatureType(kStandardViewCameraFeatureFactory));
    fx.await(fx.session().addFeature(sv.scene_id, cam_reg.feature_type_id, EmptyConfig{}));
    const auto canvas_reg = fx.await(fx.session().registerFeatureType(kCanvas2DFeatureFactory));
    fx.await(fx.session().addFeature(sv.scene_id, canvas_reg.feature_type_id, EmptyConfig{}));

    const auto cam_ops    = ViewCameraOperationIds::fromOps(cam_reg.ops, cam_reg.op_count);
    const auto canvas_ops = Canvas2DOperationIds::fromOps(canvas_reg.ops, canvas_reg.op_count);
    Canvas2DProxy canvas(fx.session(), canvas_ops);

    const float view[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    const float proj[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    const float eye[3]   = {0,0,0};
    const auto frame = [&] {
        ViewCameraProxy(fx.session(), cam_ops).update(sv.scene_id, sv.view, view, proj, eye);
        fx.flush();
    };

    // ── A: bulk creation (handles collected via non-blocking continuations) ──
    std::vector<Sprite2DHandle> handles;
    handles.reserve(kSprites);
    std::uint32_t failed = 0;
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (std::uint32_t i = 0; i < kSprites; ++i)
        {
            Sprite2DInstanceData d{};
            const float fx01 = static_cast<float>(i % 141) / 141.f;
            const float fy01 = static_cast<float>((i / 141) % 141) / 141.f;
            d.m[0] = 0.01f; d.m[3] = 0.01f;
            d.m[4] = fx01 * 2.f - 1.f;
            d.m[5] = fy01 * 2.f - 1.f;
            d.tint = 0xFF000000u | (0xFFu << ((i % 3) * 8));   // r/g/b extremes
            // Fire-and-collect: the continuation lives in the reply store, so the
            // local request handle can simply go out of scope (dtor does NOT cancel).
            auto req = canvas.addSprite(sv.scene_id, d, /*priority=*/static_cast<float>(i % 7));
            req.then([&handles, &failed](const Sprite2DSlotReply& r)
            {
                if (r.status == ECanvas2DCreateStatus::Ok && r.handle.valid())
                    handles.push_back(r.handle);
                else
                    ++failed;
            });
            if ((i % 512u) == 511u) frame(); // keep the frame ring drained
        }
        for (int i = 0; i < 32 && handles.size() + failed < kSprites; ++i) frame();
        std::printf("[A] created %u sprites in %.1f ms (failed=%u)\n",
                    static_cast<unsigned>(handles.size()), msSince(t0), failed);
    }
    CHECK(failed == 0);
    CHECK(handles.size() == kSprites);

    // Something is actually on screen.
    {
        frame();
        const auto img = fx.readback(sv.scene_id, sv.view);
        std::uint32_t lit = 0;
        for (std::size_t i = 0; i < img.size(); i += 4)
            lit += (img[i] | img[i + 1] | img[i + 2]) ? 1u : 0u;
        std::printf("[A] lit pixels: %u / %u\n", lit, W * H);
        CHECK(lit > 1000);
    }

    // ── B: static frames — client issues ZERO canvas commands ──
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kStaticFrames; ++i) frame();
        std::printf("[B] static  : %.3f ms/frame (%d frames, %u GPU-resident sprites)\n",
                    msSince(t0) / kStaticFrames, kStaticFrames, kSprites);
    }

    // ── C: dynamic frames — kDirtyPerFrame transform deltas in ONE bulk ──
    {
        std::vector<Sprite2DTransformEntry> batch(kDirtyPerFrame);
        const auto t0 = std::chrono::steady_clock::now();
        for (int f = 0; f < kDynamicFrames; ++f)
        {
            const float phase = static_cast<float>(f) * 0.05f;
            for (std::uint32_t i = 0; i < kDirtyPerFrame; ++i)
            {
                const auto& h = handles[(static_cast<std::size_t>(f) * kDirtyPerFrame + i) % handles.size()];
                auto& e  = batch[i];
                e.scene  = sv.scene_id;
                e.handle = h;
                e.m[0] = 0.01f; e.m[1] = 0.f; e.m[2] = 0.f; e.m[3] = 0.01f;
                e.m[4] = std::cos(phase + i * 0.006f);
                e.m[5] = std::sin(phase + i * 0.006f);
            }
            canvas.updateTransforms(batch);
            frame();
        }
        std::printf("[C] dynamic : %.3f ms/frame (%u dirty/frame, one bulk — wire ∝ change)\n",
                    msSince(t0) / kDynamicFrames, kDirtyPerFrame);
    }

    // ── D: key churn — an order rebuild EVERY frame (v2's worst case) ──
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (int f = 0; f < kChurnFrames; ++f)
        {
            for (std::uint32_t i = 0; i < kDirtyPerFrame; ++i)
                canvas.updateKey(sv.scene_id, handles[i], static_cast<float>((f + i) % 13), true);
            frame();
        }
        std::printf("[D] key churn: %.3f ms/frame (%u key writes/frame → full order rebuild of %u)\n",
                    msSince(t0) / kChurnFrames, kDirtyPerFrame, kSprites);
    }

    // ── E: churn storm — remove + re-create waves (recycling + growth paths) ──
    {
        const auto t0 = std::chrono::steady_clock::now();
        std::uint32_t wave_failed = 0, wave_created = 0;
        for (int wave = 0; wave < 4; ++wave)
        {
            for (std::uint32_t i = 0; i < 2'000; ++i)
                canvas.removeSprite(sv.scene_id, handles[wave * 2'000 + i]);
            frame();
            for (std::uint32_t i = 0; i < 2'000; ++i)
            {
                Sprite2DInstanceData d{};
                d.m[0] = 0.01f; d.m[3] = 0.01f;
                d.m[4] = 0.f; d.m[5] = 0.f;
                auto req = canvas.addSprite(sv.scene_id, d, 1.f);
                req.then([&](const Sprite2DSlotReply& r)
                {
                    if (r.status == ECanvas2DCreateStatus::Ok) ++wave_created; else ++wave_failed;
                });
                if ((i % 512u) == 511u) frame();
            }
            frame();
        }
        for (int i = 0; i < 32 && wave_created + wave_failed < 8'000; ++i) frame();
        std::printf("[E] churn   : 4 waves of 2000 remove + 2000 create in %.1f ms (failed=%u)\n",
                    msSince(t0), wave_failed);
        CHECK(wave_failed == 0);
        CHECK(wave_created == 8'000);
    }

    std::puts("canvas2d_stress_test: all checks passed");
    return 0;
}

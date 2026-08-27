// ============================================================================
//  scene_cycle_stress_test.cpp — PERMANENT (gpu tier): the scene-lifecycle gate
//  for the editor architecture v2 "scene domain" model (ADR: editor architecture v2 §4b C1).
//
//  Proves — with the validation layer ON and turned into a hard assertion —
//  that a full editor-shaped scene (the SAME 17-feature set the editor
//  attaches) can be created and destroyed in a loop without leaking:
//
//    per cycle:  createScene → setActiveScene → addView → addFeature ×17
//                → a few rendered frames → destroyScene → drain frames
//
//  destroyScene retires asynchronously (generation-bumped id + fif-deferred
//  shutdownFull), so the drain frames after each destroy let reclamation run
//  under load, and the post-loop drain lets the tail settle. Any leaked
//  Vulkan object is reported by the validation layer at instance/device
//  destroy — the error counter is asserted AFTER the fixture is gone, so
//  those end-of-life reports count too.
//
//  This is the regression net for C2/C3 (editor switches to per-scene
//  create/destroy and the drain-protocol compensation machinery is deleted)
//  and the mechanised half of C12's feature-detach residue audit.
//
//  Skips with exit 0 when no Vulkan device (or no validation layer) is
//  available — never fails a GPU-less CI box.
// ============================================================================

#include "DeviceRenderFixture.hpp"

#include <lux/engine/function/render/client/genops/ViewCameraOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/LightOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MaterialOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/SkinningOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ShadowMapOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshShadowOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/DeferredGBufferOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/DeferredLightingOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/SkyboxOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/TonemapOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/FogOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/WaterOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/Grid3DOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/LineListOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/HzbOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/HighlightOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/SpatialCullOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/LinearDepthOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/SsaoOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/Canvas2DOperation.ops.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

using namespace lux::render;

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                       \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

namespace
{

    constexpr std::uint32_t W = 64, H = 64;
    constexpr int kCycles = 100;
    constexpr int kFramesPerCycle = 3; // render with the scene alive
    constexpr int kDrainPerCycle = 4;  // > frames_in_flight → retirement collects under load
    constexpr int kFinalDrain = 8;

    double msSince(std::chrono::steady_clock::time_point t0)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
}

int
main(int argc, char** argv)
{
    std::setbuf(stdout, nullptr);

    // Diagnostic knobs (defaults = the full gate):
    //   --cycles=N     loop count (default 100)
    //   --no-destroy   skip destroyScene — isolates bring-up + frame-loop
    //                  validation debt from destroy-path bugs (baseline runs)
    //   --mobile       request EFeatureLevel::Mobile. The server resolves
    //                  min(achievable, requested), so this genuinely runs the
    //                  Mobile tier on a desktop box — which is what makes the
    //                  descriptor-budget gate below mean something before any
    //                  phone exists. Registered as a second ctest entry so both
    //                  tiers are covered.
    int cycles = kCycles;
    bool do_destroy = true;
    bool mobile = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::sscanf(argv[i], "--cycles=%d", &cycles) == 1)
            continue;
        if (std::strcmp(argv[i], "--no-destroy") == 0)
            do_destroy = false;
        if (std::strcmp(argv[i], "--mobile") == 0)
            mobile = true;
    }

    std::printf("=== scene_cycle_stress_test (editor feature set, validation asserted) ===\n");
    std::printf("cycles=%d destroy=%d tier=%s\n", cycles, int(do_destroy), mobile ? "Mobile" : "Desktop");

    static std::atomic<int> validation_errors{0};

    {
        lux::rendertest::DeviceRenderFixture fx(
            W,
            H,
            "scene_cycle_stress",
            {.enable_validation = true,
             .validation_errors = &validation_errors,
             .preferred_level = mobile ? lux::render::EFeatureLevel::Mobile : lux::render::EFeatureLevel::Desktop}
        );
        if (!fx.ok())
        {
            std::puts("No Vulkan device / validation layer. Skipping.");
            return 0;
        }

        // Assert the tier we ASKED for is the tier we GOT. A passing run proves
        // nothing on its own: the resolved level only gates feature admission
        // (RenderScene checks each feature's level_profiles row), it does not
        // select implementation variants today — so a --mobile run that
        // silently stayed on Desktop would look exactly like a successful one.
        {
            lux::render::DeviceCaps caps{};
            const auto cr = fx.awaitControl(fx.control().queryDeviceCaps(caps));
            const uint32_t expected = static_cast<uint32_t>(
                mobile ? lux::render::EFeatureLevel::Mobile : lux::render::EFeatureLevel::Desktop
            );
            std::printf("resolved feature_level=%u (0=Mobile 1=MobileHigh 2=Desktop)\n", cr.feature_level);
            if (cr.feature_level != expected)
            {
                std::fprintf(stderr, "FAIL: asked for tier %u, got %u\n", expected, cr.feature_level);
                return 1;
            }
        }

        // ── Feature TYPE registration: once, process-scoped (mirrors the v2
        //    two-domain split — types are per-process, instances per-scene). ──
        const auto reg = [&](const FeatureFactory& f) {
            const auto r = fx.awaitControl(fx.control().registerFeatureType(f));
            return r.feature_type_id;
        };

        // Configs mirror the standard feature plan's editor set 1:1.
        ShadowMapCommConfig shmap_cfg{};
        shmap_cfg.enable_directional_csm = 1u;
        shmap_cfg.non_directional_shadow_max_distance = 0.0f;

        MeshShadowCommConfig mshsw_cfg{};
        mshsw_cfg.comm_config_version = kMeshShadowCommConfigVersion;
        mshsw_cfg.descriptor_layout_version = kMeshShadowDescriptorLayoutVersion;

        DeferredGBufferCommConfig gbuf_cfg{};
        gbuf_cfg.comm_config_version = kDeferredGBufferCommConfigVersion;
        gbuf_cfg.descriptor_layout_version = kDeferredGBufferDescriptorLayoutVersion;
        gbuf_cfg.extension_flags |= EGpuDrivenMeshExt::HZB;

        DeferredLightingCommConfig lit_cfg{};
        // 压测钉住 SAMPLED 路径(独立 pass,与 local_read 合并作用域互补的
        // 覆盖面);cluster 参数走 comm 默认。
        lit_cfg.read_mode = ELightingReadMode::SAMPLED;
        lit_cfg.enable_clustered = 1;

        SkyboxCommConfig sky_cfg{};
        TonemapCommConfig tm_cfg{.tone_map_op = ETonemapOperator::ACES_FILMIC, .exposure = 1.0f, .gamma = 2.2f};
        Grid3DCommConfig grid_cfg{};
        LineListTransientCommConfig line_cfg{};
        HighlightCommConfig hl_cfg{};

        // (type id, add-feature thunk) — attach ORDER matches the orchestrator.
        struct Attach
        {
            const char* name;
            std::uint32_t type_id;
            std::function<RenderRequest<FeatureAddedReply>(RenderSceneId)> add;
        };
        auto& s = fx.control();
        std::vector<Attach> attach;
        const auto mk = [&](const char* name, const FeatureFactory& f, auto cfg) {
            const auto id = reg(f);
            CHECK(id != 0);
            attach.push_back({name, id, [&s, id, cfg](RenderSceneId scene) { return s.addFeature(scene, id, cfg); }});
            return 0;
        };
        CHECK(0 == mk("StandardViewCamera", kViewCameraFeatureFactory, lux::render::ViewCameraCommTag{}));
        CHECK(0 == mk("Light", kLightFeatureFactory, lux::render::LightCommTag{}));
        CHECK(0 == mk("StandardMaterial", kMaterialFeatureFactory, lux::render::MaterialCommTag{}));
        CHECK(0 == mk("StandardMeshStack", kMeshStackFeatureFactory, lux::render::MeshStackCommTag{}));
        CHECK(0 == mk("Skinning", kSkinningFeatureFactory, lux::render::SkinningCommConfig{}));
        CHECK(0 == mk("ShadowMap", kShadowMapFeatureFactory, shmap_cfg));
        CHECK(0 == mk("MeshShadow", kMeshShadowFeatureFactory, mshsw_cfg));
        CHECK(0 == mk("DeferredGBuffer", kDeferredGBufferFeatureFactory, gbuf_cfg));
        CHECK(0 == mk("DeferredLighting", kDeferredLightingFeatureFactory, lit_cfg));
        CHECK(0 == mk("Skybox", kSkyboxFeatureFactory, sky_cfg));
        CHECK(0 == mk("LinearDepth", kLinearDepthFeatureFactory, LinearDepthCommConfig{}));
        CHECK(0 == mk("Fog", kFogFeatureFactory, FogCommConfig{}));
        CHECK(0 == mk("Water", kWaterFeatureFactory, WaterCommConfig{}));
        CHECK(0 == mk("Tonemap", kTonemapFeatureFactory, tm_cfg));
        CHECK(0 == mk("Grid3DPass", kGrid3DFeatureFactory, grid_cfg));
        CHECK(0 == mk("LineListTransient", kLineListFeatureFactory, line_cfg));
        CHECK(0 == mk("Hzb", kHzbFeatureFactory, lux::render::HzbCommTag{}));
        CHECK(0 == mk("Highlight", kHighlightFeatureFactory, hl_cfg));
        CHECK(0 == mk("SpatialCull", kSpatialCullFeatureFactory, lux::render::SpatialCullCommConfig{}));
        // R5-1 着色输入槽通路:LinearDepth 产出 → Ssao 消费并发布 b11。
        CHECK(0 == mk("Ssao", kSsaoFeatureFactory, lux::render::SsaoCommTag{}));
        CHECK(0 == mk("Canvas2D", kCanvas2DFeatureFactory, lux::render::Canvas2DCommConfig{}));
        std::printf("registered %zu feature types\n", attach.size());

        const auto cam_reg = fx.awaitControl(fx.control().registerFeatureType(kViewCameraFeatureFactory));
        const auto cam_ops = ViewCameraOperationIds::fromOps(cam_reg.ops, cam_reg.op_count);
        const float view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        const float proj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        const float eye[3] = {0, 0, -3};

        // ── The cycle loop ──
        const auto t0 = std::chrono::steady_clock::now();
        for (int cycle = 0; cycle < cycles; ++cycle)
        {
            const auto sv = fx.makeSceneWithView("CycleScene", "cycle_view");
            CHECK(!sv.scene_id.isNull());

            for (auto& a : attach)
            {
                const auto r = fx.awaitControl(a.add(sv.scene_id));
                if (!r.feature.isValid())
                {
                    const auto message = lux::render::formatRenderError(lux::render::renderErrorRegistry(), r.error);
                    std::fprintf(
                        stderr,
                        "FAIL cycle %d: addFeature(%s) rejected: %s\n",
                        cycle,
                        a.name,
                        message.c_str()
                    );
                    return 1;
                }
            }
            for (int f = 0; f < kFramesPerCycle; ++f)
            {
                viewCameraUpdateTransient(
                    ViewCameraProxy(fx.session(), cam_ops),
                    sv.scene_id,
                    sv.view,
                    view,
                    proj,
                    eye
                );
                fx.flush();
            }

            if (cycle == 0)
            {
                // Reuse every FIF slot so the oldest query range is fence-
                // retired and adopted without a wait, then verify the control
                // plane exposes an immutable JSON snapshot.
                fx.flush(4);
                std::string timing(128u * 1024u, '\0');
                const auto reply =
                    fx.awaitControl(fx.control().queryGpuTiming(sv.scene_id, timing.data(), timing.size()));
                const std::string_view json{timing.data(), std::min<std::size_t>(reply.written, timing.size())};
                CHECK(reply.status == 0u);
                CHECK(json.find("\"version\":2") != std::string_view::npos);
                CHECK(json.find("\"views\":[{") != std::string_view::npos);
                CHECK(json.find("\"available\":") != std::string_view::npos);
                if (json.find("\"available\":true") != std::string_view::npos)
                {
                    CHECK(json.find("\"passes\":[{") != std::string_view::npos);
                }
            }

            // Every 10th cycle: prove the scene actually renders (readback Ok).
            if (cycle % 10 == 0)
            {
                const auto px = fx.readback(sv);
                CHECK(fx.lastReadback().bytes_written == px.size());
            }

            // ── Descriptor-set budget gate ──
            // maxBoundDescriptorSets is 4 on Mali and 32 on a desktop part, so
            // PipelineLayoutService's runtime check cannot catch a regression
            // here: it compares against the LOCAL device and would need 33 sets
            // to fire. The graph dump carries the plan's own projection, which
            // is measured against Vulkan's REQUIRED MINIMUM instead — so the
            // over-budget marker appearing in this text is the portable
            // failure, reproducible on a desktop box.
            //
            // Measured 2026-07-31, --mobile, this 19-feature editor-shaped
            // scene: widest pipeline binds 4 sets; projection 4 optimistic /
            // 4 conservative; merge cost zero (worst FRAGMENT stage is 7
            // descriptors before and after). Already inside the budget, with
            // feature-owned sets NOT folded into the scene-level domain set —
            // i.e. it fits without taking on that lifetime coupling. This gate
            // exists to keep it that way.
            if (cycle == 0)
            {
                std::string buf(512 * 1024, '\0');
                const auto rep = fx.awaitControl(fx.control().dumpRenderGraph(sv.scene_id, buf.data(), buf.size()));
                const std::string_view dump{buf.data(), std::min<std::size_t>(rep.written, buf.size())};
                // Prove the gate can actually fail before trusting it to pass:
                // an empty or truncated dump makes the find() below vacuous, so
                // a broken dump path would read as "budget fine" forever.
                // Anchor on the section header the check depends on.
                if (dump.find("最宽管线 set 数") == std::string_view::npos)
                {
                    std::fprintf(
                        stderr,
                        "FAIL: graph dump missing the layout-plan section "
                        "(needed=%u written=%u status=%d) — the budget gate below "
                        "would pass vacuously.\n",
                        rep.needed,
                        rep.written,
                        rep.status
                    );
                    return 1;
                }
                if (dump.find("超预算") != std::string_view::npos)
                {
                    std::fprintf(stderr, "FAIL: a pipeline exceeds the portable descriptor-set budget.\n");
                    // Only on failure — the full dump is ~1200 lines.
                    std::fwrite(dump.data(), 1, dump.size(), stderr);
                    return 1;
                }
            }

            if (do_destroy)
            {
                (void)fx.control().destroyScene(sv.scene_id);
                fx.flush(kDrainPerCycle); // let fif-deferred shutdownFull reclaim under load
            }

            if (cycle % 10 == 9 || cycle + 1 == cycles)
                std::printf(
                    "  cycle %3d/%d  (%.1f ms avg, validation errors so far: %d)\n",
                    cycle + 1,
                    cycles,
                    msSince(t0) / (cycle + 1),
                    validation_errors.load(std::memory_order_relaxed)
                );
        }

        fx.flush(kFinalDrain); // settle the tail before device teardown
        std::printf("total: %.1f ms for %d cycles\n", msSince(t0), cycles);
    } // ← fixture dies here: instance/device destroy emits leak reports, counted above

    const int errs = validation_errors.load(std::memory_order_relaxed);
    std::printf("validation errors (incl. destroy-time leak reports): %d\n", errs);
    CHECK(errs == 0);

    std::puts("scene_cycle_stress_test PASSED");
    return 0;
}

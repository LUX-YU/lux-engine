#include "app/RenderThreadOrchestrator.hpp"

#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/renderer/features/deffer/DeferredGBufferOperation.hpp>
#include <lux/engine/render/renderer/features/hzb/HzbOperation.hpp>
#include <lux/engine/render/renderer/features/deffer/DeferredLightingOperation.hpp>
#include <lux/engine/render/renderer/features/gizmo/LineListOperation.hpp>
#include <lux/engine/render/renderer/features/postprocess/TonemapOperation.hpp>
#include <lux/engine/render/renderer/features/highlight/HighlightOperation.hpp>
#include <lux/engine/render/renderer/features/spatialcull/SpatialCullOperation.hpp>
#include <lux/engine/render/renderer/features/meshstack/MeshStackOperation.hpp>
#include <lux/engine/render/renderer/features/material/MaterialOperation.hpp>
#include <lux/engine/render/renderer/features/view_camera/ViewCameraOperation.hpp>
#include <lux/engine/render/renderer/features/shadow/MeshShadowOperation.hpp>
#include <lux/engine/render/renderer/features/shadow/ShadowMapOperation.hpp>
#include <lux/engine/render/renderer/features/sky_box/SkyboxOperation.hpp>
#include <lux/engine/render/renderer/features/skinning/SkinningOperation.hpp>
#include <lux/engine/render/renderer/features/light/LightOperation.hpp>
#include <lux/engine/render/comm/server/FeatureRegistry.hpp>   // name-keyed feature op store
#include <lux/engine/ui/UiAppHost.hpp>                          // bringUpUIRenderServer

#include <chrono>
#include <cstdio>
#include <span>
#include <thread>
#include <utility>

namespace lux::editor
{
    namespace
    {
        // Poll slice while waiting for the render server to come up / fail.
        constexpr auto kServerWaitSlice = std::chrono::milliseconds(5);
    } // namespace

    RenderThreadOrchestrator::RenderThreadOrchestrator(
        lux::window::LuxWindow&      window,
        const EditorConfig&          config,
        EditorRenderInfra&           render_infra_out,
        lux::ui::ImGuiOperationIds&  imgui_ops_out) noexcept
        : window_(window)
        , config_(config)
        , render_infra_(render_infra_out)
        , imgui_ops_(imgui_ops_out)
    {
    }

    RenderThreadOrchestrator::~RenderThreadOrchestrator()
    {
        // Safety net: ensure the thread is stopped + joined even if the editor
        // never called requestStop() (idempotent — a normal shutdown calls it
        // mid-teardown, where this is a no-op).
        requestStop();
    }

    bool RenderThreadOrchestrator::start()
    {
        channel_ = lux::render::RenderProgramChannel<>::create();
        sync_    = std::make_shared<lux::render::RenderChannelSync>();

        render_thread_ = std::thread(&RenderThreadOrchestrator::threadMain, this);

        // Wait for the render server to either come up or fail.
        while (!server_ready_.load(std::memory_order_acquire) &&
               !server_failed_.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(kServerWaitSlice);
        }

        if (server_failed_.load(std::memory_order_acquire))
        {
            if (render_thread_.joinable())
                render_thread_.join();
            std::fprintf(stderr, "[LuxEditor] render server failed to start\n");
            return false;
        }
        return true;
    }

    void RenderThreadOrchestrator::requestStop() noexcept
    {
        if (sync_)
            sync_->requestStop();
        if (render_thread_.joinable())
            render_thread_.join();
    }

    void RenderThreadOrchestrator::threadMain()
    {
        auto server = lux::ui::bringUpUIRenderServer(
            channel_, sync_, window_, config_.enable_vulkan_validation, "LuxEditor");
        if (!server)
        {
            server_failed_.store(true, std::memory_order_release);
            return;
        }

        imgui_ops_ = server->imguiOps();

        // ─── Server-side direct init (same thread, before first tick) ───
        //
        // Replaces the old "spawn render thread → main thread waits →
        // session.createScene/addFeature/addUIView etc via channel +
        // syncCall" pattern. Everything below is direct, blocking
        // server-side initialisation. By the time `server_ready_` flips
        // true, `render_infra_` holds the scene, view, and all feature
        // handles. The main thread reads them via `renderInfra()` once
        // it sees the ready signal.

        // ── 1. Feature configs, then a name-keyed FeatureRegistry built below ──
        // The registry (built after the configs it references) replaces the old
        // hand-numbered addFeatureFactory calls + FIP magic indices + per-feature
        // op-id fields: register an ORDERED set of factories, then look up each
        // feature's handle/ops BY NAME (== FeatureFactory.name). Adding/reordering a
        // feature needs no index bookkeeping, and a plugin just add()s its factory.

        // ── 2. Build feature init configs (must outlive createScene) ──
        // The order below matches the old EditorScene::bringUp ordering:
        //   shadow infra → geometry → post → grid (last so it draws
        //   on top of the tonemapped HDR — see the long comment in
        //   the old bringUp for the rationale).
        lux::render::ShadowMapCommConfig    shmap_cfg{};
        shmap_cfg.enable_directional_csm = 1u;

        lux::render::MeshShadowCommConfig   mshsw_cfg{};
        mshsw_cfg.comm_config_version       = lux::render::kMeshShadowCommConfigVersion;
        mshsw_cfg.descriptor_layout_version = lux::render::kMeshShadowDescriptorLayoutVersion;

        lux::render::DeferredGBufferCommConfig gbuf_cfg{};
        gbuf_cfg.comm_config_version       = lux::render::kDeferredGBufferCommConfigVersion;
        gbuf_cfg.descriptor_layout_version = lux::render::kDeferredGBufferDescriptorLayoutVersion;
        // HZB 遮挡剔除开关:view-cull 读上一帧 max-Z 金字塔剔除被遮挡实例。
        gbuf_cfg.extension_flags          |= lux::render::kDeferredGBufferExtFlagHZB;

        lux::render::DeferredLightingCommConfig lit_cfg{};
        lit_cfg.read_mode        = 0; // SAMPLED
        lit_cfg.enable_clustered = 1;
        lit_cfg.cluster_x        = 16;
        lit_cfg.cluster_y        = 9;
        lit_cfg.cluster_z        = 24;
        lit_cfg.max_cluster_indices = 1'048'576;

        lux::render::SkyboxCommConfig       sky_cfg{};
        lux::render::TonemapCommConfig      tm_cfg{
            .tone_map_op = 1,
            .exposure    = 1.0f,
            .gamma       = 2.2f,
        };
        lux::render::GridCommConfig         grid_cfg{};
        // M2: line-list feature config left default — built-in shaders +
        // 200k max vertices (plenty for selection AABB + future gizmos).
        lux::render::LineListTransientCommConfig line_cfg{};
        lux::render::HighlightCommConfig         hl_cfg{};    // shaders default to builtins
        // M3: SkinningFeature has no per-instance config — its only inputs are the
        // bone palettes uploaded via the feature-scoped SkinningProxy (op-ids looked
        // up by name from the registry; see the skeletal mesh bridge's finalize in
        // MeshRenderTraits / InstanceBridge).

        // Register the features in ATTACH order. Light FIRST (owns LightResources
        // that shadow/gbuffer/forward/deferred consume — ShadowMap caches a raw
        // LightResources* at attach), Skinning next (produces the post-skin vertex
        // buffer the mesh draws sample). Everything after is RG-ordered by data
        // dependency, so list position past those two is moot — and lookups are BY
        // NAME, so adding/reordering needs no index bookkeeping (the old fragility).
        lux::render::FeatureRegistry reg;
        reg.add(*server, lux::render::kStandardViewCameraFeatureFactory);  // owns per-view camera state — BEFORE every camera consumer (cull/shadow/hzb/deferred)
        reg.add(*server, lux::render::kLightFeatureFactory);
        reg.add(*server, lux::render::kStandardMaterialFeatureFactory);  // owns ShadingModelRegistry + MaterialResources — BEFORE StandardMeshStack + mesh consumers
        reg.add(*server, lux::render::kStandardMeshStackFeatureFactory);  // owns mesh-stack resources — BEFORE Skinning + mesh consumers
        reg.add(*server, lux::render::kSkinningFeatureFactory);
        reg.add(*server, lux::render::kShadowMapFeatureFactory,        &shmap_cfg, sizeof(shmap_cfg));
        reg.add(*server, lux::render::kMeshShadowFeatureFactory,       &mshsw_cfg, sizeof(mshsw_cfg));
        reg.add(*server, lux::render::kDeferredGBufferFeatureFactory,  &gbuf_cfg,  sizeof(gbuf_cfg));
        reg.add(*server, lux::render::kDeferredLightingFeatureFactory, &lit_cfg,   sizeof(lit_cfg));
        reg.add(*server, lux::render::kSkyboxFeatureFactory,           &sky_cfg,   sizeof(sky_cfg));
        reg.add(*server, lux::render::kTonemapFeatureFactory,          &tm_cfg,    sizeof(tm_cfg));
        reg.add(*server, lux::render::kGridFeatureFactory,             &grid_cfg,  sizeof(grid_cfg));
        reg.add(*server, lux::render::kLineListTransientFactory,       &line_cfg,  sizeof(line_cfg));
        reg.add(*server, lux::render::kHzbFeatureFactory);
        reg.add(*server, lux::render::kHighlightFeatureFactory,        &hl_cfg,    sizeof(hl_cfg));
        reg.add(*server, lux::render::kSpatialCullFeatureFactory);

        // ── 3. Create scene with all features pre-attached, then bind handles ──
        // The editor IS the large-world consumer (streaming demos, big_demo): it opts
        // in by adding SpatialCullFeature above. The core scene knows nothing about it.
        const auto fips = reg.initParams();
        auto scene_result = server->createScene(
            "EditorScene",
            std::span<const lux::render::GeneralRenderServer::FeatureInitParam>{fips.data(), fips.size()});
        reg.bindHandles(scene_result.features);

        // ── 4. Create the ImGui offscreen view for the viewport panel ──
        const auto view_handle = server->addUIView(
            scene_result.scene_id,
            lux::common::Size2D{
                static_cast<uint32_t>(config_.width),
                static_cast<uint32_t>(config_.height)
            },
            "EditorMainView"
        );

        // ── 5. Populate `render_infra_` and signal main thread ──
        // The name-keyed registry holds every feature's handle + dynamic op-ids;
        // consumers query BY NAME (reg.handle("Tonemap") / reg.ops<LightOperationIds>
        // ("Light")). No per-feature fields, no magic indices.
        render_infra_.scene_id         = scene_result.scene_id;
        render_infra_.main_view        = view_handle;
        render_infra_.feature_registry = std::move(reg);

        server_ready_.store(true, std::memory_order_release);

        while (server->tick())
        {
            // Loop until the channel is asked to stop.
        }
    }

} // namespace lux::editor

// ============================================================================
//  ViewCameraOperationHandlers.cpp — StandardViewCameraFeature factory + the
//  feature-scoped per-view camera update handler. The camera update lives HERE
//  (a feature), not in the core RenderServer dispatcher, registered with a
//  DYNAMIC TypeId via register_ops_fn (the grid / light pattern). The core
//  protocol no longer names camera data.
//
//  The handler fills the feature-owned ViewCameraResource (read by the camera
//  consumers — cull / shadow / hzb / deferred lighting) AND publishes the neutral
//  per-view GPU bytes (the view-data SoA entry + the 6 cull-frustum planes) into
//  the core View's OPAQUE staging, which RenderScene::beginFrame uploads after the
//  slot fence wait (#27 — we fill here, CPU-side, during the op dispatch). The core
//  View no longer carries any camera / frustum vocabulary.
// ============================================================================

#include <lux/engine/render/comm/server/RenderServer.hpp>         // Dispatcher, Ctx, FeatureFactory
#include <lux/engine/render/comm/server/FeatureOpRegistrar.hpp>   // typed-op register/unregister
#include <lux/engine/render/renderer/features/FeatureOpSend.hpp>  // sendBulk
#include <lux/engine/render/comm/RenderProtocol.hpp>              // opcodes
#include <lux/engine/render/renderer/features/view_camera/ViewCameraOperation.hpp>
#include <lux/engine/render/renderer/features/view_camera/StandardViewCameraFeature.hpp>
#include <lux/engine/render/renderer/features/view_camera/ViewCameraResource.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>                // getView / sceneRegistry
#include <lux/engine/render/resources/SceneResources.hpp>         // ViewGpuData / fillViewGpuData (neutral staging fill)
#include <lux/engine/render/comm/client/RenderSession.hpp>        // ViewCameraProxy: builder / sendBulk

#include <Eigen/Dense>

#include <cstdint>
#include <cstring>
#include <span>

namespace lux::render
{
    using Dispatcher = GeneralRenderServer::Dispatcher;
    using Ctx        = Dispatcher::Ctx;

    // Exported by the server (forward-declared — the grid-handler convention — to
    // avoid pulling RenderServerImpl.hpp here). Each camera update entry carries
    // its own scene_id, so we resolve per entry via lookupScene.
    RenderScene* lookupScene(void* user_state, RenderSceneId scene_id);

    namespace
    {
        // Build the per-view camera frame data from a wire payload — exactly the
        // mapping the retired core handleViewFrameUpdate did (column-major float[16]
        // → Eigen mat4, derived inverses, frustum from view-proj).
        ViewFrameData buildViewFrameData(const ViewCameraUpdatePayload& u, const common::Size2D& extent)
        {
            ViewFrameData data{};
            std::memcpy(data.camera_view.view.data(), u.view_matrix, sizeof(float) * 16);
            std::memcpy(data.camera_view.proj.data(), u.proj_matrix, sizeof(float) * 16);
            data.camera_view.view_proj     = data.camera_view.proj * data.camera_view.view;
            data.camera_view.inv_view      = data.camera_view.view.inverse();
            data.camera_view.inv_proj      = data.camera_view.proj.inverse();
            data.camera_view.inv_view_proj = data.camera_view.view_proj.inverse();
            data.camera_transform.position =
                Eigen::Vector3f(u.camera_position[0], u.camera_position[1], u.camera_position[2]);
            data.frustum  = Frustum::fromViewProj(data.camera_view.view_proj);
            data.extent   = extent;
            data.viewport = Viewport{0.f, 0.f,
                                     static_cast<float>(extent.width),
                                     static_cast<float>(extent.height),
                                     0.f, 1.f};
            data.scissor  = ScissorRect{0, 0, extent.width, extent.height};
            return data;
        }

        void handleViewCameraUpdate(Ctx& ctx, std::span<const ViewCameraUpdatePayload> updates)
        {
            for (const auto& u : updates)
            {
                auto* sc = lookupScene(ctx.user_state, u.scene_id);
                if (!sc) continue;
                auto* view = sc->getView(u.view);
                if (!view) continue;

                const ViewFrameData data = buildViewFrameData(u, view->current_extent);

                // Feature-owned per-view camera mirror — read by the camera consumers
                // (cull / shadow / hzb / deferred lighting) via ViewCameraResource.
                if (auto* cam = sc->sceneRegistry().find<ViewCameraResource>())
                    cam->setView(u.view.index, data);

                // Publish the neutral per-view GPU bytes into the core View. The core
                // does NOT interpret them; RenderScene::beginFrame uploads
                // view_data_staging into the Set-0 binding-1 slot AFTER the fence wait
                // (#27 — we fill here, CPU-side, during the op dispatch). frustum_staging
                // feeds the GPU cull (RGVulkanRecorder UploadViewFrustum).
                ViewGpuData gpu{};
                fillViewGpuData(data.camera_view, data.camera_transform, data.viewport, gpu);
                std::memcpy(view->view_data_staging.data(), &gpu, sizeof(gpu));
                static_assert(sizeof(data.frustum.planes) == kViewFrustumStrideBytes,
                              "frustum staging stride mismatch");
                std::memcpy(view->frustum_staging.data(), data.frustum.planes.data(), kViewFrustumStrideBytes);
                view->has_view_data = true;
            }
        }
    } // namespace

    // ── Uniform factory interface ────────────────────────────────────────────
    static FeatureHandle standardViewCameraCreateFn(void* scene_ptr, const void* /*param*/, size_t /*sz*/)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);
        StandardViewCameraFeature::Config cfg{};   // no per-scene comm config
        return sc->addFeature<StandardViewCameraFeature>(cfg);
    }

    using ViewCameraOps = FeatureOpRegistrar<
        ServerOp<ViewCameraUpdateOp, &handleViewCameraUpdate>>;

    const FeatureFactory kStandardViewCameraFeatureFactory{
        &standardViewCameraCreateFn,
        &ViewCameraOps::registerAll,
        &ViewCameraOps::unregisterAll,
        "StandardViewCamera",
    };

    // ── Client-side proxy (typed-op send-routing) ─────────────────────────────
    void ViewCameraProxy::update(RenderSceneId scene_id, ViewHandle view,
                                 const float view_matrix[16],
                                 const float proj_matrix[16],
                                 const float camera_position[3])
    {
        ViewCameraUpdatePayload p{};
        p.scene_id = scene_id;
        p.view     = view;
        std::memcpy(p.view_matrix,     view_matrix,     sizeof(p.view_matrix));
        std::memcpy(p.proj_matrix,     proj_matrix,     sizeof(p.proj_matrix));
        std::memcpy(p.camera_position, camera_position, sizeof(p.camera_position));
        sendBulk<ViewCameraUpdateOp>(*session_, ops_, std::span<const ViewCameraUpdatePayload>{&p, 1});
    }

} // namespace lux::render

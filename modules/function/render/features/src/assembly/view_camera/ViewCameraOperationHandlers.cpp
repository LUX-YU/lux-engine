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
#include <lux/engine/function/render/client/FeatureOpSend.hpp>  // sendBulk
#include <lux/engine/function/render/client/protocol/FeatureFactory.hpp>   // FeatureFactory / GenericOkReply
#include <lux/engine/function/render/client/genops/ViewCameraOperation.ops.hpp>
#include <lux/engine/render/renderer/features/view_camera/StandardViewCameraFeature.hpp>
#include <lux/engine/render/renderer/features/view_camera/ViewCameraResource.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>                // getView / sceneRegistry
#include <lux/engine/render/resources/SceneResources.hpp>         // ViewGpuData / fillViewGpuData (neutral staging fill)
#include <lux/engine/function/render/client/RenderFrameSession.hpp>        // ViewCameraProxy: builder / sendBulk

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
            Eigen::Matrix4f rotation_view = Eigen::Matrix4f::Identity();
            std::memcpy(rotation_view.data(), u.view_matrix, sizeof(float) * 16);
            std::memcpy(data.camera_view.proj.data(), u.proj_matrix, sizeof(float) * 16);
            data.camera_transform.position = Eigen::Vector3f(
                static_cast<float>(u.render_origin.page_delta[0]) * u.coordinate_page_size + u.render_origin.local[0],
                static_cast<float>(u.render_origin.page_delta[1]) * u.coordinate_page_size + u.render_origin.local[1],
                static_cast<float>(u.render_origin.page_delta[2]) * u.coordinate_page_size + u.render_origin.local[2]);
            data.render_origin = u.render_origin;
            data.coordinate_page_size = u.coordinate_page_size;
            data.camera_view.view = rotation_view;
            data.camera_view.view.block<3, 1>(0, 3) =
                -rotation_view.block<3, 3>(0, 0) * data.camera_transform.position;
            data.camera_view.view_proj     = data.camera_view.proj * data.camera_view.view;
            data.camera_view.inv_view      = data.camera_view.view.inverse();
            data.camera_view.inv_proj      = data.camera_view.proj.inverse();
            data.camera_view.inv_view_proj = data.camera_view.view_proj.inverse();
            data.frustum  = Frustum::fromViewProj(data.camera_view.view_proj);
            data.extent   = extent;
            data.viewport = Viewport{0.f, 0.f,
                                     static_cast<float>(extent.width),
                                     static_cast<float>(extent.height),
                                     0.f, 1.f};
            data.scissor  = ScissorRect{0, 0, extent.width, extent.height};
            return data;
        }

    } // anonymous namespace (helpers)
    void handleViewCameraUpdate(GeneralRenderServer::Dispatcher::Ctx& ctx, std::span<const ViewCameraUpdatePayload> updates)
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
                // feeds the GPU cull: Renderer::renderView hands it to the render graph
                // as neutral bytes (RGFrameContext::view_frustum), and the MeshCull
                // kernel uploads them at record time. Neither the graph nor the kernel
                // knows what a View is — see MeshKernels replayMeshCullCommand.
                ViewGpuData gpu{};
                CameraView gpu_camera = data.camera_view;
                std::memcpy(gpu_camera.view.data(), u.view_matrix, sizeof(float) * 16);
                gpu_camera.inv_view = gpu_camera.view.inverse();
                fillViewGpuData(
                    gpu_camera,
                    data.camera_transform,
                    data.viewport,
                    u.render_origin,
                    u.coordinate_page_size,
                    gpu
                );
                std::memcpy(view->view_data_staging.data(), &gpu, sizeof(gpu));
                ViewCullData cull{};
                cull.frustum = Frustum::fromViewProj(data.camera_view.proj * gpu_camera.view);
                for (std::size_t axis = 0; axis < 3; ++axis)
                {
                    cull.origin_page[axis] = u.render_origin.page_delta[axis];
                    cull.origin_local_page_size[axis] = u.render_origin.local[axis];
                }
                cull.origin_local_page_size[3] = u.coordinate_page_size;
                std::memcpy(view->frustum_staging.data(), &cull, sizeof(cull));
                view->has_view_data = true;
            }
        }

} // namespace lux::render

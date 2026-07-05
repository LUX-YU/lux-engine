#pragma once
// ============================================================================
//  Camera2DUploadBridge.hpp — active Camera2D → render per-view camera (d2).
//
//  A BESPOKE IRenderableBridge that mirrors the 3D CameraSceneSystem: each frame it
//  uploads the active Camera2D's ortho view/proj into the scene's per-view ViewGpuData
//  (descriptor set 0, binding 1) via ViewCameraProxy — REUSING the same per-view camera
//  path as 3D (design/recon: 2D does not duplicate a camera upload). The Canvas2D sprite
//  shader reads that slot, so without this bridge sprites transform by whatever camera
//  (identity) sits there.
//
//  Runs during RenderableSystem::update (AFTER World::tick → Camera2DSystem has refreshed
//  the cache). No StandardViewCamera feature in the scene, or no single active Camera2D →
//  no-op (ViewCameraProxy is invalid / activeCamera returns null).
// ============================================================================

#include <lux/engine/gameplay/render_bridge/IRenderableBridge.hpp>
#include <lux/engine/gameplay/render_bridge/RenderableBridgeContext.hpp>          // session()/scene()/view()/features()
#include <lux/engine/gameplay/2d/world/systems/Camera2DSystem.hpp>                // activeCamera
#include <lux/engine/gameplay/2d/world/components/Camera2DCacheComponent.hpp>     // Camera2DCacheComponent
#include <lux/engine/render/renderer/features/view_camera/ViewCameraOperation.hpp> // ViewCameraProxy / ViewCameraOperationIds
#include <lux/engine/meta/LuxObject.hpp>   // EntityRegistry / null_entity

namespace lux::gameplay::d2
{
    class Camera2DUploadBridge final : public lux::gameplay::IRenderableBridge
    {
    public:
        void drive(lux::meta::EntityRegistry& registry, RenderableBridgeContext& ctx) override
        {
            if (stopping_) return;

            const auto cam = activeCamera(registry);   // the single ActiveCamera2DTag entity, or null
            if (cam == lux::meta::null_entity) return;
            const auto* cache = registry.try_get<Camera2DCacheComponent>(cam);
            if (!cache) return;

            // Resolve the per-view camera op by name — invalid on a scene without a
            // StandardViewCamera feature, in which case there is nothing to upload into.
            const auto ops = ctx.features().ops<lux::render::ViewCameraOperationIds>("StandardViewCamera");
            if (!ops.valid()) return;

            const float eye[3] = {0.f, 0.f, 0.f};   // 2D ortho: the sprite shader ignores cam_pos
            lux::render::ViewCameraProxy(ctx.session(), ops).update(
                ctx.scene(), ctx.view(),
                cache->view.data(), cache->proj.data(), eye);   // Eigen Matrix4f = column-major
        }

        void reap(lux::meta::EntityRegistry& /*registry*/, RenderableBridgeContext& /*ctx*/) override {}
        void beginShutdown(RenderableBridgeContext& /*ctx*/) override { stopping_ = true; }
        [[nodiscard]] bool hasPendingShutdownWork() const override { return false; }
        void flushShutdownCleanup(RenderableBridgeContext& /*ctx*/) override {}

    private:
        bool stopping_{false};
    };

} // namespace lux::gameplay::d2

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
    class Camera2DUploadBridge final : public lux::gameplay::TransientRenderableBridge
    {
    public:
        void drive(lux::meta::EntityRegistry& registry, RenderableBridgeContext& ctx) override
        {
            // Shared 2D camera gate (P0-5): the single active camera WITH a derived
            // cache. v2: the gate drives the RETAINED scene enable bit — sprites are
            // GPU-resident, so "no publishable camera" no longer stops submissions
            // (there are none); it flips SetCanvas2DEnabled EDGE-TRIGGERED instead
            // (steady state = zero wire), and the canvas draws nothing while off.
            const auto cam = publishableCamera(registry);
            const bool enabled = (cam != lux::meta::null_entity);
            if (last_enabled_ != static_cast<int>(enabled))
            {
                ctx.canvas2d().setEnabled(ctx.scene(), enabled);   // no-op without a Canvas2D
                last_enabled_ = static_cast<int>(enabled);
            }
            if (!enabled) return;
            const auto& cache = registry.get<Camera2DCacheComponent>(cam);

            // Resolve the per-view camera op by name — invalid on a scene without a
            // StandardViewCamera feature, in which case there is nothing to upload into.
            const auto ops = ctx.features().ops<lux::render::ViewCameraOperationIds>("StandardViewCamera");
            if (!ops.valid()) return;

            const float eye[3] = {0.f, 0.f, 0.f};   // 2D ortho: the sprite shader ignores cam_pos
            lux::render::ViewCameraProxy(ctx.session(), ops).update(
                ctx.scene(), ctx.view(),
                cache.view.data(), cache.proj.data(), eye);   // Eigen Matrix4f = column-major
        }

    private:
        int last_enabled_{-1};   ///< tri-state: -1 = never sent → first drive always sends
    };

} // namespace lux::gameplay::d2

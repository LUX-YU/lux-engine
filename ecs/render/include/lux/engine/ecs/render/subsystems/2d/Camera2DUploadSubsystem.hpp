#pragma once
// ============================================================================
//  Camera2DUploadSubsystem.hpp — active Camera2D → render per-view camera (d2).
//
//  A BESPOKE IRenderSubsystem that mirrors the 3D CameraSceneSystem: each frame it
//  uploads the active Camera2D's ortho view/proj into the scene's per-view ViewGpuData
//  (descriptor set 0, binding 1) via ViewCameraProxy — REUSING the same per-view camera
//  path as 3D (design/recon: 2D does not duplicate a camera upload). The Canvas2D image
//  shader reads that slot, so without this bridge images transform by whatever camera
//  (identity) sits there.
//
//  Runs during RenderSystem::update (after Camera2DSystem has refreshed
//  the cache). No StandardViewCamera feature in the scene, or no single active Camera2D →
//  no-op (ViewCameraProxy is invalid / activeCamera returns null).
// ============================================================================

#include <lux/engine/ecs/render/IRenderSubsystem.hpp>
#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/ecs/render/SceneRenderBinding.hpp>          // session()/scene()/features()
#include <lux/engine/ecs/render/subsystems/CameraViewSubsystem.hpp>
#include <lux/engine/ecs/render/components/RenderViewBindingComponent.hpp>       // camera → view wiring (data)
#include <lux/engine/ecs/render/components/2d/Camera2DCacheComponent.hpp>     // Camera2DCacheComponent
#include <lux/engine/ecs/render/RenderSpatialTransform.hpp>
#include <lux/engine/function/render/client/genops/ViewCameraOperation.ops.hpp> // ViewCameraProxy / ViewCameraOperationIds
#include <lux/engine/ecs/Registry.hpp>   // EntityRegistry

namespace lux::ecs
{
    class Camera2DSystem;

    /// ★ 批 B4 起它是一个**普通的 schedule node**(`ISystem`)。渲染绑定由构造
    ///   注入,不再每帧由 `RenderSystem` 的调度循环递进来。
    class Camera2DUploadSubsystem final : public lux::ecs::IRenderSubsystem
    {
    public:
        Camera2DUploadSubsystem() = default;

        /// 每帧把 2D 相机的矩阵推进 per-view 状态。
        [[nodiscard]] std::span<const std::string_view>
        renderFeatures() const noexcept override
        {
            static const std::string_view kFeatures[] = { "StandardViewCamera" };
            return kFeatures;
        }

        [[nodiscard]] std::span<const RenderSubsystemType>
        runsAfter() const noexcept override
        {
            static constexpr RenderSubsystemType dependencies[]{
                renderSubsystemType<CameraViewSubsystem>()};
            return dependencies;
        }

    public:
        void update(RenderSubsystemContext& uctx) override
        {
            auto& registry = uctx.registry();
            auto& ctx = uctx.render();
            // BINDING-DRIVEN (2026-07-11 user ruling): each camera carrying a
            // RenderViewBinding pushes its ortho view/proj into ITS bound view —
            // multi-camera / multi-view is data composition; the sparse set skips
            // unbound cameras (an editor-inert scene camera pushes nothing, so
            // there is no editor-vs-scene camera write race by construction).
            //
            // The retained scene enable bit stays EDGE-TRIGGERED (steady state =
            // zero wire): enabled = any bound camera with a derived cache exists
            // (Camera2DSystem ran). Independent of op resolution — a headless /
            // feature-less scene still tracks the gate (Slice-A pinned semantics).
            const auto bound = registry.view<Camera2DCacheComponent, RenderViewBindingComponent>();
            const bool enabled = (bound.begin() != bound.end());
            if (last_enabled_ != static_cast<int>(enabled))
            {
                setEnabled(ctx.canvas2d(), ctx.scene(), enabled);   // no-op without a Canvas2D
                last_enabled_ = static_cast<int>(enabled);
            }
            if (!enabled) return;

            // Resolve the per-view camera op by name — invalid on a scene without a
            // StandardViewCamera feature, in which case there is nothing to upload into.
            const auto ops = ctx.features().ops<lux::render::ViewCameraOperationIds>("StandardViewCamera");
            if (!ops.valid()) return;

            for (auto e : bound)
            {
                const auto& cache = bound.get<Camera2DCacheComponent>(e);
                const auto& bind  = bound.get<RenderViewBindingComponent>(e);
                const auto render_origin = makeRenderLargePosition(
                    cache.render_origin,
                    ctx.sceneOriginTile3D()
                );
                if (!render_origin)
                {
                    ctx.requestSceneOriginRebase(cache.render_origin);
                    continue;
                }
                lux::render::viewCameraUpdate(lux::render::ViewCameraProxy(ctx.session(), ops),
                    ctx.scene(), bind.view(),
                    cache.view.data(), cache.proj.data(), *render_origin,
                    static_cast<float>(kRenderSpatialTileSize));
            }
        }

    private:
        int last_enabled_{-1};   ///< tri-state: -1 = never sent → first drive always sends
    };

} // namespace lux::ecs

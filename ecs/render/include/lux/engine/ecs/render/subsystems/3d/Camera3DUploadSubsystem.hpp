#pragma once
// ============================================================================
//  Camera3DUploadSubsystem.hpp — bound Camera3D → render per-view camera
//  (lux::ecs). The dimensional sibling of Camera2DUploadSubsystem: BINDING-DRIVEN
//  (user ruling 2026-07-11) — each camera carrying a RenderViewBinding pushes
//  its view/proj (Camera3DSystem-derived, stored on Camera3DCacheComponent) and eye
//  (ResolvedTransform3D translation) into ITS bound view. Unbound cameras are
//  inert; multi-camera / multi-view is pure data composition.
//
//  Runs during RenderSystem::update (after the camera system refreshed state).
//  No StandardViewCamera feature → invalid ops → graceful no-op.
// ============================================================================

#include <lux/engine/ecs/render/IRenderSubsystem.hpp>
#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/ecs/render/SceneRenderBinding.hpp>          // session()/scene()/features()
#include <lux/engine/ecs/render/subsystems/CameraViewSubsystem.hpp>
#include <lux/engine/ecs/render/components/RenderViewBindingComponent.hpp>       // camera → view wiring (data)
#include <lux/engine/ecs/render/components/3d/Camera3DComponent.hpp>
#include <lux/engine/ecs/render/components/3d/Camera3DCacheComponent.hpp>
#include <lux/engine/ecs/components/ResolvedTransform3DComponent.hpp>
#include <lux/engine/ecs/SpatialTransformMath.hpp>
#include <lux/engine/ecs/render/RenderSpatialTransform.hpp>
#include <lux/engine/function/render/client/genops/ViewCameraOperation.ops.hpp>
#include <lux/engine/meta/LuxObject.hpp>

namespace lux::ecs
{
    class Camera3DSystem;

    /// ★ 批 B4 起它是一个**普通的 schedule node**(`ISystem`)。渲染绑定由构造
    ///   注入,不再每帧由 `RenderSystem` 的调度循环递进来。
    class Camera3DUploadSubsystem final : public lux::ecs::IRenderSubsystem
    {
    public:
        Camera3DUploadSubsystem() = default;

        /// 每帧把 3D 相机的矩阵推进 per-view 状态。
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
            const auto ops = ctx.features().ops<lux::render::ViewCameraOperationIds>("StandardViewCamera");
            if (!ops.valid()) return;

            for (auto e : registry.view<Camera3DComponent,
                                        Camera3DCacheComponent,
                                        ResolvedTransform3DComponent,
                                        RenderViewBindingComponent>())
            {
                const auto& cache = registry.get<Camera3DCacheComponent>(e);
                const auto& wc   = registry.get<ResolvedTransform3DComponent>(e);
                const auto& bind = registry.get<RenderViewBindingComponent>(e);
                const auto render_origin = makeRenderLargePosition(
                    wc.position,
                    ctx.sceneOriginTile3D()
                );
                if (!render_origin)
                {
                    ctx.requestSceneOriginRebase(wc.position);
                    continue;
                }
                lux::render::viewCameraUpdate(lux::render::ViewCameraProxy(ctx.session(), ops),
                    ctx.scene(), bind.view(),
                    cache.view.data(), cache.proj.data(), *render_origin,
                    static_cast<float>(kRenderSpatialTileSize));
            }
        }

    };

} // namespace lux::ecs

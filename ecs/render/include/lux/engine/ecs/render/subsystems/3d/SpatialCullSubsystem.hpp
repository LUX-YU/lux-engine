#pragma once
// ============================================================================
//  SpatialCullSubsystem.hpp — 把场景设置里的 `cull_distance` 推给 SpatialCull
//  feature。
//
//  ── 它从哪来 ──────────────────────────────────────────────────────────────
//
//  此前这段在 `StreamingSceneSystem::onPreRenderableUpdate` 里：一个跑在宿主
//  相位表上的场景系统，从 `SceneTickContext` 拿 session / features / scene_id，
//  自己发 `setParams`。也就是说，「读一个组件的字段 → 发一条渲染命令」这件事
//  当时有两种写法，取决于它碰巧长在哪一层。
//
//  它是标准的特性参数形状：读 `SceneSettingsComponent`，值变了才推。之所以没做成
//  「策略结构体 + `FeatureParamSubsystem`」，是因为 SpatialCull
//  走的是**通用**的 `FeatureParamsProxy::setParams`（按名字查 paramSetOp），而
//  `FeatureParamSubsystem` 要的是 feature 自己那套生成的 op 结构体。硬套要给策略加一条
//  「我用通用参数通道」的分支——为一个用户加一条分支不划算。
//
//  ⚠️ **推送是有代价的**（一次跨线程命令），所以按值比较、只在变化时推。此前那份
//     脏比较和流送参数的脏比较混在一个 `applied_` 标志里；拆开之后各管各的。
// ============================================================================

#include <span>
#include <string_view>

#include <lux/engine/ecs/render/RenderStage.hpp>
#include <lux/engine/ecs/render/SceneRenderBinding.hpp>
#include <lux/engine/ecs/render/components/3d/SceneSettingsComponent.hpp>

#include <lux/engine/function/render/client/RenderFrameSession.hpp>
#include <lux/engine/function/render/client/protocol/FeatureParamsOperation.hpp>
#include <lux/engine/function/render/client/features/spatialcull/SpatialCullParams.hpp>

namespace lux::ecs
{
    /// ★ 批 B3 起它是一个**普通的 schedule node**(`ISystem`),不再是
    ///   `RenderSystem` 里 vector 中的一项。
    ///
    ///   它是最适合打头阵的形状:transient(不留跨帧的远端对象、不钉资产引用、
    ///   没有在途 create),所以既不需要观察者、也不需要 releaseRefs —— 迁移只是
    ///   把 `tick(reg, ctx)` 改成 `update(SystemUpdateContext&)`,渲染绑定由构造
    ///   注入(与 CameraViewSubsystem 同款),而不是每帧由调度循环递进来。
    class SpatialCullSubsystem final : public RenderStage
    {
    public:
        SpatialCullSubsystem() = default;

        [[nodiscard]] std::span<const std::string_view>
        requiredFeatures() const noexcept override
        {
            static const std::string_view kFeatures[] = { "SpatialCull" };
            return kFeatures;
        }

        void extract(RenderSubsystemContext& uctx) override
        {
            auto& reg = uctx.registry();
            auto& ctx = uctx.render();
            auto v = reg.view<SceneSettingsComponent>();
            if (v.begin() == v.end()) return;   // 场景没有设置组件 → 用 feature 的默认值
            const float want = v.get<SceneSettingsComponent>(*v.begin()).cull_distance;
            if (applied_ && want == last_) return;

            const auto op = ctx.features().paramSetOp("SpatialCull");
            if (op == lux::render::kInvalidTypeId) return;   // feature 没挂 → no-op

            lux::render::SpatialCullParams scp{};   // cell_size 保留 SpatialCull 的默认值
            scp.cull_distance = want;
            lux::render::FeatureParamsProxy(ctx.session()).setParams(
                ctx.scene(), ctx.features().handle("SpatialCull"), op, &scp, sizeof(scp));

            last_    = want;
            applied_ = true;
        }

    private:
        float last_{0.f};
        bool  applied_{false};
    };

} // namespace lux::ecs

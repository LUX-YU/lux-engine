#pragma once
// ============================================================================
//  CameraViewSystem.hpp — view 归相机所有（lux::ecs）。
//
//  这个系统是相机域**唯一的反应者**。挂 `ViewPresentComponent` 的实体得到一个
//  render view;摘掉组件或销毁实体,view 被还回去。运行时(`lux::runtime::SceneRuntime`)
//  因此不需要认识「相机」这个概念 —— 它既不建 view,也不指派谁往里推矩阵。
//
//  ── 它是一个普通的 `lux::ecs::ISystem` ───────────────────────────────────
//  构造时吃 session + scene id(与 `RenderSystem` 同款),宿主把它装进 World 的
//  `kPhasePreTransform` 相位 —— 绑定要在派生量刷新之前落位。
//  (此前这里解释「为什么不是 SceneSystem」:那一层已经删了,World 自己有相位。)
//
//  ── 三条规矩在这里各有一个真实用户 ───────────────────────────────────────
//  ① 观察者内不得直接改世界   → 观察者只往**本节点的命令分片**入队(值命令,不是
//                                 闭包),由 `Schedule` 在 tick 末尾唯一的 barrier 应用
//  ② 立即观察者 vs 帧内轮询   → 结构性转换(发请求/还资源)用观察者;
//                                 view 创建的**回复**用帧内轮询
//  ③ 异步就绪不走观察者       → `addView` 是请求/回复(RenderRequest<ViewCreatedReply>),
//                                 观察者根本没法就地完成
//
//  `addView` 的 transport reply 仍在帧安全点轮询；回复成功后句柄立即 adopt 成
//  move-only RenderViewLease，再移入 RenderViewBindingComponent。移除绑定只需要
//  销毁 component：lease 把释放义务交还 RenderFrameSession，不再拼手写 remove 回调。
// ============================================================================

#include <lux/engine/function/visibility.h>

#include <lux/engine/ecs/render/RenderExtractionResources.hpp>
#include <lux/engine/ecs/render/SceneRenderBinding.hpp>
#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/function/render/client/core/FeatureHandle.hpp>

#include <memory>

namespace lux::ecs
{

    class LUX_FUNCTION_PUBLIC CameraViewSystem final
        : public lux::ecs::ISystem
    {
    public:
        CameraViewSystem(
            SceneRenderBinding& binding,
            ActiveRenderView& active_view);
        ~CameraViewSystem() override;

        CameraViewSystem(const CameraViewSystem&) = delete;
        CameraViewSystem& operator=(const CameraViewSystem&) = delete;

        /// 连信号 + **折入存量**,两件一起做 —— 「谁先谁后」不再是调用方要操心的事。
        /// 折入存量入的是命令,所以必须等到这里才做:`SystemSetupContext` 带着本节点
        /// 的 command writer,而构造函数没有。
        void onAdded(const lux::ecs::SystemSetupContext& setup) override;

        /// 从活着的世界里摘除:断信号。
        void onRemoved(const lux::ecs::SystemRemovalContext& removal) override;

        /// 每帧：轮询在途的 addView 回复 + 对齐 aspect。
        /// 装在 `kPhasePreTransform` —— 绑定要在派生量刷新之前落位，这一帧才自洽。
        ///
        /// **不再排空命令队列**:排空归 `Schedule::applyCommandBarrier()`,全项目
        /// 只有那一个 apply 点。
        void update(const SystemUpdateContext& context) override;

        /// 编辑器相机导航通过普通 `ISystem` 依赖声明与本子系统的
        /// 先后，不再经过可拼错的字符串名字。

        /// **阻塞**地把「已挂 ViewPresentComponent 的相机」变成「有 view 的相机」。
        ///
        /// 常规路径是异步的（观察者入队 → 排空发请求 → 若干帧后回复落地），稳态下
        /// 无所谓。但 bring-up 不行：宿主指定出图相机之后、第一帧提交之前 view 必须
        /// 在位，否则那几帧宿主 target 上**没有任何层** —— 黑屏几帧、不报错、
        /// 事后无从追查。宿主在装配末尾调一次。
        ///
        /// 前置一：帧开着（内部走 `awaitAllReady`，它自己做阻塞提交）。
        /// 前置二：调用方**已经排过一次** `Schedule::applyCommandBarrier()` ——
        ///         观察者/折入存量入的 addView 请求要先被应用才有东西可等。
        /// Close all live child view leases through the Control lane before
        /// RenderSceneLease closes. No lexical frame is required.
        void requestClose() noexcept override;
        [[nodiscard]] bool closeComplete() const noexcept override;
        [[nodiscard]] std::span<const Type> runsAfter() const noexcept
            override;

    private:
        struct Impl;
        /// 本系统的四种值命令,定义在 .cpp。作为**嵌套类**而不是自由结构:嵌套类
        /// 是成员,天然够得着 `impl_`,不必为四个小结构各写一条 friend。
        struct Commands;

        /// 唯一所有权,不再是 shared_ptr。此前它必须是 shared 的:入队的是闭包,
        /// 闭包得拿 `weak_ptr` 才能安全地发现宿主没了。现在命令是值、生产者用
        /// 槽位代次认,那条理由消失了。
        std::unique_ptr<Impl> impl_;
    };

} // namespace lux::ecs

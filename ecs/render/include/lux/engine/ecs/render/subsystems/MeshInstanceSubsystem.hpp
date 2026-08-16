#pragma once
/**
 * @file MeshInstanceSubsystem.hpp
 * @brief 「网格实例」形状的子系统基类(Static/Skeletal Mesh 的基底): the full GPU
 *        mesh-instance lifecycle written once —
 *          - first sight : ensure mesh+material (async, cached), addMeshInstance,
 *                          refcount-acquire the assets once the slot reply lands;
 *          - steady state: detect an asset-id swap (tear down → rebuild next frame),
 *                          flip first-frame view visibility, diff+push instance flags
 *                          (cast-shadow / visible / highlight, single-writer), and
 *                          push the transform ONLY when dirty — zero-copy, the matrix
 *                          pointer comes straight from the component (§6.1);
 *          - 离场        : `on_destroy` 观察者把对象句柄读走记账(实体死了/丢了 C/
 *                          获得 Exclude 标签,例如世界流送的 dormant),下一次 tick
 *                          的开头排空发 removeMeshInstance —— 此前是每帧全扫的 reap;
 *          - finalize    : optional per-frame batch flush (Skeletal's one-shot
 *                          uploadBoneBatch covering every skinned instance).
 *
 * The per-feature policy `Traits` supplies `Component` / `geometry` / `Require` /
 * `Exclude` / `transform`, plus the optional skinning quartet `FrameState` /
 * `beginFrame` / `accumulate` / `flush` (detected with C++20 `requires` — a policy
 * without them pays nothing). The component fields `mesh_asset_id` /
 * `material_asset_id` / `cast_shadow` / `visible` are read directly (duck-typed:
 * intrinsic to the mesh-instance shape). Folds the lifecycle helpers the old mesh
 * adapters shared (steady-state swap / 离场 / instance-flags diff). 具名子系统 =
 * 策略结构体 + 别名:`using MeshSubsystem = MeshInstanceSubsystem<MeshRenderPolicy>;`。
 */

#include <lux/engine/ecs/render/components/MeshGpuCacheComponent.hpp>   // 解析好的网格/材质句柄(资源子系统产出)
#include <lux/engine/ecs/render/components/MeshInstanceReadyComponent.hpp> // 「实例就绪且对当前 view 可见」观察点(本类维护)
#include <lux/engine/ecs/render/components/MeshInstanceStateComponent.hpp> // 逐实例状态(批 R1:从侧表搬进组件池)
#include <lux/engine/ecs/render/components/HighlightedComponent.hpp>    // 高亮是实体的一个标签,不是宿主推来的集合
#include <lux/engine/ecs/render/components/AssetStreamingStateComponent.hpp>
#include <lux/engine/ecs/render/VisualTransition.hpp>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <iterator>
#include <unordered_map>
#include <utility>
#include <vector>

// 验证 oracle 的开关 `LUX_ECS_EXTRACTION_VERIFY` 定义在 RenderViewUtil.hpp
// (与消费它的 `ExtractionChangeSet::drain` 同处)。**只有一个真相源** ——
// 两处各写一份默认值,迟早有人翻一个不翻另一个,而那是静默的。

#include <lux/engine/meta/LuxObject.hpp>                    // entity_id / EntityRegistry
#include <lux/engine/resource/asset/Asset.hpp>                       // asset_id_t

#include <lux/engine/function/render/client/RenderProtocol.hpp>        // MeshInstanceSlotReply / kInstanceFlag*
#include <lux/engine/function/render/client/resources/mesh/RenderObjectTypes.hpp>     // RenderObjectHandle
#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp>  // RMeshHandle
#include <lux/engine/function/render/client/RenderFrameSession.hpp>  // RenderFrameSession (.then)
#include <lux/engine/function/render/client/RenderRequest.hpp>  // ScopedRenderRequest RAII owner
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp>  // MeshStackProxy
#include <lux/engine/function/render/client/genops/HighlightOperation.ops.hpp>  // kInstanceFlagHighlight
#include <lux/engine/function/render/client/features/streaming_feedback/StreamingFeedbackOperation.hpp>

#include <lux/engine/ecs/render/IRenderSubsystem.hpp>
#include "lux/engine/ecs/render/SceneRenderBinding.hpp"
#include "lux/engine/ecs/render/RenderExtractionResources.hpp"
#include "lux/engine/ecs/render/RenderViewUtil.hpp"
#include "lux/engine/ecs/render/TrackedRenderRequest.hpp"

namespace lux::ecs
{
    class ResidencySubsystem;

    namespace instance_detail
    {
        /// Empty stand-in when a policy declares no per-frame FrameState (static mesh).
        struct NoFrameState {};

        template <class T> struct FrameStateOf { using type = NoFrameState; };
        template <class T> requires requires { typename T::FrameState; }
        struct FrameStateOf<T> { using type = typename T::FrameState; };
    }

    template <class Traits>
    /// ★ 批 B3 起它是一个**普通的 schedule node**(`ISystem`)。渲染绑定由构造
    ///   注入,不再每帧由 `RenderSystem` 的调度循环递进来。
    ///
    ///   它的 `RenderRequest::then` 状态机**原地保留,不迁 sender** ——
    ///   批 C2 的裁决与三条理由记在 `PooledSlotSubsystem.hpp` 的类头,
    ///   那里是全仓 8 处 ecs 层 `.then` 的共同裁决点。
    class MeshInstanceSubsystem final : public IRenderSubsystem
    {
        using T          = Traits;
        using C          = typename Traits::Component;
        using FrameState = typename instance_detail::FrameStateOf<T>::type;

        /// ★ 批 R1(反应式抽取):逐实例状态从本类私有的
        ///   `std::unordered_map<entity_id, Live>` 搬进了**组件池**。
        ///   完整理由见 `MeshInstanceStateComponent.hpp` 的文件头。
        using State = MeshInstanceStateComponent<T>;

        /// 已离开组件集合、但状态组件还没摘掉的实体(实体本身仍然活着)。
        /// 观察者只记这一笔;真正的 `remove<State>` 在 `update()` 里做 ——
        /// 结构性修改不许在信号里做(CLAUDE.md 规矩一)。
        ///
        /// ★ 批 R3 曾把它改成一条走 `EcsCommandBuffer` 的 `UnbindRequested`
        ///   命令,理由是「别手搓第二套延迟机制」。**改回来了**,依据是
        ///   `render_subsystem_probes` 的 ⑥-c 当场挂掉:
        ///
        ///   ① 这是**同一个节点自产自销**的延迟 —— 观察者记、本节点的 update
        ///      消费。命令分片的增值(跨 barrier 的生产者存活校验、跨节点定序、
        ///      世界级安全点)在这里一个都用不上;
        ///   ② 用了它,节点就**在没有 Schedule 时静默失效** —— 手工驱动的探针/
        ///      demo 里 `push` 返回 NoProducer,而观察者里那个错误无处可报,
        ///      只能丢掉。那正是本次重构在消灭的那类静默失败;
        ///   ③ barrier 在整个 tick 末尾,摘除会晚一个相位。
        ///
        ///   条例是「观察者内不得直接改世界」—— 本队列**遵守**它,只是用节点
        ///   私有队列而非共享分片,而消费者就是自己时这是正当的。
        std::vector<lux::meta::entity_id> to_unbind_;

        /// ★ 批 R2:变更驱动的入口。稳态下它是空的 —— 那就是整个改造的目的。
        ExtractionChangeSet<C, typename T::Require, typename T::Exclude> changes_;
        /// 上一次见到的出图 view 代次。换了就要给所有实例重发可见性。
        std::uint32_t last_seen_view_gen_{0};
        TrackedRenderRequest<
            lux::meta::entity_id,
            lux::render::MeshInstanceSlotReply,
            State> create_requests_;
        FrameState                                     frame_{};   // per-frame skinning scratch (empty for static)

        // G-05: entities whose addMeshInstance replied a FAILURE. A failed create never
        // got a valid object, so it must not become live or bump refcounts — but nor
        // should drive re-issue it every frame (command + would-be log spam). We remember
        // the FAILED asset ids + the failure kind: a CONFIG error (scene / mesh-stack
        // feature absent) is futile to retry until the ids change (the config is fixed);
        // a CAPACITY error (pool / section exhausted) is transient, retried after backoff.
        struct FailRecord
        {
            lux::asset::asset_id_t mesh_id{};
            lux::asset::asset_id_t material_id{};
            lux::render::RMeshHandle mesh{};
            lux::render::RMaterialHandle material{};
            bool                   permanent{false};   // true = don't auto-retry (config / protocol failure)
            bool                   dispatch_reported{false};
            bool                   reply_reported{false};
            int                    retry_in{0};        // transient (capacity): drives until next retry
        };
        std::unordered_map<lux::meta::entity_id, FailRecord> failed_;
        static constexpr int kTransientRetryDrives = 120;   // ~2s @ 60fps between capacity retries

        /// 已离场、`removeMeshInstance` 还没发出去的对象句柄。观察者填，`tick` 开头排空。
        /// 句柄之外还捎上实体 id —— 不是为了发命令(排空时实体多半已经不在了),
        /// 而是为了把还活着的实体身上的 `MeshInstanceReadyComponent` 摘掉
        /// (组件被摘 / 获得 Exclude 标签时实体仍在;整体销毁时 EnTT 自己会收)。
        struct Leaving
        {
            lux::render::RenderObjectHandle object{};
            lux::meta::entity_id            entity{entt::null};
            std::uint32_t transition_milliseconds{0u};
            std::uint32_t transition_seed{0u};
        };
        std::vector<Leaving>                                           leaving_;
        ComponentSetLeaveObserver<C, typename T::Require, typename T::Exclude> leave_;
        /// `on_destroy<State>` 的连接靠它解绑(Schedule 先于 World 析构,
        /// 所以本节点消亡时 registry 一定还在)。
        lux::meta::EntityRegistry* reg_{nullptr};

        /// 「实体离开了本子系统关心的组件集合」——**实体本身可能还活着**。
        /// 只记账，不发命令、不改世界（构建器此刻多半没开，而且信号正处在
        /// 别的 pool 的一次修改中途 —— CLAUDE.md 规矩一）。
        ///
        /// 真正的归还由 `update()` 里的 `remove<State>` 触发的
        /// `on_destroy<State>` 完成 —— 见 onStateDestroyed。
        void onLeave(lux::meta::entity_id e)
        {
            (void)create_requests_.abandon(e);
            to_unbind_.push_back(e);
            // G-05 的失败记录同样按离场清理，保持 failed_ 有界。
            failed_.erase(e);
        }

        [[nodiscard]] static bool sameTransform(
            const lux::render::RenderSpatialTransform3D& lhs,
            const lux::render::RenderSpatialTransform3D& rhs) noexcept
        {
            return std::equal(
                       std::begin(lhs.basis_local),
                       std::end(lhs.basis_local),
                       std::begin(rhs.basis_local)) &&
                std::equal(
                       std::begin(lhs.page_delta),
                       std::end(lhs.page_delta),
                       std::begin(rhs.page_delta)) &&
                lhs.flags == rhs.flags;
        }

        [[nodiscard]] FailRecord& rememberFailure(
            lux::meta::entity_id entity,
            lux::asset::asset_id_t mesh_id,
            lux::asset::asset_id_t material_id,
            lux::render::RMeshHandle mesh,
            lux::render::RMaterialHandle material
        )
        {
            auto [failure, inserted] = failed_.try_emplace(
                entity,
                FailRecord{
                    mesh_id,
                    material_id,
                    mesh,
                    material,
                    false,
                    false,
                    false,
                    0
                }
            );
            if (!inserted &&
                (failure->second.mesh_id != mesh_id ||
                 failure->second.material_id != material_id ||
                 failure->second.mesh != mesh ||
                 failure->second.material != material))
            {
                failure->second = FailRecord{
                    mesh_id,
                    material_id,
                    mesh,
                    material,
                    false,
                    false,
                    false,
                    0
                };
            }
            return failure->second;
        }

        void drainCreateCompletions(lux::meta::EntityRegistry& reg)
        {
            create_requests_.drain(
                [this, &reg](auto completion)
                {
                    const auto e = completion.key;
                    const auto& reply = completion.reply;
                    const bool member_now = inComponentView<C>(
                        reg,
                        e,
                        typename T::Require{},
                        typename T::Exclude{}
                    );
                    const auto* gpu = member_now
                        ? reg.template try_get<MeshGpuCacheComponent>(e)
                        : nullptr;
                    const bool intent_current =
                        gpu != nullptr &&
                        gpu->mesh_source == completion.context.mesh_id &&
                        gpu->material_source == completion.context.material_id &&
                        gpu->mesh == completion.context.mesh &&
                        gpu->material == completion.context.material;
                    const bool owner_alive =
                        !completion.abandoned && member_now && intent_current &&
                        !reg.template all_of<State>(e);
                    const bool succeeded =
                        !completion.dispatch_failed &&
                        reply.status == lux::render::MeshInstanceCreateStatus::Ok &&
                        static_cast<bool>(reply.object);

                    if (!succeeded)
                    {
                        if (reply.object)
                            leaving_.push_back(Leaving{
                                reply.object,
                                e,
                                completion.context.transition_milliseconds,
                                completion.context.transition_seed});
                        if (owner_alive)
                        {
                            auto& failure = rememberFailure(
                                e,
                                completion.context.mesh_id,
                                completion.context.material_id,
                                completion.context.mesh,
                                completion.context.material
                            );
                            if (completion.dispatch_failed)
                            {
                                const auto recovery = failure.dispatch_reported
                                    ? renderBridgeFailureRecovery(completion.error)
                                    : reportRenderBridgeFailure(
                                          "MeshInstanceSubsystem",
                                          "add mesh instance",
                                          completion.error
                                      );
                                failure.dispatch_reported = true;
                                failure.permanent = recovery !=
                                    lux::render::ERecovery::Retryable;
                                failure.retry_in = failure.permanent
                                    ? 0
                                    : kTransientRetryDrives;
                                return;
                            }

                            const bool malformed_success =
                                reply.status ==
                                    lux::render::MeshInstanceCreateStatus::Ok &&
                                !reply.object;
                            const bool transient =
                                reply.status ==
                                    lux::render::MeshInstanceCreateStatus::CapacityExhausted;
                            failure.permanent = !transient;
                            failure.retry_in = transient
                                ? kTransientRetryDrives
                                : 0;
                            if (!failure.reply_reported)
                            {
                                if (malformed_success)
                                {
                                    diagnoseRenderBridge(
                                        "[MeshInstanceSubsystem] add mesh instance "
                                        "returned success with a null object; "
                                        "identical input is latched"
                                    );
                                }
                                else
                                {
                                    diagnoseRenderBridge(
                                        "[MeshInstanceSubsystem] add mesh instance "
                                        "was refused (status %u); %s",
                                        static_cast<unsigned>(reply.status),
                                        transient
                                            ? "retrying after bounded backoff"
                                            : "identical input is latched"
                                    );
                                }
                                failure.reply_reported = true;
                            }
                        }
                        else if (member_now)
                        {
                            changes_.mark(e);
                        }
                        return;
                    }

                    if (!owner_alive)
                    {
                        leaving_.push_back(Leaving{
                            reply.object,
                            e,
                            completion.context.transition_milliseconds,
                            completion.context.transition_seed});
                        if (member_now)
                            changes_.mark(e);
                        return;
                    }

                    failed_.erase(e);
                    completion.context.object = reply.object;
                    reg.template emplace<State>(e, std::move(completion.context));
                }
            );
        }

        /// **归还路径的唯一入口。** 状态组件消失 = 这个实例不再属于任何实体。
        ///
        /// 两条路都到这里,而且句柄在信号触发时**仍然可读**
        /// (`reactive_storage_probe` ⑧ 是实证锚点):
        ///   · `registry.destroy(e)`      —— EnTT 逐组件发 on_destroy;
        ///   · `update()` 里的 `remove<State>` —— 离场/换代的安全点归还。
        ///
        /// ⚠️ 锚点必须是**本组件自己的** on_destroy。此前是
        ///    `on_destroy<MeshComponent>` + 回查侧表 —— 侧表在 registry 之外,
        ///    所以那样能读到。状态搬进组件池之后再那么写就危险了:
        ///    `registry.destroy` 清各池的**顺序不保证**,可能先清 State 再发
        ///    MeshComponent 的信号,句柄就读了个空。
        void onStateDestroyed(
            lux::meta::EntityRegistryBase& reg,
            lux::meta::entity_id e)
        {
            const auto& state = reg.get<State>(e);
            leaving_.push_back(Leaving{
                state.object,
                e,
                state.transition_milliseconds,
                state.transition_seed});
        }

        /// Single writer of the per-instance flags word: cast-shadow / visible from the
        /// component plus the highlight bit, so selection rides the same updateInstanceFlags
        /// diff path (no second writer clobbering it).
        static std::uint32_t instanceFlags(
            const C& c,
            bool highlighted,
            bool streaming) noexcept
        {
            std::uint32_t f       = lux::render::kInstanceFlagReceiveShadow;
            if (c.cast_shadow) f |= lux::render::kInstanceFlagCastShadow;
            if (c.visible)     f |= lux::render::kInstanceFlagVisible;
            if (highlighted)   f |= lux::render::kInstanceFlagHighlight;
            if (streaming)     f |= lux::render::kInstanceFlagStreamingFeedback;
            return f;
        }

    public:
        MeshInstanceSubsystem() = default;

        ~MeshInstanceSubsystem() override { leave_.detach(); detachStateSignal(); }

        /// 网格实例要三个 feature，骨骼网格再加一个：
        ///  · StandardMeshStack —— 每条实例命令(add/remove/可见性/标志/变换)都经它;
        ///  · StandardMaterial  —— 材质上传/改/销毁;
        ///  · Highlight         —— 实例标志位里的高亮位由它画出轮廓;
        ///  · Skinning          —— 仅骨骼网格(trait 有 beginFrame 的那一支)。
        [[nodiscard]] std::span<const std::string_view>
        renderFeatures() const noexcept override
        {
            if constexpr (requires { typename T::FrameState; })
            {
                static const std::string_view kFeatures[] = {
                    "StandardMeshStack", "StandardMaterial", "Highlight",
                    "StreamingFeedback", "Skinning" };
                return kFeatures;
            }
            else
            {
                static const std::string_view kFeatures[] = {
                    "StandardMeshStack", "StandardMaterial", "Highlight",
                    "StreamingFeedback" };
                return kFeatures;
            }
        }

        [[nodiscard]] std::span<const RenderSubsystemType>
        runsAfter() const noexcept override
        {
            static constexpr RenderSubsystemType kAfter[] = {
                renderSubsystemType<ResidencySubsystem>()
            };
            return kAfter;
        }

        void update(RenderSubsystemContext& uctx) override
        {
            auto& reg = uctx.registry();
            auto& ctx = uctx.render();
            auto& active_view = uctx.activeView();

            for (const auto& [e, failure] : failed_)
                if (!failure.permanent)
                    changes_.mark(e);

            // ★ 批 R3:**稳态快速出口**。静态场景下本节点应当接近零开销,而在这行
            //   之前它并不是 —— 下面那句 `ctx.meshStack()` 是一次**按字符串**查
            //   进程目录,每帧无条件做;`changes_.view()` 还要再构造一个横跨 5 个
            //   存储的视图。两样都是每帧固定成本,与实体数无关,但也与「有没有活干」
            //   无关。
            //
            //   骨骼策略除外:调色板每帧都要重新累加+上传,它没有「稳态」。
            if constexpr (!requires { T::beginFrame(frame_); })
            {
                const bool view_gen_stale =
                    active_view.view().isValid() &&
                    last_seen_view_gen_ != active_view.generation();
                if (changes_.empty() && !create_requests_.hasCompletions() &&
                    leaving_.empty() &&
                    to_unbind_.empty() && !view_gen_stale)
                {
#if !LUX_ECS_EXTRACTION_VERIFY
                    return;   // VERIFY 构型下不能走这条:oracle 每帧都要对拍
#endif
                }
            }

            if constexpr (requires { T::beginFrame(frame_); }) T::beginFrame(frame_);

            auto mesh = ctx.meshStack();   // feature-scoped instance commands (by value, cheap)

            // ① 回执只在这个安全点发布；离场、重入或资产换代后的旧实例
            //    直接进入归还队列，不会短暂挂回新意图。
            drainCreateCompletions(reg);

            // ② 摘掉离场实体的状态组件 —— 这一步**触发** on_destroy<State>,
            //    句柄由 onStateDestroyed 读进 leaving_,所以必须排在 ③ 之前。
            for (const auto e : to_unbind_)
                if (reg.valid(e) && reg.template all_of<State>(e))
                    reg.template remove<State>(e);
            to_unbind_.clear();

            // ③ 先还后借:排空离场队列。信号只把对象句柄读走记账 —— removeMeshInstance
            // 是渲染命令,必须在构建器开着的时候发,而实体销毁那一刻通常不是。
            // 顺手把仍然活着的实体身上的就绪观察点摘掉(实体销毁的场合组件已随实体没了)。
            for (const auto& lv : leaving_)
            {
                if (lv.transition_milliseconds != 0u &&
                    lv.transition_seed != 0u)
                {
                    mesh.retireMeshInstance({
                        .scene_id = ctx.scene(),
                        .object = lv.object,
                        .transition_milliseconds =
                            lv.transition_milliseconds,
                        .transition_seed = lv.transition_seed});
                }
                else
                {
                    mesh.removeMeshInstance({
                        .scene_id = ctx.scene(),
                        .object = lv.object});
                }
                if (reg.valid(lv.entity))
                    reg.template remove<MeshInstanceReadyComponent>(lv.entity);
            }
            leaving_.clear();

            // ④ 出图 view 换代 → 所有实例都要对新代次重发可见性。这是唯一
            //    「真的需要全量」的转换,而且极罕见。**排在排空之前**,好让重发
            //    落在本帧 —— 挪到之后就会晚一帧,那一帧新 view 上一个网格都没有。
            if (active_view.view().isValid() &&
                last_seen_view_gen_ != active_view.generation())
            {
                last_seen_view_gen_ = active_view.generation();
                changes_.markAll();
            }

            // ⑤ 逐实体处理。**驱动**在这里选:稳态下 `changes_` 是空的,
            //    于是这一整段一个实体都不碰(批 R2)。
            //    换出语义与 VERIFY 对拍都在 `drain` 里(批 R3 从本节点收上去的)。
            changes_.drain(reg, renderSubsystemType<MeshInstanceSubsystem>().name,
                [&](lux::meta::entity_id e, C& c)
                { return processEntity(reg, ctx, active_view, mesh, e, c); });

            // ★ 此前这是第二个相位 `flush(ctx)` —— `RenderSystem` 对**全部**子系统
            //   跑完 tick 之后再统一跑一遍 flush,也就是它内部的第二个全局 barrier。
            //
            //   折进本节点的 update 末尾是安全的:批处理的输入 `frame_` 完全来自
            //   **本节点自己**这一轮的累积(骨骼调色板只由带蒙皮的实例产生),不依赖
            //   任何别的节点的 tick。全仓另一个实现 flush 的是 Image2D(批 B4),
            //   它同样只用自己的每帧状态。于是那个 barrier 没有存在理由。
            if constexpr (requires { T::flush(frame_, ctx); }) T::flush(frame_, ctx);
        }

    private:
        /// 一个实体的全部维护:换代 → 可见性 → 标志位 → 变换 → 骨骼累加,
        /// 或者(还没有状态组件时)首见建实例。
        ///
        /// ★ 批 R2 把它从 `update` 的那个大 lambda 里抽出来,有两个理由:
        ///   ① 驱动可换 —— 全扫和变更集用**同一段逻辑**,不是两份实现;
        ///   ② 验证 oracle 需要能对同一个实体再跑一遍并问「还有活干吗」。
        ///
        /// @return 本次是否做了任何**实质动作**(发了命令 / 改了世界)。
        ///         VERIFY 构型靠这个判断反应式路径有没有漏信号。
        bool processEntity(
            lux::meta::EntityRegistry& reg,
            SceneRenderBinding&        ctx,
            ActiveRenderView&          active_view,
            auto&                      mesh,
            lux::meta::entity_id       e,
            C&                         c
        )
        {
            bool did_work = false;
            {
                const InstanceTransform xf = T::transform(e, reg, ctx);
                if (!xf.valid)
                    return false;

                // 网格与材质由资源子系统解析。组件不在 = 还没就绪（或作者没设网格）——
                // 已经建好的实例不受影响（下面 live 分支照常维护），只是**建不了新的**。
                const auto* gpu = reg.template try_get<MeshGpuCacheComponent>(e);

                // ★ 批 R1:此前是 `instances_.find(e)` —— 一次哈希 + 指针跳转。
                //   现在是稀疏集索引。R2 会连这一次查找也去掉(状态组件直接进 view)。
                if (State* const state = reg.template try_get<State>(e))
                {
                    State& inst = *state;

                    // Runtime asset-id swap: tear down so next frame's first-sight path
                    // rebuilds with the new ids (one-frame gap).
                    // 判据是 cache 组件里的**来源 id**,不是组件字段:作者改了字段之后
                    // 新资产要过若干帧才就绪,期间应当继续画旧的 —— 拿字段判会立刻拆掉
                    // 实例然后空等,画面闪一下。资产没了(组件消失)也算换:拆掉。
                    // 句柄比对(热更新批5):内容变更是「id 不变、缓存重建、句柄变」,
                    // 只比 id 看不见 —— 实例会拿着已销毁的旧句柄继续画。
                    const bool swapped =
                        gpu == nullptr ||
                        inst.mesh_id != gpu->mesh_source || inst.material_id != gpu->material_source ||
                        inst.mesh != gpu->mesh || inst.material != gpu->material;
                    if (swapped)
                    {
                        // ★ 批 R1:此前这里当场发 removeMeshInstance + 擦侧表。
                        //   现在只摘状态组件 —— on_destroy<State> 把句柄读进
                        //   `leaving_`,命令在**下一次 tick 的 ③** 发出。归还只有
                        //   一个出口,不存在「这条路径忘了发 remove」的错法。
                        //   往**别的** pool 动手而当前迭代的是 C 的 pool,
                        //   EnTT 下安全(与资源解析器同款)。
                        reg.template remove<State>(e);
                        // 实例没了,「就绪」观察点跟着摘 —— 重建走 first-sight,
                        // 可见性重发时再挂回来。
                        reg.template remove<MeshInstanceReadyComponent>(e);
                        return true;
                    }
                    // view 尚未落地(相机的 addView 回复还没回来)时不发 —— 发了也是
                    // 对一个无效句柄发。下一帧代次不匹配依旧成立,会自动补上。
                    if (active_view.view().isValid() &&
                        inst.visible_for_view_gen != active_view.generation())
                    {
                        mesh.makeInstanceVisibleForView({.scene_id = ctx.scene(), .view = active_view.view(), .object = inst.object});
                        inst.visible_for_view_gen = active_view.generation();
                        did_work = true;
                        // 就绪观察点:实例已建成(live)且对**当前**代次发过可见性。
                        // 缩略图 readback 等的正是这一刻(组件头有完整语义)。
                        if (!reg.template all_of<MeshInstanceReadyComponent>(e))
                            reg.template emplace<MeshInstanceReadyComponent>(e);
                    }
                    const std::uint32_t flags = instanceFlags(
                        c,
                        reg.template all_of<HighlightedComponent>(e),
                        reg.template all_of<AssetStreamingStateComponent>(e));
                    if (flags != inst.last_flags)
                    {
                        mesh.updateInstanceFlags({.scene_id = ctx.scene(), .object = inst.object, .flags = flags});
                        inst.last_flags = flags;
                        did_work = true;
                    }
                    if (!sameTransform(inst.last_transform, xf.spatial))
                    {
                        updateTransform(
                            mesh, ctx.scene(), inst.object, xf.spatial);
                        inst.last_transform = xf.spatial;
                        did_work = true;
                    }

                    // ⚠️ 骨骼调色板累加**不算 did_work**:它每帧对每个蒙皮实例
                    //    都要做,不是「变化驱动」的东西。把它算进去会让验证 oracle
                    //    对骨骼节点永远报差异。骨骼策略的全量遍历见 update 的说明。
                    if constexpr (requires { T::accumulate(frame_, inst.object, inst.mesh, e, reg); })
                        T::accumulate(frame_, inst.object, inst.mesh, e, reg);
                    return did_work;
                }
                if (create_requests_.contains(e)) return did_work;

                // No resolved generation exists yet. Keep an old failure record
                // dormant until residency publishes the next exact GPU intent.
                if (gpu == nullptr) return did_work;

                // G-05: a prior create replied failure. Skip re-issuing unless the
                // situation changed: source ids OR resolved handles differ
                // (including same-id hot reload) → retry;
                // a config error otherwise stays skipped; a capacity error retries after
                // a bounded backoff.
                if (auto fit = failed_.find(e); fit != failed_.end())
                {
                    FailRecord& fr = fit->second;
                    if (fr.mesh_id != gpu->mesh_source ||
                        fr.material_id != gpu->material_source ||
                        fr.mesh != gpu->mesh ||
                        fr.material != gpu->material)
                        failed_.erase(fit);        // intent generation changed → retry below
                    else if (fr.permanent)
                        return did_work;           // permanent (config / protocol) → keep skipping
                    else if (fr.retry_in > 0)
                    {
                        --fr.retry_in;
                        return did_work;           // retryable: still in backoff
                    }
                    // Keep the record during the retry so its diagnostic bits
                    // remain stable across request generations.
                }

                // First sight: 资产由资源子系统解析好了才有这个组件。没有 = 还没就绪
                // （网格没上传完、或作者指定的材质还没好）→ 下一帧再看。
                // 此前这里是两次 `ensure*` + 一段「材质是 nil 还是没好」的判断 ——
                // 那个判断现在做在解析器里一次，组件在 = 两者都可用。
                const lux::render::RMeshHandle     m   = gpu->mesh;
                const lux::render::RMaterialHandle mat = gpu->material;

                const std::uint32_t flags = instanceFlags(
                    c,
                    reg.template all_of<HighlightedComponent>(e),
                    reg.template all_of<AssetStreamingStateComponent>(e));
                // Capture asset ids so the reply can store them (the source component
                // may be gone by reap time). 取自 cache 组件 —— 与上面的换资产判据同源。
                const auto mesh_id     = gpu->mesh_source;
                const auto material_id = gpu->material_source;
                const auto transition = visualTransitionOf(reg, e);
                const std::uint32_t transition_milliseconds =
                    transition.duration_milliseconds;
                const std::uint32_t transition_seed = transition.seed;
                State intent{
                    {}, m, mat, 0u, mesh_id, material_id, flags,
                    transition_milliseconds,
                    transition_seed,
                    xf.spatial
                };
                const auto started = create_requests_.start(
                    e,
                    std::move(intent),
                    [&]()
                    {
                        return addMeshInstance(
                            mesh,
                            ctx.scene(),
                            m,
                            mat,
                            xf.spatial,
                            flags,
                            T::geometry,
                            lux::render::kPassMaskOpaqueDefault,
                            ~0u,
                            transition_milliseconds,
                            transition_seed
                        );
                    }
                );
                if (started == ETrackedRequestStart::STARTED)
                    did_work = true;
                else if (started == ETrackedRequestStart::INVALID_REQUEST)
                {
                    auto& failure = rememberFailure(
                        e,
                        mesh_id,
                        material_id,
                        m,
                        mat
                    );
                    failure.permanent = true;
                    failure.retry_in = 0;
                    if (!failure.reply_reported)
                    {
                        diagnoseRenderBridge(
                            "[MeshInstanceSubsystem] add mesh instance produced "
                            "an invalid request; identical input is latched"
                        );
                        failure.reply_reported = true;
                    }
                }
            }
            return did_work;
        }


    public:
        void onAdded(const SystemSetupContext& setup) override
        {
            auto& reg = setup.registry();
            leave_.attach(reg, [this](lux::meta::entity_id e) { onLeave(e); });
            attachStateSignal(reg);
            attachChangeSources(reg);
        }
        void onRemoved(const SystemRemovalContext&) override
        {
            leave_.detach();
            detachStateSignal();
            changes_.detach();
        }

    private:
        /// ★ 批 R2:声明「什么算这个实体需要重新处理」。
        ///
        /// 逐条显式列出,不藏在某个类型参数里 —— 漏一条的后果是
        /// **那一类变化下游永远收不到**,而它不报错,只表现为某个东西不更新。
        /// 显式列表至少让漏掉的那条是**读得出来**的。
        ///
        /// 与 `T::Require` / `T::Exclude` 一起展开,所以两个实例化(静态 / 骨骼)
        /// 各自听自己的伴随组件,不必为策略再写一份。
        ///
        /// `attach` 跑完这段之后**立即折入存量** —— 见 ExtractionChangeSet 的
        /// 类注释:那是这一层包装存在的全部理由。
        template <class Set, class... Rs, class... Es>
        static void declareSources(Set& s, ComponentList<Rs...>, ComponentList<Es...>)
        {
            // 集合归属本身变了。
            s.template on_construct<C>();
            s.template on_update   <C>();          // visible / cast_shadow 等字段
            (s.template on_construct<Rs>(), ...);
            (s.template on_update   <Rs>(), ...);  // 世界变换移动即经此(R0 的 patch)
            (s.template on_destroy  <Es>(), ...);  // 排除标签消失 = 重新入场
            // 资产解析结果:出现 = 可以建实例;变 = 换代;消失 = 拆实例。
            s.template on_construct<MeshGpuCacheComponent>();
            s.template on_update   <MeshGpuCacheComponent>();
            s.template on_destroy  <MeshGpuCacheComponent>();
            // 高亮是标签的增删,本就发结构性信号。
            s.template on_construct<HighlightedComponent>();
            s.template on_destroy  <HighlightedComponent>();
            s.template on_construct<AssetStreamingStateComponent>();
            s.template on_update   <AssetStreamingStateComponent>();
            s.template on_destroy  <AssetStreamingStateComponent>();
            // 自己的状态组件。两个方向都要听:
            //   · construct —— 实例**刚变 live**(回执落地后由 update ① 装上)。
            //     它需要一次初始可见性下发 + 挂就绪观察点。⚠️ 漏了这一条,
            //     实例建好之后再也没有任何信号提到它,可见性永远不发 ——
            //     **画面上就是什么都不出现,且零报错**。验证 oracle 抓到的就是它
            //     (`state=1 cache=1 pending=0 marked=0`:有实例、没被标记、还有活干)。
            //   · destroy   —— 换资产拆掉 / 离场 → 下一轮重新考虑首见。
            s.template on_construct<State>();
            s.template on_destroy  <State>();
        }

        void attachChangeSources(lux::meta::EntityRegistry& reg)
        {
            changes_.attach(reg,
                static_cast<entt::id_type>(renderSubsystemType<MeshInstanceSubsystem>().hash),
                [](auto& s)
                { declareSources(s, typename T::Require{}, typename T::Exclude{}); });
        }

        /// 归还信号的连接/解绑。**不折入存量**:本组件只由本子系统 emplace,
        /// 连接发生在节点装配时、任何实例建成之前 —— 不存在「连接之前就已经
        /// 有状态组件」的实体。(这与 `connectAndSeed` 要解决的问题不同:
        /// 那里的源组件是**别人**写的,可能早就在了。)
        void attachStateSignal(lux::meta::EntityRegistry& reg)
        {
            if (reg_ == &reg) return;
            detachStateSignal();
            reg_ = &reg;
            reg.template on_destroy<State>()
               .template connect<&MeshInstanceSubsystem::onStateDestroyed>(*this);
        }
        void detachStateSignal()
        {
            if (!reg_) return;
            reg_->template on_destroy<State>()
                .template disconnect<&MeshInstanceSubsystem::onStateDestroyed>(*this);
            reg_ = nullptr;
        }

    public:

        // 本节点不需要额外的 `releaseAll()`：实例属于 scene，由
        // scene lease 整体回收；跨场景 mesh/material 兴趣票属于
        // ResidencySubsystem；在途 create 由 `TrackedRenderRequest` 的词法
        // owner 取消。因此成员析构就是完整的本地收场。
    };
} // namespace lux::ecs

#pragma once
/**
 * @file PooledSlotSubsystem.hpp
 * @brief 「池化槽位」形状的子系统基类(Directional/Point/Spot 光的基底): each
 *        entity owns a pooled render object created/updated/destroyed by handle.
 *        Written once; the per-feature policy `Traits` supplies `Component` /
 *        `Desc` / `Handle` / `Reply` / `feature` / `Ops` / `extract` /
 *        `create·update·destroy` / `handle`. The async create-pending + bit-equal
 *        update-on-change + destroy-on-leave lifecycle lives here. 具名子系统 =
 *        策略结构体 + 别名:`using PointLightSubsystem = PooledSlotSubsystem<PointLightRenderPolicy>;`。
 *
 * ── 离场从「每帧全扫」改成「信号 + 排空」（新工作线阶段 1）────────────────────
 *
 * 此前 `reap` 每帧遍历 `live_` 的**每一个**活实例，对每个做一次 `inComponentView`
 * 成员测试，只为找出（通常是零个）离场的。现在由 `ComponentSetLeaveObserver` 在实体
 * 真正离开组件集时通知一次：**O(活实例 × 帧) → O(结构性变化)**。
 *
 * 形状是「观察者只记意图 + 安全点排空」，与批 3 的 `CameraViewSubsystem` 同款，理由也
 * 一样，只是这里多一条**渲染特有**的：destroy 是一条渲染命令，必须在帧构建器开着的
 * 时候发；而信号是在 `registry.destroy(e)` 那一刻派发的，那通常不在 `update()` 里。
 * 所以观察者只把句柄从 `live_` 挪进 `leaving_`，命令留到下一次 `update` 安全点发。
 *
 * ⚠️ 三处**静默漏资源**的坑，都在下面各自的现场标了：
 *   ① 离场时 create 还在途（句柄还没回来，`live_` 里没有它可挪）
 *   ② 离场后又回来（同一实体先 leave 再 enter）
 *   ③ 排空的时机（`update` 只在节点仍被 schedule 驱动时执行）
 */

#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <lux/engine/meta/LuxObject.hpp>   // entity_id / EntityRegistry
#include <lux/engine/ecs/render/VisualTransition.hpp>

#include <lux/engine/ecs/render/IRenderSubsystem.hpp>
#include "lux/engine/ecs/render/SceneRenderBinding.hpp"
#include "lux/engine/ecs/render/RenderViewUtil.hpp"
#include "lux/engine/ecs/render/TrackedRenderRequest.hpp"

namespace lux::ecs
{
    template <class Traits>
    /// ★ 批 B3 起它是一个**普通的 schedule node**(`ISystem`)。渲染绑定由构造
    ///   注入,不再每帧由 `RenderSystem` 的调度循环递进来。
    ///
    /// ── 请求生命周期边界 ────────────────────────────────────────
    ///
    /// ECS 节点不直接安装 `RenderRequest::then`。`TrackedRenderRequest`
    /// 是唯一边界:它先把 `ScopedRenderRequest` 收进词法所有者,
    /// 续体只投递一条带串号的节点私有完成记录。组件集检查、
    /// 句柄收编与渲染命令都只在 `update()` 安全点的 `drain()` 发生。
    ///
    /// 多步跨帧编排仍由 runtime/execution 的 stdexec sender + AsyncScope
    /// 承担;这里的 tracker 只表达「一个 ECS owner 等一个 RPC 回执」,
    /// 避免把 stdexec 的重模板头拉进 ECS 公开头。架构门禁按文件
    /// 白名单锁住这个边界,新业务节点无法再直接添加续体。
    class PooledSlotSubsystem final : public IRenderSubsystem
    {
        using T = Traits;
        using C = typename Traits::Component;
        struct Live
        {
            typename T::Handle handle{};
            typename T::Desc   last_sent{};
            std::uint32_t      transition_milliseconds{0u};
        };
        std::unordered_map<lux::meta::entity_id, Live> live_;
        TrackedRenderRequest<
            lux::meta::entity_id,
            typename T::Reply,
            typename T::Desc> create_requests_;

        /// A failed create remains latched for the exact descriptor that caused
        /// it.  Changed authored data clears the latch immediately.  Retryable
        /// failures stay in the table while counting down, so their first-error
        /// diagnostic is not re-emitted on every retry generation.
        struct FailRecord
        {
            typename T::Desc intent{};
            bool             permanent{false};
            bool             dispatch_reported{false};
            bool             reply_reported{false};
            int              retry_in{0};
        };
        std::unordered_map<lux::meta::entity_id, FailRecord> failed_;
        static constexpr int kTransientRetryDrives = 120;

        /// 已离场、destroy 命令还没发出去的句柄。观察者填，`update` 开头排空。
        /// 存句柄而不是实体 id：排空时实体多半已经不存在了，回头查是查不到的
        /// （这正是 `on_destroy` 里必须**当场把句柄读走**的原因，CLAUDE.md 有条同款
        /// 规矩讲 EnTT 没有 cleanup 组件留存机制）。
        struct Leaving final
        {
            typename T::Handle handle{};
            std::uint32_t transition_milliseconds{0u};
        };
        std::vector<Leaving> leaving_;

        ComponentSetLeaveObserver<C, typename T::Require, typename T::Exclude> leave_;

    public:
        PooledSlotSubsystem() = default;

        ~PooledSlotSubsystem() override { leave_.detach(); }

        void onAdded(const SystemSetupContext& setup) override
        {
            leave_.attach(setup.registry(),
                          [this](lux::meta::entity_id e) { onLeave(e); });
        }

        void onRemoved(const SystemRemovalContext&) override { leave_.detach(); }

        /// 本节点驱动的那个 feature（Light）—— 由 trait 声明,不在这里写死。
        [[nodiscard]] std::span<const std::string_view>
        renderFeatures() const noexcept override
        {
            static const std::string_view kFeatures[] = { T::feature };
            return kFeatures;
        }

        void update(RenderSubsystemContext& uctx) override
        {
            auto& reg = uctx.registry();
            auto& ctx = uctx.render();
            const auto ops = ctx.features().template ops<typename T::Ops>(T::feature);

            create_requests_.drain(
                [this, &reg, &ctx](auto completion)
                {
                    const auto handle = T::handle(completion.reply);
                    const bool succeeded =
                        !completion.dispatch_failed &&
                        completion.reply.status == 0 &&
                        !handle.isNull();
                    const bool owner_alive =
                        !completion.abandoned &&
                        inComponentView<C>(
                            reg,
                            completion.key,
                            typename T::Require{},
                            typename T::Exclude{}
                        ) &&
                        !live_.contains(completion.key);

                    if (succeeded && owner_alive)
                    {
                        failed_.erase(completion.key);
                        live_.emplace(
                            completion.key,
                            Live{
                                handle,
                                std::move(completion.context),
                                transitionMilliseconds(
                                    reg,
                                    completion.key)}
                        );
                        return;
                    }
                    if (!handle.isNull())
                        leaving_.push_back(Leaving{handle, 0u});

                    if (!owner_alive)
                        return;

                    const bool malformed_success =
                        !completion.dispatch_failed &&
                        completion.reply.status == 0 &&
                        handle.isNull();
                    FailRecord& failure = rememberFailure(
                        completion.key,
                        completion.context
                    );

                    if (completion.dispatch_failed)
                    {
                        // The RenderError is the source of truth for retry
                        // policy.  Report only the first failure for unchanged
                        // intent; subsequent retry generations keep this record.
                        const auto recovery = failure.dispatch_reported
                            ? renderBridgeFailureRecovery(completion.error)
                            : reportRenderBridgeFailure(
                                  T::feature,
                                  "create pooled slot",
                                  completion.error
                              );
                        failure.dispatch_reported = true;
                        failure.permanent =
                            recovery != lux::render::ERecovery::Retryable;
                        failure.retry_in = failure.permanent
                            ? 0
                            : kTransientRetryDrives;
                        return;
                    }

                    // The light protocol currently collapses capacity and
                    // server-side rejection into status != 0.  It is therefore
                    // unsafe to declare it permanent; bounded retry preserves
                    // recovery without a per-frame command loop.  Conversely,
                    // "success + null handle" violates the reply invariant and
                    // cannot heal with identical input.
                    failure.permanent = malformed_success;
                    failure.retry_in = malformed_success
                        ? 0
                        : kTransientRetryDrives;
                    if (!failure.reply_reported)
                    {
                        if (malformed_success)
                        {
                            diagnoseRenderBridge(
                                "[{}] create pooled slot returned success with "
                                "a null handle; identical input is latched",
                                T::feature
                            );
                        }
                        else
                        {
                            diagnoseRenderBridge(
                                "[{}] create pooled slot was rejected (status {}); "
                                "retrying after {} drives",
                                T::feature,
                                static_cast<unsigned>(completion.reply.status),
                                kTransientRetryDrives
                            );
                        }
                        failure.reply_reported = true;
                    }
                }
            );

            // 先还后借：排空离场队列，池子的槽位先腾出来再收本帧的新建。
            // ⚠️ 坑③：这是单个 destroy 命令的唯一出口。节点已从 schedule
            //   移除后不会再有 update；正常场景拆解会立即 destroyScene，
            //   由 scene ownership 整体回收这些对象，因此不需要在析构期发命令。
            for (const auto& leaving : leaving_)
            {
                if constexpr (requires {
                    T::retire(
                        ctx.session(),
                        ops,
                        ctx.scene(),
                        leaving.handle,
                        leaving.transition_milliseconds);
                })
                {
                    T::retire(
                        ctx.session(),
                        ops,
                        ctx.scene(),
                        leaving.handle,
                        leaving.transition_milliseconds);
                }
                else
                {
                    T::destroy(
                        ctx.session(),
                        ops,
                        ctx.scene(),
                        leaving.handle);
                }
            }
            leaving_.clear();

            // Require/Exclude companions (Point/Spot require ResolvedTransformComponent).
            auto view = componentView<C>(reg, typename T::Require{}, typename T::Exclude{});
            view.each([&](lux::meta::entity_id e, const C& c, auto&&...)
            {
                std::optional<typename T::Desc> extracted = T::extract(
                    e,
                    c,
                    reg,
                    ctx
                );
                if (!extracted)
                    return;
                const auto& d = *extracted;

                if (auto it = live_.find(e); it != live_.end())
                {
                    if (std::memcmp(&it->second.last_sent, &d, sizeof(d)) != 0)
                    {
                        T::update(ctx.session(), ops, ctx.scene(), it->second.handle, d);
                        it->second.last_sent = d;
                    }
                    return;
                }
                if (create_requests_.contains(e)) return;

                if (auto failed = failed_.find(e); failed != failed_.end())
                {
                    // A permanent failure is permanent only for the descriptor
                    // that produced it.  Authored changes are a new intent and
                    // get one immediate attempt.
                    if (std::memcmp(
                            &failed->second.intent,
                            &d,
                            sizeof(d)
                        ) != 0)
                    {
                        failed_.erase(failed);
                    }
                    else
                    {
                        if (failed->second.permanent)
                            return;
                        if (failed->second.retry_in > 0)
                        {
                            --failed->second.retry_in;
                            return;
                        }
                    }
                }

                const auto transition_milliseconds =
                    transitionMilliseconds(reg, e);
                const auto started = create_requests_.start(
                    e,
                    d,
                    [&ctx, &ops, &d, transition_milliseconds]()
                    {
                        if constexpr (requires {
                            T::create(
                                ctx.session(),
                                ops,
                                ctx.scene(),
                                d,
                                transition_milliseconds);
                        })
                        {
                            return T::create(
                                ctx.session(),
                                ops,
                                ctx.scene(),
                                d,
                                transition_milliseconds);
                        }
                        else
                        {
                            return T::create(
                                ctx.session(), ops, ctx.scene(), d);
                        }
                    }
                );
                if (started == ETrackedRequestStart::INVALID_REQUEST)
                {
                    FailRecord& failure = rememberFailure(e, d);
                    failure.permanent = true;
                    failure.retry_in = 0;
                    if (!failure.reply_reported)
                    {
                        diagnoseRenderBridge(
                            "[{}] create pooled slot produced an invalid request; "
                            "identical input is latched",
                            T::feature
                        );
                        failure.reply_reported = true;
                    }
                }
            });
        }

        // ★ 批 C2 删掉了 `releaseAll()` —— 完整理由见 `Image2DSubsystem` 同位置
        //   (零调用者 + 函数体与成员析构等价 + 不发渲染命令所以无需安全点)。
        //   池化对象(灯)同样 scene-owned:随 destroyScene 一起没,逐个 destroy
        //   是多余的命令 —— `leaving_` 里那些也一样,所以析构直接丢掉是对的。

    private:
        [[nodiscard]] static std::uint32_t transitionMilliseconds(
            const lux::meta::EntityRegistry& registry,
            lux::meta::entity_id entity) noexcept
        {
            if constexpr (requires { T::supports_visual_transition; })
            {
                if constexpr (T::supports_visual_transition)
                {
                    return visualTransitionOf(
                        registry, entity).duration_milliseconds;
                }
            }
            return 0u;
        }

        [[nodiscard]] FailRecord& rememberFailure(
            lux::meta::entity_id e,
            const typename T::Desc& intent
        )
        {
            auto [it, inserted] = failed_.try_emplace(
                e,
                FailRecord{intent, false, false, false, 0}
            );
            if (!inserted &&
                std::memcmp(&it->second.intent, &intent, sizeof(intent)) != 0)
            {
                it->second = FailRecord{intent, false, false, false, 0};
            }
            return it->second;
        }

        /// 观察者回调。**只记账，不发命令**（理由见 `ComponentSetLeaveObserver` 的注释：
        /// 此刻帧构建器多半没开，就地发命令会被静默丢掉）。
        void onLeave(lux::meta::entity_id e)
        {
            failed_.erase(e);
            if (auto it = live_.find(e); it != live_.end())
            {
                leaving_.push_back(Leaving{
                    it->second.handle,
                    it->second.transition_milliseconds});
                live_.erase(it);
                // ⚠️ 坑②：离场后又回来。`live_` 里已经没有它，下一次 update 的首见分支
                //   会重新 create，旧句柄在 leaving_ 里照样被还掉，两者互不干扰。
                //   （所以这里**不能**只标记不删：留在 live_ 里会让 update 以为它还在，
                //   既不重建也不还，那盏灯就此僵死。）
                return;
            }
            (void)create_requests_.abandon(e);
            // 都不在 → 本子系统从没为这个实体记过账。②③ 两条路径会为集合外的实体触发
            // （`on_destroy<ResolvedTransform3DComponent>` 对每个有变换的实体都响），
            // 这里的两次哈希查找就是过滤器。
        }
    };

} // namespace lux::ecs

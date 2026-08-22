#pragma once
/**
 * @file FeatureParamSubsystem.hpp
 * @brief 「特性参数」形状的子系统基类(Grid2D/Grid3D 的基底):组件字段 →
 *        feature 的一条 SetParams op,dirty-diffed. Written once; the per-feature
 *        policy `Traits` supplies only `Component` / `feature` name / `Ops` /
 *        `Payload` / `extract` / `push`. Lifecycle (resolve feature + view +
 *        bit-equal dirty compare + 离场清理) lives here. It is only suitable
 *        for parameters whose absence requires no renderer-side clear command.
 */

#include <cstring>
#include <optional>
#include <unordered_map>

#include <lux/engine/ecs/Registry.hpp>   // entity_id / EntityRegistry

#include <lux/engine/ecs/render/IRenderSubsystem.hpp>
#include "lux/engine/ecs/render/SceneRenderBinding.hpp"
#include "lux/engine/ecs/render/RenderViewUtil.hpp"

namespace lux::ecs
{
    /// ★ 批 B3 起它是一个**普通的 schedule node**(`ISystem`)。
    ///   本形状没有异步 create、没有 per-entity 的远端对象,所以既不需要
    ///   releaseRefs 也不需要延迟命令 —— 离场就地 erase 脏比较缓存即可。
    template <class Traits>
    class FeatureParamSubsystem final : public IRenderSubsystem
    {
        using T = Traits;
        using C = typename Traits::Component;
        std::unordered_map<lux::ecs::Entity, typename T::Payload> last_;

        /// 实体离场 → 把它的脏比较缓存删掉。
        ///
        /// 本形状的离场**不发任何渲染命令**（它推的是 feature 的 SetParams，是
        /// 场景级状态，没有 per-entity 的对象要销毁），所以这里可以在观察者里
        /// 就地 erase —— 那是纯本地记账，不改世界、不碰构建器。这也是它与
        /// `PooledSlotSubsystem` / `MeshInstanceSubsystem` 不同的地方：那两个必须
        /// 排队到 tick 才发命令。
        ComponentSetLeaveObserver<C, ComponentList<>, ComponentList<>> leave_;

    public:
        FeatureParamSubsystem() = default;

        ~FeatureParamSubsystem() override { leave_.detach(); }

        /// 本节点驱动的那个 feature（Grid2D / Grid3D / Skybox）—— trait 说了算。
        [[nodiscard]] std::span<const std::string_view>
        renderFeatures() const noexcept override
        {
            static const std::string_view kFeatures[] = { T::feature };
            return kFeatures;
        }

        [[nodiscard]] std::span<const RenderSubsystemType>
        runsAfter() const noexcept override
        {
            if constexpr (requires { T::runsAfter(); })
                return T::runsAfter();
            return {};
        }

        void update(RenderSubsystemContext& uctx) override
        {
            auto& reg = uctx.registry();
            auto& ctx = uctx.render();
            const auto feat = ctx.features().handle(T::feature);
            if (!feat.isValid()) return;   // feature absent in this scene → no-op (graceful)
            const auto ops = ctx.features().template ops<typename T::Ops>(T::feature);

            reg.template view<C>().each([&](lux::ecs::Entity e, const C& c)
            {
                std::optional<typename T::Payload> p = T::extract(e, c, reg, ctx, feat);
                if (!p) return;                          // not ready this frame (e.g. texture pending) → skip
                if (auto it = last_.find(e); it != last_.end())
                {
                    if (std::memcmp(&it->second, &*p, sizeof(*p)) == 0)
                        return;                          // unchanged → steady state, emit nothing
                    T::push(ctx.session(), ops, *p);
                    // ★ 这里此前有个 `onReplaced` 钩子:载荷里钉着客户端缓存资源
                    //   (天空盒那张 direct texture)的 trait 在这里回收旧的。资源
                    //   引用计数归资源子系统之后没有 trait 再实现它,连同 reap 里的
                    //   `onDropped` 一起删了 —— 留着一个零实现的检测点只是噪音。
                    it->second = *p;
                    return;
                }
                T::push(ctx.session(), ops, *p);
                last_.emplace(e, *p);
            });
        }

        void onAdded(const SystemSetupContext& setup) override
        {
            leave_.attach(setup.registry(),
                          [this](lux::ecs::Entity e) { last_.erase(e); });
        }
        void onRemoved(const SystemRemovalContext&) override { leave_.detach(); }

        [[nodiscard]] bool supportsDynamicRemoval() const noexcept override
        {
            return true;
        }

        // ★ 这里此前有 `releaseRefs`,它做两件事:调 `Traits::clear(ctx)` 与清
        //   脏比较缓存。两件都删了。
        //
        //   `clear` 的理由写着「so a REUSED scene does not keep sampling」——
        //   **场景复用早就没了**(`IRenderSubsystem` 自己的注释也写着那条
        //   drain-don't-cancel 协议「existed only because the editor once reused
        //   one render scene across swaps」)。生产里唯一的调用者是
        //   `SceneRuntime::tearDown`,紧接着就 `scene_lease_.close()`——
        //   往一个正在销毁的场景推参数,可证明无效果。
        //
        //   脏比较缓存随本对象析构 —— 那本来就是纯本地记账。
    };

} // namespace lux::ecs

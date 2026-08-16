#pragma once
// ============================================================================
//  ComponentChangeSet.hpp — 「谁需要重新处理」的实体集合(变更驱动的入口)。
//
//  ── 它在这一层的理由 ────────────────────────────────────────────────────
//
//  最初(批 R2)写在 `ecs/render/RenderViewUtil.hpp`,因为当时只有渲染抽取
//  节点用它。批 T2 起变换系统也要用,而变换系统在 `ecs/core` —— 够不到 render。
//  **第二个消费者出现在更低的层,就是该下沉的时候**,不是提前抽象。
//
//  ── 它自己不实现任何机制 ────────────────────────────────────────────────
//
//  这是 `entt::reactive` storage 的**薄包装**。去重、连续迭代、实体销毁时
//  自动摘除、消费时与组件集合求交 —— 全部由 EnTT 提供,逐条实测在
//  `ecs/core/test/reactive_storage_probe.cpp`(14 条断言,含派生 registry
//  的绑定、`on_update` 只认 patch、池表扩容后 storage 地址不变)。
//
//  包这一层只为两件 EnTT 不做、而做错了**不报错**的事,见 attach / drain。
// ============================================================================

#include <lux/engine/ecs/EcsVerify.hpp>   // LUX_ECS_EXTRACTION_VERIFY(唯一真相源)
#include <lux/engine/meta/LuxObject.hpp>  // EntityRegistry / entt::exclude

#include <algorithm>
#include <string_view>
#include <type_traits>
#include <vector>

#if LUX_ECS_EXTRACTION_VERIFY
#  include <cstdio>
#endif

namespace lux::ecs
{
    /// 伴随/排除组件的声明式清单 —— 展开成
    /// `registry.view<C, Require...>(entt::exclude<Exclude...>)`。
    template <class... Cs> struct ComponentList {};

    /// 主组件 C + 它的 Require/Exclude 清单构成的 entt view。
    /// 调用形如 `componentView<C>(reg, Require{}, Exclude{})`。
    template <class C, class... Rs, class... Es>
    [[nodiscard]] inline auto componentView(
        lux::meta::EntityRegistry& reg, ComponentList<Rs...>, ComponentList<Es...>)
    {
        return reg.template view<C, Rs...>(entt::exclude<Es...>);
    }

    /// @tparam C   主组件；@tparam Req/Exc  与 `componentView` 同一套集合语义。
    template <class C, class Req, class Exc> class ExtractionChangeSet;

    template <class C, class... Rs, class... Es>
    class ExtractionChangeSet<C, ComponentList<Rs...>, ComponentList<Es...>>
    {
        using Storage = std::remove_reference_t<
            decltype(std::declval<lux::meta::EntityRegistry&>()
                         .template storage<entt::reactive>(entt::id_type{}))>;

        lux::meta::EntityRegistry* reg_{nullptr};
        /// ★ attach 时缓存。此前每次 `view()`/`clear()`/`empty()`/`markAll()` 都要
        ///   `registry.storage<reactive>(id)` 查一次哈希 —— 每帧 3~4 次,不论有没有
        ///   活干都要付。缓存的前提(池表扩容不搬动已有 storage)由探针 ⑨ 断言。
        Storage*                   storage_{nullptr};
        std::vector<lux::meta::entity_id> scratch_;

        [[nodiscard]] Storage& storage() const { return *storage_; }

    public:
        /// **声明信号源 + 折入存量,绑成一次调用。**
        ///
        /// EnTT 的 reactive storage 起始为空,只对**连接之后**发生的事说话
        /// (探针 ⑥ 是实证锚点)。而自然的装配写法是「建实体 → 挂组件 → 让系统
        /// 开始工作」,连接往往发生在最后一步 —— 组件早就 emplace 了,
        /// `on_construct` 永远不会为它触发,那些实体**从此不再被处理**。
        ///
        /// 这个错法**不报错**:构建全绿、测试全过、进程干净退出,只表现为
        /// 「某些东西不渲染/不动」。CLAUDE.md 把它列为铁律,现场见
        /// `HierarchyIndex.hpp::ensureHierarchyIndex`。
        ///
        /// 所以这里**不提供「只连接」的入口** —— 「连了但没折」在 API 上不可表达。
        ///
        ///     changes_.attach(reg, "MeshInstance"_hs, [](auto& s) {
        ///         s.template on_construct<MeshGpuCacheComponent>()
        ///          .template on_update   <MeshGpuCacheComponent>();
        ///     });
        template <class Declare>
        void attach(lux::meta::EntityRegistry& reg, entt::id_type id, Declare&& declare)
        {
            reg_     = &reg;
            storage_ = &reg.template storage<entt::reactive>(id);
            auto& s  = storage();
            std::forward<Declare>(declare)(s);
            // 折入存量 —— 这一行就是本类存在的一半理由。
            componentView<C>(reg, ComponentList<Rs...>{}, ComponentList<Es...>{})
                .each([&s](lux::meta::entity_id e, auto&&...) { s.emplace(e); });
        }

        void detach()
        {
            if (!reg_) return;
            storage().reset();   // 释放全部信号连接
            storage().clear();
            reg_     = nullptr;
            storage_ = nullptr;
        }

        /// 待处理实体 ∩ 组件集合。**求交发生在消费时**,所以「标记之后又离开
        /// 集合」的实体自然被跳过,不必消费者再判一次。
        [[nodiscard]] auto view() const
        {
            return storage().template view<C, Rs...>(entt::exclude<Es...>);
        }

        [[nodiscard]] bool attached() const noexcept { return reg_ != nullptr; }
        [[nodiscard]] bool empty() const { return !reg_ || storage().empty(); }
        void clear() { if (reg_) storage().clear(); }

        /// Explicitly re-schedule one entity when an external asynchronous
        /// intent settles stale. Membership is still intersected at drain(), so
        /// callers do not need to duplicate Require/Exclude checks here.
        void mark(lux::meta::entity_id e)
        {
            if (reg_ && reg_->valid(e))
                storage().emplace(e);
        }

        /// 排空:整体换出,逐个交给 @p fn(签名 `bool(entity_id, C&)`,
        /// 返回「本次做了实质动作吗」)。
        ///
        /// ⚠️ **整体换出再处理**,与 `Schedule::applyCommandBarrier` 同一理由:
        ///    `fn` 里的结构性修改会经信号**回头往本集合里写**,而那正是正在被
        ///    迭代的存储。换出之后,处理期间新标记的自然落到下一轮。
        ///
        /// ★ VERIFY 构型下再做一件事 —— 本类存在的另一半理由:
        ///   全扫一遍,找「**有活干、却从来没被标记过**」的实体。那就是漏了信号源。
        ///
        ///   ⚠️ 判据是「没被标记」,**不是**「还有活干」。混淆两者会造出假阳性:
        ///   有些输入是**持续状态**而非一次性变化（例如由变换解析器维护的派生状态）。
        ///   那种实体每次处理都「有活干」,但它
        ///   被正确标记了,不是漏信号。(这条是把探针接进 CI 时暴露的。)
        ///
        ///   放在**本类**而不是各节点里:每个变更驱动的节点都需要同一张网,
        ///   让每家自己抄 20 行,迟早有一家抄漏 —— 而那正是这张网要防的错法。
        template <class Fn>
        void drain(lux::meta::EntityRegistry& reg, std::string_view owner, Fn&& fn)
        {
            scratch_.clear();
            for (const auto e : view())
                scratch_.push_back(e);
            clear();
            for (const auto e : scratch_)
                (void)fn(e, reg.template get<C>(e));

#if LUX_ECS_EXTRACTION_VERIFY
            auto marked = scratch_;
            std::sort(marked.begin(), marked.end());
            componentView<C>(reg, ComponentList<Rs...>{}, ComponentList<Es...>{})
                .each([&](lux::meta::entity_id e, C& c, auto&&...)
            {
                if (std::binary_search(marked.begin(), marked.end(), e))
                    return;   // 标记过 → 不管还有没有活干,信号没漏
                if (fn(e, c))
                    std::fprintf(stderr,
                        "[EXTRACTION-VERIFY] %.*s: entity %u had work but was NEVER "
                        "marked — a change source is missing its signal\n",
                        static_cast<int>(owner.size()), owner.data(),
                        static_cast<unsigned>(entt::to_integral(e)));
            });
#else
            (void)owner;
#endif
        }

        /// 把集合里**同时还带着 @p Extra... 的**成员标一遍。
        ///
        /// 给「**外部**输入变了,影响的是带某个标记的那一批」用 —— 那类变化按
        /// 定义**不产生**任何针对这些实体的组件信号,信号驱动够不着,必须由
        /// 知道那份输入的人推一把。两个真实用例:出图 view 换代(所有活实例
        /// 重发可见性)、相机移动(所有**视差**图的烘焙平移都变了)。
        ///
        /// ⚠️ 这是**兜底**,不是常规入口。每加一个,就等于承认有一份输入不在
        ///    组件系统里 —— 先想想那份输入能不能变成组件。
        template <class... Extra>
        void markAllWith()
        {
            if (!reg_) return;
            auto& s = storage();
            componentView<C>(*reg_, ComponentList<Rs..., Extra...>{},
                             ComponentList<Es...>{})
                .each([&s](lux::meta::entity_id e, auto&&...) { s.emplace(e); });
        }

        void markAll() { markAllWith<>(); }
    };

} // namespace lux::ecs

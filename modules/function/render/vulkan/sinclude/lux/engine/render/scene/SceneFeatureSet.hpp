#pragma once
/**
 * @file SceneFeatureSet.hpp
 * @brief 一个场景的特性容器、查询与每(特性,视图)状态账本。
 *
 * 从 RenderScene 拆出的第三个组件。一句话职责:
 * **持有已装特性的数据结构,并回答关于它们的查询**。
 *
 * ⚠ **本类零事务、不碰状态机、不调任何特性虚函数。**
 *
 * 这条边界不是洁癖,是被两个代码事实逼出来的:
 *
 *   1. SceneFeature 的 feature_id_ / descriptor_ / lifecycle_state_ / scene_
 *      是私有的,只对 `friend class RenderScene` 开放。三条事务路径
 *      (装特性 / 启停 / 卸特性)都要**写**这些字段。
 *   2. initAndAttachTo / onDetachFromScene / allocateViewState 三个虚函数的签名
 *      都要 RenderScene& 。
 *
 * 若把事务一并搬进来,就得给本类也加 friend(把 SceneFeature 的封装开口扩大一倍),
 * 或者让事务横跨两个对象、回滚路径也横跨两个对象 —— 那正是最容易写出双重释放的
 * 形状。所以:**数据结构在这里,事务编排留在 RenderScene**。
 *
 * 读取无需 friend —— typeId() / descriptor() / isEnabled() / name() 都是公开访问器;
 * 只有写才需要,而写全部留在 RenderScene。insert() 因此只返回句柄,由调用方回填
 * feature_id_。
 *
 * 账本(每特性拥有哪些视图的状态)只做**记账**:record/forget/take。真正调用
 * allocateViewState / deallocateViewState 的是 RenderScene。
 *
 * 线程:与 RenderScene 一致 —— 仅渲染线程。
 */

#include <lux/engine/render/SceneFeature.hpp>
// 本容器同时装两类特性,所以**它**要认识 RenderFeature —— 而且要完整类型:
// insert<T>() 里的 is_base_of_v 需要它。基类 SceneFeature 反过来则完全不认识
// RenderFeature(那里没有任何"我是不是渲染特性"的询问接口)。
#include <lux/engine/render/RenderFeature.hpp>
#include <lux/cxx/container/BasicSparseSet.hpp>   // SlotKeyAutoSparseSet

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lux::render
{
    class LUX_FUNCTION_PUBLIC SceneFeatureSet
    {
    public:
        /// 编辑器特性设置面板用的每特性参数描述(只读快照)。
        /// 无可调参数的特性(paramStructName() == "")其 struct_name/data/size
        /// 为空/null/0。所有视图指向渲染线程拥有的存储,仅在调用期间有效。
        struct FeatureParamDesc
        {
            FeatureHandle    id;   // 完整代数句柄(5-5),不只是 .index ——
                                   // 否则编辑器的陈旧选择可能别名到被复用的槽
            bool             enabled;
            std::string_view name;
            std::string_view struct_name;
            const void*      data;
            std::size_t      size;
        };

        SceneFeatureSet() = default;
        ~SceneFeatureSet() = default;

        SceneFeatureSet(const SceneFeatureSet&)            = delete;
        SceneFeatureSet& operator=(const SceneFeatureSet&) = delete;
        SceneFeatureSet(SceneFeatureSet&&)                 = delete;
        SceneFeatureSet& operator=(SceneFeatureSet&&)      = delete;

        /// 一个特性向渲染图声明其 pass 的操作。空 = 该特性不产 pass。
        ///
        /// 存**操作**而不是"它是不是 RenderFeature":下游只需要"让它向 builder
        /// 声明 pass"这一件事,不需要知道它的类型。于是 RenderFeature 这个类型
        /// 不必被穿过槽 → 缓存 → span → 图编译四层结构。
        using PassContributeFn = void (*)(SceneFeature&, RGBuilder&);

        // ── 容器 ────────────────────────────────────────────────────────
        /// 插入并返回代数句柄。**不**回填 feature->feature_id_ —— 那是私有字段,
        /// 由持有 friend 权限的 RenderScene 写。
        ///
        /// 模板参数 T 是**具体类型**,所以"这个特性产不产 pass"在这里是编译期
        /// 事实,一个 if constexpr 就够 —— 调用方不必传标志,SceneFeature 也不必
        /// 提供任何"我是不是 RenderFeature"的运行期询问接口。
        template <typename T>
        [[nodiscard]] FeatureHandle insert(std::unique_ptr<T> feature)
        {
            static_assert(std::is_base_of_v<SceneFeature, T>, "只能装入 SceneFeature 及其派生类");
            // 无捕获 lambda 隐式转函数指针 —— 零分配,槽里也不会出现同一个对象
            // 的第二个指针。**这是整个容器里 RenderFeature 唯一出现的地方。**
            PassContributeFn contribute = nullptr;
            if constexpr (std::is_base_of_v<RenderFeature, T>)
                contribute = [](SceneFeature& f, RGBuilder& b)
                             { static_cast<RenderFeature&>(f).addPasses(b); };
            return insertSlot(std::move(feature), contribute);
        }
        bool  erase(FeatureHandle handle);
        void  clear();

        [[nodiscard]] SceneFeature* get(FeatureHandle handle) const;
        [[nodiscard]] bool         contains(FeatureHandle handle) const noexcept;
        [[nodiscard]] std::size_t  size() const noexcept;

        // ── 查询 ────────────────────────────────────────────────────────
        /// 是否已有该稳定类型的活特性(kInvalidFeatureTypeId 永不匹配 —— 未声明
        /// 类型的特性不参与依赖/冲突判定)。
        [[nodiscard]] bool hasType(FeatureTypeId type) const noexcept;

        [[nodiscard]] SceneFeature* findByName(std::string_view name) const noexcept;

        /// 第一个仍**非可选地**依赖 @p type 的特性(跳过 @p exclude);无则 nullptr。
        /// 供卸载守卫用:另一个已装特性还要求它时,拒绝卸载。
        [[nodiscard]] const SceneFeature* firstRequiring(FeatureTypeId type, const SceneFeature* exclude) const noexcept;

        [[nodiscard]] std::vector<FeatureParamDesc> queryParamDescs() const;

        // ── 稠密视图(一次重建产出两份,顺序一致)────────────────────────
        //
        // 两份都是同一次遍历的产物:all ⊇ enabled,顺序均与装载顺序一致。

        /// 全部已装特性(含未启用)。
        [[nodiscard]] std::span<SceneFeature* const> all() const;

        /// 已启用的特性。
        [[nodiscard]] std::span<SceneFeature* const> enabled() const;

        void markCacheDirty() noexcept { cache_dirty_ = true; }

        // ── 向渲染图贡献 pass ───────────────────────────────────────────
        /// 依次让每个**已启用且产 pass** 的特性向 @p builder 声明它的 pass,
        /// 顺序与装载顺序一致(图编译依赖它)。
        ///
        /// 这里没有"已启用的 RenderFeature 列表"这种东西 —— 只拥有资源的特性
        /// 其槽里的 contribute 为空,自然被跳过。图编译因此完全不必认识
        /// RenderFeature:它只要求"把 pass 声明进这个 builder"。
        void contributePasses(RGBuilder& builder) const;

        // ── 每(特性,视图)状态账本 —— 纯记账 ────────────────────────────
        [[nodiscard]] bool ownsViewState(uint32_t feature_index, uint32_t view_index) const noexcept;
        void recordViewState(uint32_t feature_index, uint32_t view_index);
        void forgetViewState(uint32_t feature_index, uint32_t view_index) noexcept;
        /// 快照某特性登记的全部视图 id。**不改动账本** —— 调用方要在遍历中经
        /// releaseFeatureViewState 逐个释放,而后者的守卫要读账本;先删条目会让
        /// 守卫全部落空、deallocateViewState 一次都不执行(每视图状态泄漏)。
        [[nodiscard]] std::vector<uint32_t> viewsOwnedBy(uint32_t feature_index) const;
        /// 释放完毕后清掉该特性的账本条目。
        void forgetAllViewState(uint32_t feature_index) noexcept;
        void clearLedger() noexcept;

    private:
        /// 类型擦除的插入实现(在 .cpp)。@p contribute 由上面的模板在具体类型
        /// 已知处绑好;nullptr = 该特性不产 pass。
        [[nodiscard]] FeatureHandle insertSlot(std::unique_ptr<SceneFeature> feature,
                                               PassContributeFn contribute);

        void rebuildEnabledCacheIfNeeded() const;

        /// 一个槽 = 特性本体 + 它能为渲染图做的那件事。
        ///
        /// 存操作而非"它的另一个类型视图":后者是同一个对象的第二个指针,读起来
        /// 像冗余;而 contribute 表达的是一条**能力**,空指针就是"没有这个能力"。
        /// 存进槽里(而不是旁挂一个集合)使插入/擦除/清空天然同步。
        struct Slot
        {
            std::unique_ptr<SceneFeature> feature;
            PassContributeFn              contribute{nullptr};
        };

        using FeatureSet = lux::cxx::SlotKeyAutoSparseSet<FeatureHandle, Slot>;

        FeatureSet features_;

        /// 哪些 (特性, 视图) 对已成功分配过每视图状态 —— 真相源,好让
        /// deallocateViewState 恰好执行一次:视图移除与特性移除两侧对称。
        /// 按特性 index 键控(代数在 features_ 查找边界校验)。
        std::unordered_map<uint32_t, std::unordered_set<uint32_t>> view_states_;

        mutable std::vector<SceneFeature*>  all_dense_{};
        mutable std::vector<SceneFeature*>  enabled_dense_{};
        mutable bool                        cache_dirty_{true};
    };

} // namespace lux::render

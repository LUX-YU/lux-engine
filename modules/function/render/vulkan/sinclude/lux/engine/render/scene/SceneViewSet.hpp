#pragma once
/**
 * @file SceneViewSet.hpp
 * @brief 一个场景的视图集合与生命周期。
 *
 * 从 RenderScene 拆出的第二个组件。一句话职责:
 * **持有视图集合及其生命周期**(容器、状态机、活跃缓存、延迟销毁、每视图 GPU 槽)。
 *
 * **本类不认识特性。** 视图的增删要连带通知特性(allocate/deallocateViewState),
 * 但那个虚函数签名是 `allocateViewState(uint32_t, RenderScene&)` —— 调用它必须
 * 持有 RenderScene&。与其让本类反手持有场景引用(制造一个反向依赖),不如把特性侧
 * 通知留给 RenderScene 编排:本类只提供 isRemovable()/markDestroying() 这样的
 * 分步原语,让场景能在正确的时机插入通知,且顺序与原实现逐字一致。
 *
 * 三条延迟销毁 FIFO 里,视图这条(pending_destroys_)在本类;另两条(编译图 /
 * 每视图图资源)在 SceneGraphCache。
 *
 * 线程:与 RenderScene 一致 —— 仅渲染线程。
 */

#include <lux/engine/function/render/client/core/RenderTypes.hpp>   // lux::math::Extent2u
#include <lux/engine/render/scene/View.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/cxx/container/BasicSparseSet.hpp>     // SlotKeyAutoSparseSet

#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <vector>

namespace lux::render
{
    class SceneGraphCache;
    class ResourceRegistry;

    struct ViewCreateInfo
    {
        lux::math::Extent2u initial_extent{};
        const char *debug_name{"View"};
    };

    class LUX_FUNCTION_PUBLIC SceneViewSet
    {
    public:
        /// @param registry 场景资源注册表 —— 每视图 GPU 槽从其中的 SceneResources 取。
        explicit SceneViewSet(ResourceRegistry& registry) noexcept;
        ~SceneViewSet();

        /// 接上图缓存 —— 视图持有的录制上下文与物理资源归它所有,释放必须还给它。
        ///
        /// 为什么是 setter 而不是构造参数:`RenderScene::graph_cache_` 是
        /// `unique_ptr`,在**构造体内**建立(它的构造需要 debug_name_,而后者按声明序
        /// 在它之后初始化)。所以本对象构造时它还不存在,只能建好之后接上。
        /// RenderScene 的构造体内紧跟着 make_unique 调用本方法。
        void setGraphCache(SceneGraphCache& cache) noexcept { graph_cache_ = &cache; }

        SceneViewSet(const SceneViewSet&)            = delete;
        SceneViewSet& operator=(const SceneViewSet&) = delete;
        SceneViewSet(SceneViewSet&&)                 = delete;
        SceneViewSet& operator=(SceneViewSet&&)      = delete;

        // ── 查询 ────────────────────────────────────────────────────────
        [[nodiscard]] View*       get(ViewHandle handle) noexcept;
        [[nodiscard]] const View* get(ViewHandle handle) const noexcept;
        [[nodiscard]] std::size_t activeCount() const noexcept;

        /// 活跃视图的稠密视图(内部缓存,脏时重建)。
        [[nodiscard]] std::span<View* const> active() const;
        /// 全部视图(含正在销毁的)。图缓存在重编提交点要收走它们的图资源。
        ///
        /// 与 active() 同形返回 span:这两个函数同名同义,此前一个返回 span
        /// 一个每次调用现造一个 vector,是个会被误用的不一致。空槽在建缓存时
        /// 就滤掉了(唯一的消费方本来就在逐个判空)。
        [[nodiscard]] std::span<View* const> all() const;

        // ── 生命周期 ────────────────────────────────────────────────────
        /// 建视图 + 分配每视图 GPU 槽。**不**通知特性 —— 调用方在拿到句柄后自行通知。
        [[nodiscard]] ViewHandle add(const ViewCreateInfo& info);

        /// 该句柄是否可被移除(存在且尚未进入销毁中)。
        /// 幂等守卫:重复 removeView 必须不重复释放特性状态、不重复通知注册表、
        /// 不重复入队(第二条记录会在 GC 时把同一句柄处理两遍)。
        [[nodiscard]] bool isRemovable(ViewHandle handle) const noexcept;

        /// 标记为销毁中并入延迟销毁队列。调用方须已完成特性/注册表侧的通知。
        /// 此处**不**释放 GPU 槽 —— 其它 frames-in-flight 槽位的在飞命令可能仍在
        /// 引用它,必须等到 collectDestroyed 的水位越过。
        void markDestroying(ViewHandle handle);

        /// 每帧收尾:清掉活跃视图的 frame_systems_done 标记。
        void endViewFrame();

        /// 回收已被 GPU 完成水位越过的待销毁视图(释放其图资源 + GPU 槽 + 移出容器)。
        /// @param frame_id         首次见到的条目用它打戳(其最后一次 GPU 使用的上界)。
        /// @param completed_serial **栅栏证实**的完成水位;戳记被越过才释放。
        void collectDestroyed(uint64_t frame_id, uint64_t completed_serial);

        /// 场景 teardown:释放全部视图的图资源与 GPU 槽并清空容器。幂等。
        ///
        /// 此前签名是 `shutdown(SceneGraphCache*)` —— 清理需要一个**兄弟对象**做参数,
        /// 于是 `~SceneViewSet() = default` 成了唯一可能,而跳过 shutdown() 不崩、
        /// 只是静默泄漏。图缓存改成构造后接上的成员之后,析构就能自己收尾了。
        void shutdown();

        void markCacheDirty() noexcept { cache_dirty_ = true; }

    private:
        void rebuildActiveCacheIfNeeded() const;
        void initViewUBO(View& view);
        void destroyViewUBO(View& view);
        /// 释放一个视图持有的图资源(录制上下文 + 物理资源),幂等。
        void releaseViewGraphResources(View& view) noexcept;

        struct PendingDestroy
        {
            ViewHandle view_id;
            uint64_t   destroy_frame{0};
        };

        using ViewSet = lux::cxx::SlotKeyAutoSparseSet<ViewHandle, std::unique_ptr<View>>;

        ResourceRegistry&    registry_;
        /// 非拥有。由 setGraphCache 在 RenderScene 构造体内接上;RenderScene 里
        /// graph_cache_ 声明在 view_set_ **之前**,所以逆序析构时它还活着。
        SceneGraphCache*     graph_cache_{nullptr};
        ViewSet                   views_;
        std::deque<PendingDestroy> pending_destroys_;

        mutable std::vector<View*> active_dense_{};
        /// all() 的稠密缓存,与 active_dense_ 在同一次重建里一起填 —— 共用
        /// cache_dirty_ 是安全的:视图增删会同时影响两者,而状态变化只多刷一次
        /// all_dense_,是无害的多余工作。
        mutable std::vector<View*> all_dense_{};
        mutable bool               cache_dirty_{true};
    };

} // namespace lux::render

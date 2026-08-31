#include <lux/engine/render/scene/SceneViewSet.hpp>

#include <lux/engine/render/graph/RGVulkanRecorder.hpp>
#include <lux/engine/render/graph/RGVulkanResourceAllocator.hpp>
#include <lux/engine/render/gpu/lifecycle/ResourceRegistry.hpp>
#include <lux/engine/render/resources/SceneResources.hpp>
#include <lux/engine/render/scene/SceneGraphCache.hpp>

#include <utility>

namespace lux::render
{
    SceneViewSet::SceneViewSet(ResourceRegistry& registry) noexcept : registry_(registry)
    {
    }

    SceneViewSet::~SceneViewSet()
    {
        // 自己收自己的尾。此前是 `= default` —— 不是疏忽,是 shutdown() 当时需要一个
        // **兄弟对象**(SceneGraphCache*)做参数,析构里根本拿不到,于是清理只能靠外部
        // 记得调,而跳过它不崩、是静默泄漏(录制上下文 + 物理资源都不还)。
        // 图缓存改成构造后接上的成员之后,这条限制就没了。shutdown() 幂等,
        // RenderScene::shutdownFull() 已经调过一次。
        shutdown();
    }

    // ─────────────────────────────────────────────────────────────────────
    //  查询
    // ─────────────────────────────────────────────────────────────────────

    View* SceneViewSet::get(ViewHandle handle) noexcept
    {
        if (!views_.contains(handle))
            return nullptr;
        return views_.at(handle).get();
    }

    const View* SceneViewSet::get(ViewHandle handle) const noexcept
    {
        if (!views_.contains(handle))
            return nullptr;
        return views_.at(handle).get();
    }

    void SceneViewSet::rebuildActiveCacheIfNeeded() const
    {
        if (!cache_dirty_)
            return;
        active_dense_.clear();
        active_dense_.reserve(views_.size());
        all_dense_.clear();
        all_dense_.reserve(views_.size());
        for (const auto& view_ptr : views_.values())
        {
            if (!view_ptr)
                continue;
            all_dense_.push_back(view_ptr.get());
            if (view_ptr->state == ViewState::Active)
                active_dense_.push_back(view_ptr.get());
        }
        cache_dirty_ = false;
    }

    std::span<View* const> SceneViewSet::active() const
    {
        rebuildActiveCacheIfNeeded();
        return {active_dense_.data(), active_dense_.size()};
    }

    std::size_t SceneViewSet::activeCount() const noexcept
    {
        rebuildActiveCacheIfNeeded();
        return active_dense_.size();
    }

    std::span<View* const> SceneViewSet::all() const
    {
        rebuildActiveCacheIfNeeded();
        return {all_dense_.data(), all_dense_.size()};
    }

    // ─────────────────────────────────────────────────────────────────────
    //  每视图 GPU 槽
    // ─────────────────────────────────────────────────────────────────────

    // SceneResources 由 RenderScene 构造函数无条件 emplace 进同一个 resources_,
    // 而视图生命周期全在构造之后 —— 故用 must<>:原先的判空是死守卫,且它把
    // "槽位分配失败"静默成了"view_slot 保持无效",后者要到渲染期才炸。
    void SceneViewSet::initViewUBO(View& view)
    {
        view.view_slot = registry_.must<SceneResources>().allocateView();
    }

    void SceneViewSet::destroyViewUBO(View& view)
    {
        registry_.must<SceneResources>().freeView(view.view_slot);
    }

    void SceneViewSet::releaseViewGraphResources(View& view) noexcept
    {
        if (!view.resource_state)
            return;
        if (graph_cache_)
        {
            graph_cache_->recorder().deallocateRecordContext(view.resource_state->record_ctx);
            graph_cache_->allocator().deallocate(view.resource_state->physical_resources);
        }
        view.resource_state.reset();
    }

    // ─────────────────────────────────────────────────────────────────────
    //  生命周期
    // ─────────────────────────────────────────────────────────────────────

    ViewHandle SceneViewSet::add(const ViewCreateInfo& info)
    {
        auto view = std::make_unique<View>();
        view->current_extent = info.initial_extent;
        view->debug_name = info.debug_name ? info.debug_name : "View";

        // SlotKeyAutoSparseSet::insert assigns a generational ViewHandle.
        ViewHandle handle = views_.insert(std::move(view));
        auto* new_view = views_.at(handle).get();
        new_view->handle = handle;
        new_view->state = ViewState::Active;

        initViewUBO(*new_view);
        markCacheDirty();
        return handle;
    }

    bool SceneViewSet::isRemovable(ViewHandle handle) const noexcept
    {
        if (!views_.contains(handle))
            return false;
        // 已在销毁中的视图会在槽表里逗留到 GC 释放它(fif 帧后)。这个窗口内的重复
        // removeView 必须被挡掉,否则会重复释放特性状态、重复通知注册表、并再入队
        // 一条记录 —— 第二条会在 GC 时把同一句柄处理两遍。(5-4)
        return views_.at(handle)->state != ViewState::Destroying;
    }

    void SceneViewSet::markDestroying(ViewHandle handle)
    {
        if (!views_.contains(handle))
            return;
        auto& view = *views_.at(handle);
        view.state = ViewState::Destroying;
        markCacheDirty();
        // 不在此释放 GPU 槽:其它 frames-in-flight 槽位的在飞命令可能仍引用它。
        pending_destroys_.push_back({handle, 0});
    }

    void SceneViewSet::endViewFrame()
    {
        rebuildActiveCacheIfNeeded();
        for (auto* view : active_dense_)
            view->frame_systems_done = false;
    }

    void SceneViewSet::collectDestroyed(uint64_t frame_id, uint64_t completed_serial)

    {
        for (auto& pd : pending_destroys_)
        {
            if (pd.destroy_frame == 0)
                pd.destroy_frame = frame_id; // 首见打戳 = 其最后一次 GPU 使用的上界
        }

        while (!pending_destroys_.empty())
        {
            auto& oldest = pending_destroys_.front();
            // 只有**栅栏证实**的完成水位越过戳记才释放。原来的
            // "frame_id - destroy_frame >= fif" 算术会高估完成度:序号在不提交的
            // tick 上照样前进,可能在 GPU 仍引用时就释放。
            if (oldest.destroy_frame > completed_serial)
                break;

            if (views_.contains(oldest.view_id))
            {
                auto& view_ptr = views_.at(oldest.view_id);
                if (view_ptr)
                {
                    releaseViewGraphResources(*view_ptr);
                    destroyViewUBO(*view_ptr);
                }
                views_.erase(oldest.view_id);
            }
            pending_destroys_.pop_front();
        }
    }

    void SceneViewSet::shutdown()
    {
        for (auto& v : views_.values())
        {
            if (!v)
                continue;
            releaseViewGraphResources(*v);
            destroyViewUBO(*v);
        }
        views_.clear();
        pending_destroys_.clear();
        active_dense_.clear();
        all_dense_.clear();
        cache_dirty_ = false;
    }

} // namespace lux::render

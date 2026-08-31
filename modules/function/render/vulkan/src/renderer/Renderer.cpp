#include <lux/engine/render/renderer/Renderer.hpp>
#include <lux/engine/function/render/client/core/RenderFatal.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/scene/View.hpp>
#include <lux/engine/render/graph/RGCompiledGraph.hpp>
#include <lux/engine/render/graph/RGRecorder.hpp> // RGFrameContext, RGRecordContext
#include <lux/engine/render/graph/RGVulkanRecorder.hpp>
#include <lux/engine/render/graph/RGGraphResize.hpp>
#include <lux/engine/render/graph/PhysicalResourceAllocator.hpp>
#include <lux/engine/render/graph/RGVulkanResourceAllocator.hpp>
#include <lux/engine/render/resources/SceneResources.hpp>
#include <lux/engine/render/gpu/descriptor/SceneDomainDescriptorSets.hpp>
// scene_ds channel feeds the domain GLOBAL instance
#include <lux/engine/render/RenderFeature.hpp>

#include <cassert>

namespace lux::render
{

    // ─────────────────────────────────────────────────────────────────────
    //  Ctor / Dtor
    // ─────────────────────────────────────────────────────────────────────

    Renderer::Renderer(std::shared_ptr<RenderContext> ctx) : ctx_(std::move(ctx))
    {
        assert(ctx_ && "Renderer: RenderContext must not be null");
    }

    Renderer::~Renderer()
    {
        const VkResult idle = ctx_->deviceContext().logicalDevice().waitIdle();
        if (idle != VK_SUCCESS && idle != VK_ERROR_DEVICE_LOST)
            renderFatal("Renderer failed to wait for device idle during teardown");
        // Member destruction order (reverse of declaration) guarantees
        // scenes_ is destroyed before ctx_, so ~RenderScene() → shutdownFull()
        // runs while the shared RenderContext is still alive.
    }

    // ─────────────────────────────────────────────────────────────────────
    //  Scene lifecycle
    // ─────────────────────────────────────────────────────────────────────

    AddSceneResult Renderer::addScene(RenderScene::Config config)
    {
        auto initial_views = std::move(config.initial_views);
        auto scene = std::make_unique<RenderScene>(ctx_, config);
        auto* scene_ptr = scene.get();
        scene_ptr->bindFeatureTypeRegistry(feature_type_registry_);
        // SlotKeyAutoSparseSet::insert assigns a generational RenderSceneId.
        auto scene_id = scenes_.insert(std::move(scene));
        // 回填,好让场景自发上报的诊断说得清是哪个场景(见 RenderScene::sceneId)。
        scene_ptr->setSceneId(scene_id);

        AddSceneResult result{scene_id};
        result.view_handles.reserve(initial_views.size());
        for (auto& vi : initial_views)
            result.view_handles.push_back(scene_ptr->addView(vi));
        return result;
    }

    void Renderer::removeScene(RenderSceneId scene_id)
    {
        if (!scenes_.contains(scene_id))
            return;

        // Defer teardown instead of stalling the whole device (the old waitIdle()
        // blocked every other scene to destroy one). The scene leaves the live set
        // immediately — its slot's generation bumps so the id can never alias a
        // future scene, and it renders no more — and its GPU resources are reclaimed
        // in endFrame() once all in-flight frames that could reference them have
        // completed (see retired_scenes_ / collectRetiredScenes).
        retired_scenes_.push_back({std::move(scenes_.at(scene_id)), current_stamp_.serial});
        scenes_.erase(scene_id);
    }

    void Renderer::collectRetiredScenes(uint64_t frame_serial)
    {
        // Reclaim gate = the FENCE-PROVEN completion watermark, not serial
        // arithmetic. Serials advance on ticks that never submit (command-only
        // ticks, the no-views early return right after a DestroyScene), so
        // "frame_serial - retire_serial >= fif" over-claimed completion and the
        // scene-cycle stress gate caught shutdownFull destroying resources a
        // still-executing command buffer referenced. retire_serial is an upper
        // bound of the scene's last possible GPU use; same-queue FIFO completion
        // makes the watermark monotone over all earlier submissions.
        const uint64_t fif = ctx_->framesInFlight();
        const uint64_t completed = completedSerialOr(frame_serial > fif ? frame_serial - fif : 0);
        std::erase_if(retired_scenes_, [&](RetiredScene& r) {
            if (r.retire_serial > completed)
                return false; // GPU may still reference this scene's resources
            r.scene->shutdownFull();
            return true; // drop — the unique_ptr frees the scene
        }
        );
    }

    // ─────────────────────────────────────────────────────────────────────
    //  Frame lifecycle (render thread)
    // ─────────────────────────────────────────────────────────────────────

    void Renderer::beginFrame(const FrameStamp& stamp)
    {
        current_stamp_ = stamp;
        const auto scene_clock_now = std::chrono::steady_clock::now();
        const float scene_time = std::chrono::duration<float>(scene_clock_now - scene_clock_start_).count();
        const float scene_delta = std::chrono::duration<float>(scene_clock_now - scene_clock_previous_).count();
        scene_clock_previous_ = scene_clock_now;

        // Set the current serial on the destroy queue and collect resources
        // whose retire serial is now guaranteed complete. "Guaranteed" means
        // FENCE-PROVEN (setGpuCompletedSerial, fed from FrameDriver each tick):
        // (current - FIF) arithmetic is only the driverless fallback — serials
        // advance on non-submitting ticks, so arithmetic alone over-claims.
        auto& dq = ctx_->deferredDestroyQueue();
        dq.beginFrame(stamp.serial);
        const uint64_t completed =
            completedSerialOr((stamp.serial > stamp.frames_in_flight) ? stamp.serial - stamp.frames_in_flight : 0);
        dq.collect(completed);
        ctx_->retireScheduler().collect(completed);

        // Retire global transfer scheduler overflow staging from the completed frame.
        auto& global_scheduler = ctx_->globalTransferScheduler();
        if (global_scheduler.isInitialized())
            global_scheduler.retireStaging(stamp.slotIndex());

        auto& global_registry = ctx_->globalRegistry();
        // 每帧维护回调由**安装点**登记(见各资源的安装处),这里只按阶段驱动。
        // 顺序 = 登记顺序 = 原先的注册顺序。
        for (uint32_t phase = 0; phase < static_cast<uint32_t>(EUploadPhase::Count); ++phase)
        {
            for (const auto& hook : global_registry.beginFrameHooks(static_cast<EUploadPhase>(phase)))
                hook(stamp);
        }

        for (auto& scene_ptr : scenes_.values())
        {
            if (scene_ptr)
            {
                // The render owner advances one monotonic clock for every scene.
                // Transition shaders and delayed render-object retirement now
                // observe the same time without a per-frame client command.
                scene_ptr->setSceneTime(scene_time, scene_delta, stamp.serial);
                scene_ptr->beginFrame(stamp);
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────
    //  runUploadPhase — global + per-scene transfers/uploads, once per tick
    // ─────────────────────────────────────────────────────────────────────

    void Renderer::runUploadPhase(VkCommandBuffer cmd)
    {
        record(cmd, current_stamp_);
    }

    void Renderer::record(VkCommandBuffer cmd, const FrameStamp& stamp)
    {
        // ── Lazy-initialize global transfer scheduler ─────────────────
        auto& global_scheduler = ctx_->globalTransferScheduler();
        if (!global_scheduler.isInitialized())
        {
            TransferScheduler::Config ts_cfg{
                .allocator = ctx_->vmaAllocator(),
                .ring_capacity = 4 * 1024 * 1024,
                .frames_in_flight = ctx_->framesInFlight(),
            };
            (void)global_scheduler.init(ts_cfg);
        }

        // ── Execute global transfer scheduler ─────────────────────────
        if (global_scheduler.isInitialized())
        {
            global_scheduler.resetFrame(stamp.slotIndex());
            global_scheduler.executeAll(cmd);
        }

        // Per-scene uploads (instance data, point cloud, trajectory, etc.).
        for (auto& scene_ptr : scenes_.values())
        {
            if (scene_ptr)
                scene_ptr->record(cmd, stamp);
        }
    }

    // ─────────────────────────────────────────────────────────────────────
    //  prepareSceneForRender — shared preamble for renderSceneViews / renderSingleView
    // ─────────────────────────────────────────────────────────────────────

    bool Renderer::prepareSceneForRender(RenderScene& scene, const RenderTargetLayout& layout)
    {
        auto& gs = scene.graphState();

        // The compiled graph template is only valid for the FORMAT KEY it was
        // built from(layout 拆分):槽位集合/format/aspect/initial_layout/
        // preserve_content 烙进 VkPipeline,变了必须重编;final_layout/usage/
        // is_presentable 只影响图末 final barrier 与图像创建参数,录制期按
        // 当前 binding 参数化——同一 scene 渲往 SAMPLED 目标 / 可回读目标 /
        // swapchain 共用同一模板,原"双 target 逐帧重编 thrash"从根上消失。
        // Compare UNCONDITIONALLY (4-1): the old `hasSlot(SceneColor) && …` short
        // circuit meant a NEW layout with no SceneColor never counted as changed, so a
        // stale graph kept being used for the new binding. Any format-key difference
        // now triggers a recompile against the REQUESTED layout.
        const bool layout_changed = !formatKeyEquals(layout, gs.last_layout);

        if ((!gs.valid || layout_changed) && !scene.isGraphRecompileSuppressed())
        {
            // 格式异构显式拒绝:同一帧内因 format_key 差异二次重编 = 同一场景的
            // 多个层指向了 format_key 不同的 target(格式与槽位集合必须一致;
            // final_layout / usage 的差异无需分模板)。这不是缓存问题而是使用错误,
            // 照旧重编以保持能渲,但必须让上层看见 —— 每帧重编会把帧率拖垮。
            //
            // 上报通道自带同键合并,所以这里不再需要一个 warned-once 标志:
            // 「说过一次就不说了」会让持续发生的问题在诊断面板上看着像已经好了。
            if (layout_changed && gs.valid && gs.last_format_key_compile_serial == current_stamp_.serial)
            {
                ctx_->reportError(
                    renderError<err::frame::GraphRecompileThrash>(),
                    scene.sceneId().index,
                    current_stamp_.serial
                );
            }
            gs.last_format_key_compile_serial = current_stamp_.serial;
            // Always (re)compile against the REQUESTED layout — never fall back to the
            // stored last_layout: rendering a new binding with a graph built for a
            // DIFFERENT layout is exactly the mismatch this guards against.
            // compileGraphTemplate treats a no-SceneColor layout as a valid "no graph"
            // outcome (the scene then renders blank), so it is not special-cased here.
            scene.compileGraphTemplate(layout);

            // 场景的目标布局里没有 SceneColor 槽 → 图编不出来,这个场景的所有视图
            // 都是空的。上层多半以为自己已经把目标接好了。
            if (!gs.valid && !layout.hasSlot(TargetSlot::SCENE_COLOR))
            {
                ctx_->reportError(
                    renderError<err::frame::NoSceneColorTarget>(),
                    scene.sceneId().index,
                    current_stamp_.serial
                );
            }
        }

        return gs.valid;
    }

    void Renderer::renderSingleView(
        RenderScene& scene,
        View& view,
        const RenderTargetBinding& binding,
        FrameRuntime& rt,
        uint32_t cross_view_index
    )
    {
        // The binding is the single source of truth for its target: it carries both
        // the physical images AND the layout the graph must compile against (P4d
        // merged the formerly-separate `layout` parameter into it, so the two can no
        // longer disagree). A binding that reaches here without a layout is a caller
        // bug — the swapchain paths set binding.layout per overlay phase before calling
        // (they previously passed it as a separate argument).
        if (binding.layout == nullptr)
        {
            ctx_->reportError(
                renderError<err::frame::BindingHasNoLayout>(),
                scene.sceneId().index,
                current_stamp_.serial
            );
            return;
        }
        const RenderTargetLayout& layout = *binding.layout;

        // View 瘦身:current_extent 是渲染期从 binding 派生的缓存(相机
        // 帧数据/HZB 读它拿到的就是本次实际渲染尺寸),视图不再自持尺寸账本。
        view.current_extent = {binding.extent.width, binding.extent.height};

        if (!prepareSceneForRender(scene, layout))
            return;

        if (!prepareViewForRender(scene, view, binding, rt))
            return;

        auto& gs = scene.graphState();
        DrawRequest req;
        req.scene = &scene;
        req.view = &view;
        req.target = &binding;
        renderView(scene, gs, view, req, rt, cross_view_index);
    }

    void Renderer::endFrame(uint64_t frame_serial)
    {
        // 与 retired_scenes_ / DeferredDestroyQueue 同一水位口径:栅栏证实的完成序号,
        // 无驱动(headless)时回落到旧算术 —— 那种 tick 从不提交 GPU 工作,算术在那里
        // 是空泛安全的。
        const uint64_t fif = ctx_->framesInFlight();
        const uint64_t completed = completedSerialOr(frame_serial > fif ? frame_serial - fif : 0);

        for (auto& scene_ptr : scenes_.values())
        {
            if (scene_ptr)
                scene_ptr->endFrame(frame_serial, completed);
        }

        // (此处曾每帧完整遍历一遍全局服务链去调 onEndFrame —— 而**零个**资源
        //  重写过它。接口与遍历一并删除。)

        collectRetiredScenes(frame_serial);
    }

    // ─────────────────────────────────────────────────────────────────────
    //  prepareViewForRender — ensure per-view GPU resources are ready
    // ─────────────────────────────────────────────────────────────────────

    bool
    Renderer::prepareViewForRender(RenderScene& scene, View& view, const RenderTargetBinding& binding, FrameRuntime& rt)
    {
        auto& gs = scene.graphState();
        if (!gs.valid || !gs.graph)
            return false;

        // Determine target extent from the binding
        if (binding.extent.width == 0 || binding.extent.height == 0)
            return false;
        VkExtent2D target_ext{binding.extent.width, binding.extent.height};

        // Lazily allocate the per-view resource state
        if (!view.resource_state)
            view.resource_state = std::make_unique<RGResourceState>();

        // Resize/allocate if extent changed (or first use — current_extent starts at {0,0})
        const RGGraphDescription* current_graph_desc = &gs.graph->original_graph;
        const bool resized_or_ready = ResizeViewResources(
            *gs.graph,
            target_ext,
            *view.resource_state,
            scene.graphRecorder(),
            scene.graphAllocator(),
            ctx_->framesInFlight(),
            nullptr,
            [&scene, current_graph_desc](RGResourceState&& retired_state) {
                scene.retireViewResourceState(std::move(retired_state), current_graph_desc);
            }
        );
        if (!resized_or_ready)
            return false;

        // Pre-record dynamic imported handle refresh (moved out of recorder::record()).
        scene.graphRecorder().refreshDynamicImportedResources(*view.resource_state, *gs.graph);
        return true;
    }

    // ─────────────────────────────────────────────────────────────────────
    //  renderView() — record graph commands for a single view
    // ─────────────────────────────────────────────────────────────────────

    void Renderer::renderView(
        RenderScene& scene,
        SceneGraphState& gs,
        View& view,
        const DrawRequest& req,
        FrameRuntime& rt,
        uint32_t cross_view_index
    )
    {
        assert(gs.graph && view.resource_state && "renderView: graph and view resources must be ready");

        // Guard: view must have a valid slot allocated (skips views whose slot
        // hasn't been assigned yet or are pending GC).
        if (!view.view_slot.isValid())
            return;

        auto& scene_res = scene.resources().must<SceneResources>();

        const uint32_t slot = rt.stamp.slotIndex();

        RGFrameContext frame_ctx{};
        frame_ctx.frame_index = slot;
        frame_ctx.image_index = rt.image_index;
        frame_ctx.frame_id = rt.stamp.serial;
        // The scene_ds channel now uniformly feeds the domain GLOBAL instance
        // (a dual write keeps its data consistent with the old Scene set). A
        // merged pipeline's GLOBAL domain slot already binds the domain
        // instance; this is also where the old bare-slot bindSceneDS call
        // sites end up reading the domain instance — with that, the last
        // class of reader of the old Scene set is gone, and
        // SceneResources's per-set instance can be retired once the dual
        // write is removed. When the domain instance is missing (very early
        // startup, or a scene with no domain), this falls back to the old
        // set, matching pre-migration behavior.
        {
            auto* domains = scene.domainDescriptorSets();
            const VkDescriptorSet domain_scene =
                domains ? domains->set(lux::rdesc::EBindFrequency::GLOBAL, slot) : VK_NULL_HANDLE;
            frame_ctx.scene_ds = (domain_scene != VK_NULL_HANDLE) ? domain_scene : scene_res.getDescriptorSet(slot);
        }
        frame_ctx.scene_index = scene.sceneGlobalSlot().index;
        frame_ctx.view_index = view.view_slot.index;
        frame_ctx.view = &view;
        // 把视锥平面以中性字节交给渲染图 —— 组装方在这里决定"搬哪些字节",
        // 图层与 kernel 因此都不必认识 View 的结构(见 RGFrameContext 处说明)。
        frame_ctx.view_frustum = std::span<const std::byte>{view.frustum_staging};
        frame_ctx.cross_view_index = cross_view_index;

        // Let each enabled feature inject its own extension data.
        for (auto* feat : scene.enabledFeatures())
            feat->populateFrameContext(frame_ctx);

        // Inject per-slot images/views from the target binding
        if (req.target)
        {
            // 当前 target 的 layout 随录制下发——final_layout/usage 不再
            // 参与模板编译键,图末 final barrier 与后续帧首触屏障按它参数化。
            frame_ctx.target_layout = req.target->layout;
            for (size_t si = 0; si < kTargetSlotCount; ++si)
            {
                const auto& slot_imgs = req.target->slot(static_cast<TargetSlot>(si));
                if (slot_imgs.images.empty())
                    continue;

                // Use frame slot when the binding has per-FIF entries,
                // otherwise fall back to index 0 (single-frame bindings like swapchain).
                const size_t idx = (slot < slot_imgs.images.size()) ? slot : 0;
                frame_ctx.imported_slots[si].image = slot_imgs.images[idx];
                if (idx < slot_imgs.views.size())
                    frame_ctx.imported_slots[si].view = slot_imgs.views[idx];
            }
        }

        scene.graphRecorder().record(
            *view.resource_state,
            *gs.graph,
            frame_ctx,
            rt.primary_cmd,
            &scene.renderContext().globalRegistry()
        );

        // Expose per-view multi-queue submissions to the frame driver.
        if (view.resource_state->record_ctx.multi_queue_submit.is_multi_queue)
        {
            rt.multi_queue_submits.push_back(&view.resource_state->record_ctx.multi_queue_submit);
        }

        // Fold this view's externally-injected sync (addExternalGraphicsWait/Signal,
        // e.g. a CUDA producer) into the frame-level GRAPHICS submit. Empty unless a
        // feature injected in populateFrameContext — a pure no-op for normal rendering.
        if (!frame_ctx.external_graphics_waits.empty())
            rt.external_graphics_waits.insert(
                rt.external_graphics_waits.end(),
                frame_ctx.external_graphics_waits.begin(),
                frame_ctx.external_graphics_waits.end()
            );
        if (!frame_ctx.external_graphics_signals.empty())
            rt.external_graphics_signals.insert(
                rt.external_graphics_signals.end(),
                frame_ctx.external_graphics_signals.begin(),
                frame_ctx.external_graphics_signals.end()
            );
    }

    // ─────────────────────────────────────────────────────────────────────
    //  Accessors
    // ─────────────────────────────────────────────────────────────────────

    RenderContext& Renderer::renderContext() noexcept
    {
        return *ctx_;
    }
    uint32_t Renderer::framesInFlight() const noexcept
    {
        return ctx_->framesInFlight();
    }

    RenderScene* Renderer::getScene(RenderSceneId id) noexcept
    {
        // tryGet returns nullptr for a stale id (generation mismatch) as well as
        // an unknown one — no separate validity check needed.
        auto* slot = scenes_.tryGet(id);
        return slot ? slot->get() : nullptr;
    }

} // namespace lux::render

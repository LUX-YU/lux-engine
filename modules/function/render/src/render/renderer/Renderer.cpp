#include <lux/engine/render/renderer/Renderer.hpp>
#include <lux/engine/render/RendererContext.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/scene/View.hpp>
#include <lux/engine/render/graph/RGCompiledGraph.hpp>
#include <lux/engine/render/graph/RGRecorder.hpp> // RGFrameContext, RGRecordContext
#include <lux/engine/render/graph/RGVulkanRecorder.hpp>
#include <lux/engine/render/graph/RGGraphResize.hpp>
#include <lux/engine/render/graph/PhysicalResourceAllocator.hpp>
#include <lux/engine/render/graph/RGVulkanResourceAllocator.hpp>
#include <lux/engine/render/resources/SceneResources.hpp>
#include <lux/engine/render/resources/mesh/MeshResources.hpp>
#include <lux/engine/render/resources/material/MaterialResources.hpp>
#include <lux/engine/render/resources/lighting/LightResources.hpp>
#include <lux/engine/render/resources/TextureResources.hpp>
#include <lux/engine/render/transfer/TransferContributor.hpp>
#include <lux/engine/render/RenderFeature.hpp>

#include <cassert>
#include <iostream>

namespace lux::render
{

    // ─────────────────────────────────────────────────────────────────────
    //  Ctor / Dtor
    // ─────────────────────────────────────────────────────────────────────

    Renderer::Renderer(std::shared_ptr<RenderContext> ctx)
        : ctx_(std::move(ctx))
    {
        assert(ctx_ && "Renderer: RenderContext must not be null");
    }

    Renderer::~Renderer()
    {
        if (ctx_)
            ctx_->deviceContext().logicalDevice().waitIdle();
        global_transfer_scheduler_.shutdown();
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
        auto id = scenes_.insert(std::move(scene));
        auto scene_id = static_cast<RenderSceneId>(id);

        AddSceneResult result{scene_id};
        result.view_handles.reserve(initial_views.size());
        for (auto& vi : initial_views)
            result.view_handles.push_back(scene_ptr->addView(vi));
        return result;
    }

    void Renderer::removeScene(RenderSceneId scene_id)
    {
        auto idx = static_cast<uint32_t>(scene_id);
        if (scenes_.contains(idx))
        {
            // `waitIdle` is tolerant of `VK_ERROR_DEVICE_LOST` (the macro
            // logs but does not assert) so teardown can run to completion
            // even when an earlier frame faulted the GPU.
            ctx_->deviceContext().logicalDevice().waitIdle();
            scenes_.at(idx)->shutdownFull();
            scenes_.erase(idx);
        }
    }

    // ─────────────────────────────────────────────────────────────────────
    //  Frame lifecycle (render thread)
    // ─────────────────────────────────────────────────────────────────────

    void Renderer::beginFrame(const FrameStamp& stamp)
    {
        current_stamp_ = stamp;

        // Set the current serial on the destroy queue and collect resources
        // whose retire serial is now guaranteed complete.  The completed
        // serial is (current - FIF): after the fence wait for this slot the
        // GPU has finished all frames up to completedSerial.
        auto& dq = ctx_->deferredDestroyQueue();
        dq.beginFrame(stamp.serial);
        const uint64_t completed = (stamp.serial > stamp.frames_in_flight)
                                 ?  stamp.serial - stamp.frames_in_flight
                                 :  0;
        dq.collect(completed);
        ctx_->retireScheduler().collect(completed);

        // Retire global transfer scheduler overflow staging from the completed frame.
        if (global_scheduler_initialized_)
            global_transfer_scheduler_.retireStaging(stamp.slotIndex());

        auto& global_registry = ctx_->globalRegistry();
        for (uint32_t phase = 0; phase < static_cast<uint32_t>(EUploadPhase::Count); ++phase)
        {
            for (auto* svc : global_registry.globalFrameServices(static_cast<EUploadPhase>(phase)))
            {
                if (svc) svc->onBeginFrame(stamp);
            }
        }

        for (auto &scene_ptr : scenes_.values())
        {
            if (scene_ptr)
                scene_ptr->beginFrame(stamp);
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
        if (!global_scheduler_initialized_)
        {
            TransferScheduler::Config ts_cfg{
                .allocator        = ctx_->vmaAllocator(),
                .ring_capacity    = 4 * 1024 * 1024,
                .frames_in_flight = ctx_->framesInFlight(),
            };
            if (global_transfer_scheduler_.init(ts_cfg))
            {
                global_scheduler_initialized_ = true;

                auto& reg = ctx_->globalRegistry();

                // MeshResources + MaterialResources are NOT added here — they are
                // built lazily (StandardMeshStack / StandardMaterial attach, or first
                // upload) and may not exist yet at scheduler init (windowed apps record
                // frames before those features attach). Both are registered below, the
                // frame they first appear.
                // LightResources is per-scene now (M1): registered as a
                // per-scene transfer contributor in the RenderScene ctor.
                if (auto* tex = reg.find<TextureResources>()) {
                    global_transfer_scheduler_.contributors().add(
                        makeTransferContributorWithPost(tex, /*priority=*/10));
                }
            }
        }

        // Register the lazily-built MeshResources as a transfer contributor the
        // frame it first exists (see ensureGlobalMeshResources). Priority 0 keeps
        // it ordered ahead of Material/Texture regardless of add order.
        if (global_scheduler_initialized_ && !mesh_contributor_added_)
        {
            if (auto* mesh = ctx_->globalRegistry().find<MeshResources>())
            {
                global_transfer_scheduler_.contributors().add(
                    makeTransferContributor(mesh, /*priority=*/0));
                mesh_contributor_added_ = true;
            }
        }

        // Same for the lazily-built MaterialResources (see ensureGlobalMaterialResources).
        if (global_scheduler_initialized_ && !material_contributor_added_)
        {
            if (auto* mat = ctx_->globalRegistry().find<MaterialResources>())
            {
                global_transfer_scheduler_.contributors().add(
                    makeTransferContributor(mat, /*priority=*/5));
                material_contributor_added_ = true;
            }
        }

        // ── Execute global transfer scheduler ─────────────────────────
        if (global_scheduler_initialized_)
        {
            global_transfer_scheduler_.resetFrame(stamp.slotIndex());
            global_transfer_scheduler_.executeAll(cmd);
        }

        // Per-scene uploads (instance data, point cloud, trajectory, etc.).
        for (auto &scene_ptr : scenes_.values())
        {
            if (scene_ptr)
                scene_ptr->record(cmd, stamp);
        }
    }

    // ─────────────────────────────────────────────────────────────────────
    //  prepareSceneForRender — shared preamble for renderSceneViews / renderSingleView
    // ─────────────────────────────────────────────────────────────────────

    bool Renderer::prepareSceneForRender(
        RenderScene &scene,
        const RenderTargetLayout &layout)
    {
        auto &gs = scene.graphState();

        // Auto-recompile if graph is invalid (and not suppressed).
        if (!gs.valid && !scene.isGraphRecompileSuppressed())
        {
            if (gs.last_layout.hasSlot(TargetSlot::SceneColor))
                scene.compileGraphTemplate(gs.last_layout);
            else if (layout.hasSlot(TargetSlot::SceneColor))
                scene.compileGraphTemplate(layout);
            else if (!gs.render_skip_warned)
            {
                // No SceneColor target in either the stored or the active layout, so
                // no compile is even attempted and the view renders blank. This used
                // to be wholly silent (M21); surface it once. (Compile *failures* are
                // already logged by RenderScene::compileGraphTemplate.)
                gs.render_skip_warned = true;
                std::cerr << "[Renderer] scene has no SceneColor render target in "
                             "either its stored or the active layout; its views are "
                             "skipped (blank) until a SceneColor target is provided.\n";
            }
        }

        // Recovered → re-arm the one-shot warning for any future regression.
        if (gs.valid)
            gs.render_skip_warned = false;

        return gs.valid;
    }

    void Renderer::renderSingleView(
        RenderScene &scene,
        View &view,
        const RenderTargetBinding &binding,
        const RenderTargetLayout &layout,
        FrameRuntime &rt,
        uint32_t cross_view_index)
    {
        if (!prepareSceneForRender(scene, layout))
            return;

        if (!prepareViewForRender(scene, view, binding, rt))
            return;

        auto &gs = scene.graphState();
        DrawRequest req;
        req.scene = &scene;
        req.view = &view;
        req.target = &binding;
        renderView(scene, gs, view, req, rt, cross_view_index);
    }

    void Renderer::endFrame(uint64_t frame_serial)
    {
        for (auto &scene_ptr : scenes_.values())
        {
            if (scene_ptr)
                scene_ptr->endFrame(frame_serial);
        }

        for (uint32_t phase = 0; phase < static_cast<uint32_t>(EUploadPhase::Count); ++phase)
        {
            for (auto* svc : ctx_->globalRegistry().globalFrameServices(static_cast<EUploadPhase>(phase)))
            {
                if (svc) svc->onEndFrame(current_stamp_);
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────
    //  prepareViewForRender — ensure per-view GPU resources are ready
    // ─────────────────────────────────────────────────────────────────────

    bool Renderer::prepareViewForRender(
        RenderScene &scene,
        View &view,
        const RenderTargetBinding &binding,
        FrameRuntime &rt)
    {
        auto &gs = scene.graphState();
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
            [&scene, current_graph_desc](RGResourceState&& retired_state)
            {
                scene.retireViewResourceState(std::move(retired_state), current_graph_desc);
            });
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
        RenderScene &scene,
        SceneGraphState &gs,
        View &view,
        const DrawRequest &req,
        FrameRuntime &rt,
        uint32_t cross_view_index)
    {
        assert(gs.graph && view.resource_state && "renderView: graph and view resources must be ready");

        // Guard: view must have a valid slot allocated (skips views whose slot
        // hasn't been assigned yet or are pending GC).
        if (!view.view_slot.valid())
            return;

        auto *scene_res = scene.sceneRegistry().find<SceneResources>();
        if (!scene_res)
            return;

        const uint32_t slot = rt.stamp.slotIndex();

        RGFrameContext frame_ctx{};
        frame_ctx.frame_index = slot;
        frame_ctx.image_index = rt.image_index;
        frame_ctx.frame_id = rt.stamp.serial;
        frame_ctx.scene_ds = scene_res->getDescriptorSet(slot);
        frame_ctx.scene_index = scene.sceneGlobalSlot().index;
        frame_ctx.view_index = view.view_slot.index;
        frame_ctx.view = &view;
        frame_ctx.cross_view_index = cross_view_index;

        // Let each enabled feature inject its own extension data.
        for (auto& feat : scene.features())
        {
            if (feat && feat->isEnabled())
                feat->populateFrameContext(frame_ctx);
        }

        // Inject per-slot images/views from the target binding
        if (req.target)
        {
            for (size_t si = 0; si < kTargetSlotCount; ++si)
            {
                const auto &slot_imgs = req.target->slot(static_cast<TargetSlot>(si));
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
            &scene.renderContext().globalRegistry());

        // Expose per-view multi-queue submissions to the frame driver.
        if (view.resource_state->record_ctx.multi_queue_submit.is_multi_queue)
        {
            rt.multi_queue_submits.push_back(
                &view.resource_state->record_ctx.multi_queue_submit);
        }

        // Fold this view's externally-injected sync (addExternalGraphicsWait/Signal,
        // e.g. a CUDA producer) into the frame-level GRAPHICS submit. Empty unless a
        // feature injected in populateFrameContext — a pure no-op for normal rendering.
        if (!frame_ctx.external_graphics_waits.empty())
            rt.external_graphics_waits.insert(
                rt.external_graphics_waits.end(),
                frame_ctx.external_graphics_waits.begin(),
                frame_ctx.external_graphics_waits.end());
        if (!frame_ctx.external_graphics_signals.empty())
            rt.external_graphics_signals.insert(
                rt.external_graphics_signals.end(),
                frame_ctx.external_graphics_signals.begin(),
                frame_ctx.external_graphics_signals.end());
    }

    // ─────────────────────────────────────────────────────────────────────
    //  Accessors
    // ─────────────────────────────────────────────────────────────────────

    RenderContext &Renderer::renderContext() noexcept { return *ctx_; }
    uint32_t Renderer::framesInFlight() const noexcept { return ctx_->framesInFlight(); }

    RenderScene *Renderer::getScene(RenderSceneId id) noexcept
    {
        auto idx = static_cast<uint32_t>(id);
        if (!scenes_.contains(idx))
            return nullptr;
        return scenes_.at(idx).get();
    }

} // namespace lux::render

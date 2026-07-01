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
        // SlotKeyAutoSparseSet::insert assigns a generational RenderSceneId.
        auto scene_id = scenes_.insert(std::move(scene));

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
        const uint64_t fif = ctx_->framesInFlight();
        std::erase_if(retired_scenes_, [&](RetiredScene& r)
        {
            // frame_serial >= retire_serial always (retire happens during a frame
            // whose endFrame runs with frame_serial == that frame). Reuse of the
            // GPU work only completes fif frames after the last submission.
            if (frame_serial - r.retire_serial < fif)
                return false; // GPU may still reference this scene's resources
            r.scene->shutdownFull();
            return true;      // drop — the unique_ptr frees the scene
        });
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

        // The compiled graph template is only valid for the exact layout it was
        // built from. Recompile when the graph is invalid OR when the layout we are
        // about to render to differs from the one it was compiled for (last_layout)
        // — otherwise a scene whose target layout changed (format / usage / slots,
        // or a different target) would render with a graph that mismatches its
        // binding. The requested `layout` is always complete at the render entry
        // (offscreen e.layout / swapchain sb.layout), so prefer it over the stored
        // one when recompiling.
        //   NOTE: with one stable layout per scene (today's usage) layout_changed is
        //   always false → a zero-cost safety net. Two *simultaneous* heterogeneous
        //   layouts on one scene would recompile every frame (thrash) until the
        //   per-key graph-template cache lands (阶段4); that case is broken today and
        //   correct-but-slow after this change.
        // Compare UNCONDITIONALLY (四-1): the old `hasSlot(SceneColor) && …` short
        // circuit meant a NEW layout with no SceneColor never counted as changed, so a
        // stale graph kept being used for the new binding. Any layout difference now
        // triggers a recompile against the REQUESTED layout.
        const bool layout_changed = !(layout == gs.last_layout);

        if ((!gs.valid || layout_changed) && !scene.isGraphRecompileSuppressed())
        {
            // Always (re)compile against the REQUESTED layout — never fall back to the
            // stored last_layout (四-1): rendering a new binding with a graph built for
            // a DIFFERENT layout is exactly the mismatch this guards against.
            // compileGraphTemplate treats a no-SceneColor layout as a valid "no graph"
            // outcome (the scene then renders blank), so it is not special-cased here.
            scene.compileGraphTemplate(layout);

            if (!gs.valid && !layout.hasSlot(TargetSlot::SceneColor) && !gs.render_skip_warned)
            {
                // Surface the blank-render reason once (compile *failures* are logged
                // by compileGraphTemplate itself).
                gs.render_skip_warned = true;
                std::cerr << "[Renderer] scene has no SceneColor render target in the "
                             "active layout; its views are skipped (blank) until one is "
                             "provided.\n";
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
        FrameRuntime &rt,
        uint32_t cross_view_index)
    {
        // The binding is the single source of truth for its target: it carries both
        // the physical images AND the layout the graph must compile against (阶段4 P4d
        // merged the formerly-separate `layout` parameter into it, so the two can no
        // longer disagree). A binding that reaches here without a layout is a caller
        // bug — the swapchain paths set binding.layout per overlay phase before calling
        // (they previously passed it as a separate argument).
        if (binding.layout == nullptr)
        {
            std::cerr << "[Renderer] renderSingleView: binding carries no layout — "
                         "view skipped (阶段4 P4d).\n";
            return;
        }
        const RenderTargetLayout &layout = *binding.layout;

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

        collectRetiredScenes(frame_serial);
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
        // tryGet returns nullptr for a stale id (generation mismatch) as well as
        // an unknown one — no separate validity check needed.
        auto* slot = scenes_.tryGet(id);
        return slot ? slot->get() : nullptr;
    }

} // namespace lux::render

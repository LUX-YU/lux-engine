#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/core/VulkanContext.hpp>
#include <lux/engine/render/RendererContext.hpp>
#include <lux/engine/render/transfer/TransferContributor.hpp>
#include <lux/engine/render/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/pipeline/PipelineManager.hpp>
#include <lux/engine/render/core/DescriptorSetLayoutContract.hpp>
#include <lux/engine/render/resources/SceneResources.hpp>
// (mesh-stack + light resource includes removed — the mesh stack is owned by
//  StandardMeshStackFeature and light by LightFeature now; the core scene names
//  none of these domain types. "core stays domain-free".)
#include <lux/engine/render/core/MaterialFamily.hpp>
#include <lux/engine/render/graph/RGVulkanResourceAllocator.hpp>
#include <lux/engine/render/graph/RGVulkanRecorder.hpp>
#include <lux/engine/render/graph/RenderGraphCompiler.hpp>
#include <lux/engine/render/graph/RGCompiledGraph.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RGGraphResize.hpp>
#include <lux/engine/render/graph/RGDebugPrint.hpp>
#include <lux/engine/render/utils/FormatMap.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>

namespace lux::render
{

    // ─────────────────────────────────────────────────────────────────────
    //  Construction / Destruction
    // ─────────────────────────────────────────────────────────────────────

    RenderScene::RenderScene(std::shared_ptr<RenderContext> ctx)
        : RenderScene(std::move(ctx), Config{}){}

    RenderScene::RenderScene(std::shared_ptr<RenderContext> ctx,
                             const Config &cfg)
        : render_ctx_(std::move(ctx)), config_(cfg), debug_name_(cfg.scene_name)
    {
        retire_owner_token_ = static_cast<FrameRetireScheduler::OwnerToken>(
            reinterpret_cast<uintptr_t>(this));
        pipeline_config_ = cfg.pipeline;

        auto &rctx = *render_ctx_;
        auto &res_ctx = rctx.resourceContext();

        // Per-scene growable descriptor-pool chain — backs every per-scene
        // persistent descriptor set below (light/scene/instance/vertex-pool,
        // plus shadow/skinning added at feature-attach). Layouts stay GLOBAL;
        // only set allocation is per-scene, so the chain auto-scales with scene
        // count and is torn down whole at scene teardown (no leaked sets).
        scene_descriptor_arena_.init(rctx.deviceContext().logicalDevice(),
                                     SceneDescriptorArena::PoolSizeTemplate{});

        // Register per-scene SceneResources in scene_registry_.
        {
            scene_registry_.emplace<SceneResources>();
            SceneResources::InitInfo si{
                .device_context = rctx.deviceContext(),
                .slices = rctx.framesInFlight(),
                .initial_scene_capacity = 8,
                .initial_view_capacity = 8,
                .arena = &scene_descriptor_arena_,
                .set_layout = rctx.descriptorLayouts().getSceneSetLayout(),
            };
            auto* scene_res = scene_registry_.find<SceneResources>();
            scene_res->init(si);
            scene_res->setDeferredQueue(&rctx.deferredDestroyQueue());
        }

        // (LightResources is NOT created here. It is owned by LightFeature, which
        // emplaces + inits it in initAndAttachTo when the scene opts in by adding
        // that feature — emplaced before ShadowResources so reverse-order shutdown
        // tears Shadow down first, ShadowResources holding a raw LightResources*.
        // A scene without LightFeature renders unlit. The core stays domain-free.)

        // (The standard 3D mesh-stack resources — InstanceResources /
        // VertexPoolRegistry / MeshInputPool / VertexProductionRegistry — are NOT
        // created here. They are owned by StandardMeshStackFeature, which ensures +
        // inits them in initAndAttachTo when the scene opts into mesh rendering by
        // adding that feature (BEFORE the mesh consumers cache their pointers). A
        // scene without it — 2D / headless / compute-only — pays nothing: no ~96MB
        // vertex/index arenas, no instance SSBO, no Set-7 vertex pool. Core stays
        // domain-free.)

        // (Large-world SpatialCullGrid is NOT created here. It is owned by
        // SpatialCullFeature, which emplaces + inits it in initAndAttachTo when the
        // scene opts in by adding that feature. The general core stays domain-free;
        // a scene that doesn't add SpatialCullFeature pays nothing.)

        // Acquire a scene-global slot from the per-scene SceneResources.
        scene_global_slot_ = scene_registry_.find<SceneResources>()->allocateScene();

        // ── Unified transfer scheduler ──────────────────────────────────
        {
            TransferScheduler::Config ts_cfg{
                .allocator        = rctx.vmaAllocator(),
                .ring_capacity    = 4 * 1024 * 1024, // 4 MiB
                .frames_in_flight = rctx.framesInFlight(),
            };
            (void)transfer_scheduler_.init(ts_cfg);

            // (InstanceResources' transfer contributor is registered by
            // StandardMeshStackFeature in initAndAttachTo — the scene ctor no longer
            // knows the mesh stack.)

            // Register SceneResources (HOST_WRITE barriers merged into post-batch).
            auto* sr = scene_registry_.find<SceneResources>();
            transfer_scheduler_.contributors().add(
                makeTransferContributor(sr, /*priority=*/10));

            // (LightResources transfer contributor is registered by LightFeature in
            // initAndAttachTo — the scene ctor no longer knows light. A scene without
            // LightFeature contributes no light SSBO flush.)
        }

        // Per-scene render-graph infrastructure
        allocator_ = std::make_unique<RGVulkanResourceAllocator>(res_ctx);
        recorder_ = std::make_unique<RGVulkanRecorder>(
            res_ctx, rctx.pipelineManager());

        forEachFeature(features_.values(), [&](RenderFeature &f)
                       { f.configureMaterialPipelines(material_pipeline_); });

        markFeatureCacheDirty();
        markViewCacheDirty();

        initialized_ = true;
    }

    RenderScene::~RenderScene()
    {
        if (initialized_)
            shutdownFull();
    }

    // ─────────────────────────────────────────────────────────────────────
    //  Feature Management
    // ─────────────────────────────────────────────────────────────────────

    uint32_t RenderScene::addFeatureImpl(
        std::unique_ptr<RenderFeature> feature,
        uint32_t extractor_type_id)
    {
        feature->scene_ = this;
        feature->initAndAttachTo(*this);

        auto *raw = feature.get();
        raw->extractor_type_id_ = extractor_type_id;
        render_object_extractor_.registerProvider(raw, extractor_type_id);

        // If scene is already initialized, run lifecycle for this feature
        if (initialized_ && raw->isEnabled())
        {
            raw->configureMaterialPipelines(material_pipeline_);
        }

        // Insert into SparseSet — auto-assigns the feature_id
        size_t id = features_.insert(std::move(feature));
        raw->feature_id_ = static_cast<uint32_t>(id);
        markFeatureCacheDirty();
        // Adding a feature changes the pass set; force graph recompile on next render.
        graph_state_.valid = false;

        return raw->feature_id_;
    }

    uint32_t RenderScene::addFeatureErased(
        std::unique_ptr<RenderFeature> feature,
        uint32_t extractor_type_id)
    {
        return addFeatureImpl(std::move(feature), extractor_type_id);
    }

    RenderFeature *RenderScene::getFeature(uint32_t feature_id) const
    {
        if (!features_.contains(feature_id))
            return nullptr;
        return features_.at(feature_id).get();
    }

    bool RenderScene::removeFeature(uint32_t feature_id)
    {
        if (!features_.contains(feature_id))
            return false;

        auto &ptr = features_.at(feature_id);
        render_object_extractor_.unregisterProvider(ptr.get());

        ptr->onDetachFromScene(*this);
        // Erase (destroying the feature) while scene_ is still valid,
        // so the destructor's destroy() → renderContext() path works safely.
        features_.erase(feature_id);
        markFeatureCacheDirty();
        graph_state_.valid = false; // removing a feature invalidates the graph
        return true;
    }

    void RenderScene::removeAllFeatures()
    {
        // Collect ids first since erase during iteration may invalidate
        std::vector<uint32_t> ids;
        ids.reserve(features_.size());
        for (auto &f : features_.values())
            if (f)
                ids.push_back(f->feature_id_);

        for (auto id : ids)
            removeFeature(id);
    }

    void RenderScene::setFeatureEnabled(uint32_t feature_id, bool enabled)
    {
        if (auto *f = getFeature(feature_id))
        {
            if (f->isEnabled() == enabled)
                return;
            f->setEnabled(enabled);
            markFeatureCacheDirty();
            graph_state_.valid = false; // trigger recompile next frame
        }
    }

    void RenderScene::setFeatureEnabled(std::string_view feature_name, bool enabled)
    {
        for (auto &f : features_.values())
            if (f->name() == feature_name)
            {
                if (f->isEnabled() == enabled)
                    return;
                f->setEnabled(enabled);
                markFeatureCacheDirty();
                graph_state_.valid = false; // trigger recompile next frame
                return;
            }
    }

    std::vector<RenderScene::FeatureInfo> RenderScene::queryFeatures() const
    {
        std::vector<FeatureInfo> infos;
        infos.reserve(features_.size());
        for (const auto &f : features_.values())
            infos.push_back({f->name(), f->isEnabled()});
        return infos;
    }

    std::vector<RenderScene::FeatureParamDesc> RenderScene::queryFeatureParamDescs() const
    {
        std::vector<FeatureParamDesc> descs;
        descs.reserve(features_.size());
        for (const auto &f : features_.values())
        {
            if (!f) continue;
            const std::string_view sn = f->paramStructName();
            const bool has_params = !sn.empty();
            const void* data = has_params ? f->paramData() : nullptr;
            // Enforce data/size consistency: a null pointer always reports size 0
            // so the packed enumerate stream never claims bytes it can't supply.
            const std::size_t size = (has_params && data) ? f->paramSize() : 0;
            descs.push_back(FeatureParamDesc{
                f->featureId(),
                f->isEnabled(),
                f->name(),
                sn,
                data,
                size});
        }
        return descs;
    }

    // ─────────────────────────────────────────────────────────────────────
    //  Render Path Selection
    // ─────────────────────────────────────────────────────────────────────

    void RenderScene::setRenderPath(ERenderPath path)
    {
        if (path == render_path_)
            return;
        render_path_ = path;
        graph_state_.valid = false;
    }

    // ─────────────────────────────────────────────────────────────────────
    //  View Management
    // ─────────────────────────────────────────────────────────────────────
    uint32_t RenderScene::addView(const ViewCreateInfo &info)
    {
        auto view = std::make_unique<View>();
        view->current_extent = info.initial_extent;
        view->debug_name     = info.debug_name ? info.debug_name : "View";

        // AutoSparseSet::insert returns the assigned index
        size_t id        = views_.insert(std::move(view));
        uint32_t handle  = static_cast<uint32_t>(id);
        auto* new_view   = views_.at(handle).get();
        new_view->handle = handle;
        new_view->state  = ViewState::Active;

        // Allocate per-view UBO buffers + descriptor sets
        initViewUBO(*new_view);

        markViewCacheDirty();
        rebuildEnabledFeatureCacheIfNeeded();

        // call allocateViewState for each enabled feature
        for (auto* feat : enabled_features_dense_)
            feat->allocateViewState(handle, *this);

        return handle;
    }

    void RenderScene::removeView(uint32_t handle)
    {
        if (!views_.contains(handle))
            return;

        rebuildEnabledFeatureCacheIfNeeded();
        for (auto *feat : enabled_features_dense_)
            feat->deallocateViewState(handle);

        // Only ShadowResources subscribes to view-destroy (per-view cache
        // eviction), and it is per-scene now (Plan A) — so the scene registry
        // notify reaches it. The old global-registry notify is a dead no-op
        // (no global frame service overrides onViewDestroyed) and is dropped.
        scene_registry_.notifySceneViewDestroyed(scene_global_slot_.index, handle);

        auto &view = *views_.at(handle);
        // Do NOT call destroyViewUBO here — the view's ViewBuffer slot may still be
        // referenced by in-flight GPU commands for the other frames-in-flight slots.
        // Deferring to endFrame() GC ensures the GPU has finished all work before
        // the slot is freed.
        view.state = ViewState::Destroying;
        markViewCacheDirty();
        pending_destroys_.push_back({handle, 0});
    }

    View *RenderScene::getView(uint32_t handle) noexcept
    {
        if (!views_.contains(handle))
            return nullptr;
        auto &ptr = views_.at(handle);
        return ptr ? ptr.get() : nullptr;
    }

    const View *RenderScene::getView(uint32_t handle) const noexcept
    {
        if (!views_.contains(handle))
            return nullptr;
        auto &ptr = views_.at(handle);
        return ptr ? ptr.get() : nullptr;
    }

    size_t RenderScene::activeViewCount() const noexcept
    {
        rebuildActiveViewCacheIfNeeded();
        return active_views_dense_.size();
    }

    void RenderScene::endViewFrame(uint32_t /*frame_slot*/)
    {
        rebuildActiveViewCacheIfNeeded();
        for (auto* view : active_views_dense_)
            view->frame_systems_done = false;
        // GC for pending_destroys_ is handled in endFrame() using the
        // monotonic frame_id to avoid the cyclic-index sentinel ambiguity.
    }

    // ─────────────────────────────────────────────────────────────────────
    //  Full Initialisation
    // ─────────────────────────────────────────────────────────────────────

    // (init() removed — RAII constructor handles all initialisation)

    // ─────────────────────────────────────────────────────────────────────
    //  Infra Accessors
    // ─────────────────────────────────────────────────────────────────────

    MaterialPipeline &RenderScene::materialPipeline() noexcept
    {
        return material_pipeline_;
    }

    RGVulkanResourceAllocator &RenderScene::graphAllocator() noexcept
    {
        return *allocator_;
    }

    RGVulkanRecorder &RenderScene::graphRecorder() noexcept
    {
        return *recorder_;
    }

    SceneGraphState &RenderScene::graphState() noexcept
    {
        return graph_state_;
    }

    const SceneGraphState &RenderScene::graphState() const noexcept
    {
        return graph_state_;
    }

    void RenderScene::compileGraphTemplate(const RenderTargetLayout &layout)
    {
        auto &rctx = *render_ctx_;

        // Store for recompilation
        graph_state_.last_layout = layout;

        // Retire previous graph if it exists (deferred destroy after FIF frames)
        const RGGraphDescription* old_graph_desc = nullptr;
        if (graph_state_.graph)
        {
            old_graph_desc = &graph_state_.graph->original_graph;
            RetiredGraph retired;
            retired.graph = std::move(graph_state_.graph);
            retired.retire_frame = 0; // will be filled by endFrame tracking
            retired_graphs_.push_back(std::move(retired));
            graph_state_.valid = false;
        }

        // Invalidate all view resource states up-front (keyed on the just-retired
        // old graph for pool return). This MUST run before the early returns below:
        // otherwise an invalid-layout or compile-failure recompile retires the old
        // graph but leaves every view's physical images + record context allocated
        // against it (VRAM held until a later success) and bypasses the
        // transient-pool return invariant (endFrame would deallocate, not
        // deallocateToPool, because graph_state_.graph is now null). (P1#37)
        invalidateAllViewResources(old_graph_desc);

        if (!layout.hasSlot(TargetSlot::SceneColor))
        {
            return;
        }

        const auto &color_slot = layout.slot(TargetSlot::SceneColor);

        // 1. Set up RGBuilder with backbuffer + depth
        RGBuilder builder;

        // Use a placeholder extent (1x1) for the template — actual images are injected per-frame
        RGTextureDescription bb_desc = RGTextureDescription::Absolute(
            1, 1, mapVkFormat(color_slot.format)
        );

        RGImportedResourceInfo bb_import{};
        bb_import.image_getter     = nullptr; // no getter; images injected via imported_slots
        bb_import.update_group     = static_cast<uint32_t>(RGUpdateGroup::GROUP_SWAPCHAIN);
        bb_import.initial_layout   = color_slot.initial_layout;
        bb_import.final_layout     = color_slot.final_layout;
        bb_import.preserve_content = color_slot.preserve_content;

        auto backbuffer = builder.importSlottedTexture(
            TargetSlot::SceneColor, "SceneColor", bb_desc, bb_import);

        RGTextureDescription depth_desc = RGTextureDescription::Relative(
            1.0f, 1.0f, lux::common::ETextureFormat::D32_SFLOAT
        );
        depth_desc.usage = static_cast<uint32_t>(ERGTextureUsageBits::DEPTH_STENCIL) | static_cast<uint32_t>(ERGTextureUsageBits::SAMPLED);
        auto scene_depth = builder.createTexture("SceneDepth", depth_desc);

        // 2. Collect enabled features and call addPasses
        std::vector<RenderFeature *> feature_ptrs;
        rebuildEnabledFeatureCacheIfNeeded();
        feature_ptrs.reserve(enabled_features_dense_.size());
        feature_ptrs.insert(feature_ptrs.end(),
            enabled_features_dense_.begin(),
            enabled_features_dense_.end()
        );

        for (auto *feature : feature_ptrs)
            feature->addPasses(builder);

        // 4. Static compile (no resource allocation)
        auto compiled = RenderGraphCompiler::compile(
            std::move(builder).build(),
            rctx.pipelineManager()
        );

        if (!compiled.valid)
        {
            if (!compiled.compile_error.empty())
                std::cerr << "[RenderScene] Graph compile failed for scene '" << debug_name_
                          << "': " << compiled.compile_error << "\n";
            graph_state_.valid = false;
            return;
        }

        graph_state_.graph = std::make_unique<RGCompiledGraph>(std::move(compiled));
        graph_state_.final_color_handle = backbuffer;
        graph_state_.valid = true;

        // Optional graph dump for editor debugging — set LUX_DUMP_RG=1 in the
        // environment to write the compiled execution order + pass details to
        // stderr each time the scene's render graph is (re)compiled.
        if (const char* dump = std::getenv("LUX_DUMP_RG"); dump && dump[0] != '0' && dump[0] != '\0')
        {
            std::cerr << "[RenderScene] '" << debug_name_ << "' graph (re)compiled:\n";
            printCompiledGraph(*graph_state_.graph, std::cerr);
        }
        // (view resource states already invalidated up-front, before the early
        // returns — see P1#37 above)
    }

    void RenderScene::invalidateAllViewResources(const RGGraphDescription* source_graph) noexcept
    {
        for (auto &v : views_.values())
        {
            if (v && v->resource_state)
            {
                retired_view_resources_.push_back(
                    RetiredViewResources{std::move(v->resource_state), 0, source_graph}
                );
            }
        }
    }

    void RenderScene::retireViewResourceState(RGResourceState&& state, const RGGraphDescription* source_graph)
    {
        if (state.record_ctx.frames_in_flight == 0)
            return;

        auto retired = std::make_unique<RGResourceState>(std::move(state));
        retired_view_resources_.push_back(RetiredViewResources{std::move(retired), 0, source_graph});
    }

    // ─────────────────────────────────────────────────────────────────────
    //  Scene Data
    // ─────────────────────────────────────────────────────────────────────

    RenderContext &RenderScene::renderContext() noexcept
    {
        return *render_ctx_;
    }

    const RenderContext &RenderScene::renderContext() const noexcept
    {
        return *render_ctx_;
    }

    const std::vector<std::unique_ptr<RenderFeature>> &RenderScene::features() const noexcept
    {
        return features_.values();
    }

    // ─────────────────────────────────────────────────────────────────────
    //  Per-Frame Lifecycle
    // ─────────────────────────────────────────────────────────────────────

    void RenderScene::dumpCompiledGraph(std::ostream& os) const
    {
        if (graph_state_.valid && graph_state_.graph)
            printCompiledGraph(*graph_state_.graph, os);
        else
            os << "[RenderScene] '" << debug_name_
               << "' has no compiled render graph yet (not rendered, or compile failed).\n";
    }

    void RenderScene::beginFrame(const FrameStamp& stamp)
    {
        current_stamp_ = stamp;
        // Reset the domain-neutral instance cull-mask address each frame BEFORE the
        // feature onFrameBegin loop. A provider feature (e.g. SpatialCullFeature) sets
        // it below; with no provider it stays 0 (= every instance active). The core
        // never names SpatialCullGrid — large-world is just one possible provider.
        instance_cull_mask_addr_ = 0;
        const uint32_t frame_slot = stamp.slotIndex();
        rebuildActiveViewCacheIfNeeded();
        rebuildEnabledFeatureCacheIfNeeded();

        for (uint32_t phase = 0; phase < static_cast<uint32_t>(EUploadPhase::Count); ++phase)
        {
            for (auto* svc : scene_registry_.sceneFrameServices(static_cast<EUploadPhase>(phase)))
            {
                if (svc) svc->onBeginFrame(stamp);
            }
        }

        // Write scene-global + per-view data into SceneResources after it has
        // advanced to this slot AND the slot fence has been waited (we are inside
        // FrameDriver::beginFrame's caller). updateView() only caches the data
        // (it runs pre-fence-wait during the drain); the actual mapped-SSBO write
        // for the current slot happens HERE so it never races the in-flight frame.
        if (auto *scene_res = scene_registry_.find<SceneResources>())
        {
            SceneGlobalGpuData sg{};
            sg.time_sec = scene_time_;
            sg.delta_time = scene_delta_time_;
            sg.frame_number = static_cast<uint32_t>(scene_total_frames_);
            sg.pad0 = 0.f;
            scene_res->writeSceneGlobal(scene_global_slot_, sg);

            for (auto *view : active_views_dense_)
            {
                if (!view || !view->has_view_data)
                    continue;
                // Upload the neutral per-view bytes the scene staged in updateView
                // (the core does not interpret them; #27: write happens here, after
                // the fence wait, never during the request drain).
                scene_res->writeViewData(view->view_slot,
                    view->view_data_staging.data(), kViewDataStrideBytes, frame_slot);
            }
        }

        // (Large-world cell streaming is no longer driven here. It lives entirely in
        // SpatialCullFeature::onFrameBegin — which runs in the loop below — and feeds
        // its mask address through the domain-neutral instance_cull_mask_addr_ set
        // above. The general scene knows nothing about SpatialCullGrid.)

        allocator_->garbageCollect(stamp.serial, render_ctx_->framesInFlight() + 1);

        const FeatureFrameContext frame_ctx{
            frame_slot
        };
        for (auto* feature : enabled_features_dense_)
            feature->onFrameBegin(frame_ctx);
    }

    // RenderScene::updateView was REMOVED (View 去 3D 化). Per-view camera data is a
    // feature domain now: StandardViewCamera's op handler fills ViewCameraResource +
    // the View's neutral GPU staging (view_data_staging / frustum_staging) directly,
    // CPU-side during the op dispatch; beginFrame uploads the staging after the slot
    // fence wait (#27). The core scene no longer names camera matrices / frustum and
    // no longer broadcasts onFrustumUpdated to every feature.

    void RenderScene::endFrame(uint64_t frame_id)
    {
        const uint32_t fif = render_ctx_->framesInFlight();

        // Garbage-collect retired per-view resources (physical images + record ctx)
        // MUST run BEFORE retired_graphs_ because source_graph points into those graphs.
        // First pass: stamp all unstamped entries
        for (auto &rv : retired_view_resources_)
        {
            if (rv.retire_frame == 0)
                rv.retire_frame = frame_id;
        }
        // Second pass: pop entries that have aged past FIF
        while (!retired_view_resources_.empty())
        {
            auto &oldest = retired_view_resources_.front();
            if (frame_id - oldest.retire_frame < fif)
                break;

            if (recorder_)
                recorder_->deallocateRecordContext(oldest.state->record_ctx);
            if (allocator_ && oldest.source_graph)
                allocator_->deallocateToPool(oldest.state->physical_resources, *oldest.source_graph);
            else if (allocator_)
                allocator_->deallocate(oldest.state->physical_resources);
            retired_view_resources_.pop_front();
        }

        // Garbage-collect retired graphs that have aged past frames-in-flight.
        // Runs AFTER retired_view_resources_ to keep source_graph pointers alive.
        // First pass: stamp all unstamped entries with the current frame_id
        for (auto &rg : retired_graphs_)
        {
            if (rg.retire_frame == 0)
                rg.retire_frame = frame_id;
        }
        // Second pass: pop entries that have aged past FIF
        while (!retired_graphs_.empty())
        {
            auto &oldest = retired_graphs_.front();

            if (frame_id - oldest.retire_frame < fif)
                break;

            // Safe to free — all view resources referencing this graph are already cleaned up
            retired_graphs_.pop_front();
        }

        // Garbage-collect views whose UBO buffers are now safe to free.
        // Uses the same monotonic frame_id sentinel pattern as retired_graphs_.
        for (auto &pd : pending_destroys_)
        {
            if (pd.destroy_frame == 0)
                pd.destroy_frame = frame_id;
        }

        while (!pending_destroys_.empty())
        {
            auto &oldest = pending_destroys_.front();
            if (frame_id - oldest.destroy_frame >= fif)
            {
                // GPU has finished all work referencing these resources.
                if (views_.contains(oldest.view_id))
                {
                    auto& view_ptr = views_.at(oldest.view_id);
                    if (view_ptr && view_ptr->resource_state)
                    {
                        if (recorder_)
                            recorder_->deallocateRecordContext(view_ptr->resource_state->record_ctx);
                        if (allocator_)
                            allocator_->deallocate(view_ptr->resource_state->physical_resources);
                        view_ptr->resource_state.reset();
                    }
                    if (view_ptr)
                        destroyViewUBO(*view_ptr);
                    views_.erase(oldest.view_id);
                }
                pending_destroys_.pop_front();
            }
            else
            {
                break;
            }
        }

        for (uint32_t phase = 0; phase < static_cast<uint32_t>(EUploadPhase::Count); ++phase)
        {
            for (auto* svc : scene_registry_.sceneFrameServices(static_cast<EUploadPhase>(phase)))
            {
                if (svc) svc->onEndFrame(current_stamp_);
            }
        }

        // Forward endFrame to each active view
        forEachActiveView(
            [&](View &v)
            { 
                v.frame_systems_done = false; 
            }
        );
    }

    // ─────────────────────────────────────────────────────────────────────
    //  View Lifecycle (all View state is driven by RenderScene)
    // ─────────────────────────────────────────────────────────────────────

    void RenderScene::initViewUBO(View &view)
    {
        if (auto *scene_res = scene_registry_.find<SceneResources>())
            view.view_slot = scene_res->allocateView();
    }

    void RenderScene::destroyViewUBO(View &view)
    {
        if (auto *scene_res = scene_registry_.find<SceneResources>())
            scene_res->freeView(view.view_slot);
    }

    void RenderScene::resizeView(View &view, common::Size2D new_extent)
    {
        if (new_extent.width == view.current_extent.width &&
            new_extent.height == view.current_extent.height)
            return;
        view.current_extent = new_extent;
        view.resize_pending = true;
    }

    void RenderScene::endViewFrame(View &view)
    {
        view.frame_systems_done = false;
    }

    // ─────────────────────────────────────────────────────────────────────
    //  Shutdown
    // ─────────────────────────────────────────────────────────────────────

    void RenderScene::shutdownFull()
    {
        if (!initialized_)
            return;

        // Destroy all view UBO resources and per-view GPU state before clearing
        for (auto &v : views_.values())
        {
            if (!v) continue;
            if (v->resource_state)
            {
                if (recorder_)
                    recorder_->deallocateRecordContext(v->resource_state->record_ctx);
                if (allocator_)
                    allocator_->deallocate(v->resource_state->physical_resources);
                v->resource_state.reset();
            }
            destroyViewUBO(*v);
        }
        views_.clear();
        active_views_dense_.clear();
        view_cache_dirty_ = false;

        for (auto &f : features_.values())
        {
            if (!f)
                continue;
            render_object_extractor_.unregisterProvider(f.get());
            f->onDetachFromScene(*this);
        }
        // Clear (destroy) features while scene_ is still valid, so that
        // destructors calling destroy()→renderContext() can safely access the
        // scene.  Null out scene_ only after the feature objects are gone.
        features_.clear();
        enabled_features_dense_.clear();
        feature_cache_dirty_ = false;
        // (scene_ pointers in the now-destroyed features are irrelevant)
        render_object_extractor_.clearProviders();

        // Free retired graphs
        for (auto &retired : retired_graphs_)
        {
            (void)retired; // graph unique_ptr auto-destroys
        }
        retired_graphs_.clear();

        // Free retired per-view resources
        for (auto &retired : retired_view_resources_)
        {
            if (retired.state)
            {
                if (recorder_)
                    recorder_->deallocateRecordContext(retired.state->record_ctx);
                if (allocator_)
                    allocator_->deallocate(retired.state->physical_resources);
            }
        }
        retired_view_resources_.clear();

        // Free current graph
        graph_state_.graph.reset();
        graph_state_.valid = false;

        recorder_.reset();
        allocator_.reset();

        // Drop pending retirement callbacks owned by this scene before
        // scene-local resources are destroyed.
        render_ctx_->retireScheduler().purge(retire_owner_token_);

        // Free the scene-global slot, then shutdown per-scene resources.
        if (auto *sr = scene_registry_.find<SceneResources>())
            sr->freeScene(scene_global_slot_);

        // Shutdown transfer scheduler before registry (scheduler references
        // resources owned by registry).
        transfer_scheduler_.shutdown();
        scene_registry_.shutdown();

        // Destroy the per-scene descriptor-pool chain LAST — this frees every
        // descriptor set the scene allocated in one shot (the per-scene
        // resources only drop their set handles in shutdown(), they never
        // vkFreeDescriptorSets). Safe: Renderer::removeScene waitIdle's first.
        scene_descriptor_arena_.destroy();

        initialized_ = false;
    }

    // ─────────────────────────────────────────────────────────────────────
    //  GPU Resource Upload / Frame Commands
    // ─────────────────────────────────────────────────────────────────────

    void RenderScene::recordUploads(VkCommandBuffer cmd, const FrameStamp& stamp)
    {
        const uint32_t frame_slot = stamp.slotIndex();
        const uint32_t staging_slot = frame_slot % kMaxFramesInFlight;

        // ── Transfer scheduler path ─────────────────────────────────────
        transfer_scheduler_.resetFrame(staging_slot);
        transfer_scheduler_.retireStaging(staging_slot);

        // (PointCloud / Trajectory transfer-contributor registration is performed
        // by the OWNING feature in initAndAttachTo now — like Light / mesh-stack —
        // so the core scene no longer names point-cloud / trajectory domain types.)
        transfer_scheduler_.executeAll(cmd);
    }

    void RenderScene::rebuildActiveViewCacheIfNeeded() const
    {
        if (!view_cache_dirty_) return;
        active_views_dense_.clear();
        active_views_dense_.reserve(views_.size());
        for (const auto& view_ptr : views_.values())
        {
            if (view_ptr && view_ptr->state == ViewState::Active)
                active_views_dense_.push_back(view_ptr.get());
        }
        view_cache_dirty_ = false;
    }

    void RenderScene::rebuildEnabledFeatureCacheIfNeeded() const
    {
        if (!feature_cache_dirty_) return;
        enabled_features_dense_.clear();
        enabled_features_dense_.reserve(features_.size());
        for (const auto& feature_ptr : features_.values())
        {
            if (feature_ptr && feature_ptr->isEnabled())
                enabled_features_dense_.push_back(feature_ptr.get());
        }
        feature_cache_dirty_ = false;
    }

} // namespace lux::render

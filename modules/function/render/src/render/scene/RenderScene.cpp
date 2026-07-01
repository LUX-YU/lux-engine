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

    bool RenderScene::ensureFeatureViewState(RenderFeature& feature, uint32_t view_index)
    {
        // Keyed by the feature's index (generation validated at the features_
        // lookup boundary; the matrix only tracks live (feature, view) pairs).
        auto& owned = feature_view_states_[feature.featureId().index];
        if (owned.contains(view_index))
            return true;
        if (!feature.allocateViewState(view_index, *this))
            return false;
        owned.insert(view_index);
        return true;
    }

    void RenderScene::releaseFeatureViewState(RenderFeature& feature, uint32_t view_index) noexcept
    {
        auto it = feature_view_states_.find(feature.featureId().index);
        if (it == feature_view_states_.end() || !it->second.contains(view_index))
            return;
        feature.deallocateViewState(view_index);
        it->second.erase(view_index);
    }

    FeatureHandle RenderScene::addFeatureImpl(
        std::unique_ptr<RenderFeature> feature,
        uint32_t extractor_type_id)
    {
        // Unified install entry (三-2): dependency / conflict / multiplicity validation
        // lives HERE — the single internal install path that the comm AddFeature
        // handler, the bulk createScene path AND a direct in-module addFeature<T>() all
        // funnel through — instead of being duplicated in the comm handlers. The
        // descriptor comes from the FeatureInstallScope the comm paths wrap create_fn
        // in; a bare addFeature<T>() has none (untyped → no declared relationships →
        // nothing to validate), so it installs unchecked as before. Rejection here
        // returns an invalid handle BEFORE any attach / insert / provider registration,
        // so there is nothing to roll back.
        const FeatureDescriptor* desc = pending_install_descriptor_;
        if (desc && desc->valid())
        {
            if (desc->multiplicity == FeatureMultiplicity::SinglePerScene && hasFeatureOfType(desc->type))
            {
                std::cerr << "[RenderScene] install '" << desc->name
                          << "' rejected: SinglePerScene — an instance already exists.\n";
                return FeatureHandle{};
            }
            for (FeatureTypeId c : desc->conflicts)
                if (hasFeatureOfType(c))
                {
                    std::cerr << "[RenderScene] install '" << desc->name
                              << "' rejected: conflicts with an installed feature.\n";
                    return FeatureHandle{};
                }
            for (const auto& dep : desc->dependencies)
                if (!dep.optional && !hasFeatureOfType(dep.type))
                {
                    std::cerr << "[RenderScene] install '" << desc->name
                              << "' rejected: a required dependency is not installed "
                                 "(install it first).\n";
                    return FeatureHandle{};
                }
        }

        feature->scene_ = this;
        // Apply the type-level descriptor BEFORE attach so a feature can observe its
        // own descriptor during initAndAttachTo. (三-2)
        if (desc && desc->valid())
            feature->descriptor_ = *desc;
        feature->lifecycle_state_ = FeatureState::Attaching;
        if (auto attached = feature->initAndAttachTo(*this); !attached)
        {
            // attach failed (三-3): nothing has been registered/inserted yet, so just
            // drop the half-attached feature — its destructor frees whatever it
            // allocated during the partial attach. The caller sees an invalid handle.
            std::cerr << "[RenderScene] install rejected: feature attach failed ("
                      << attached.error().message() << ").\n";
            return FeatureHandle{};
        }

        auto *raw = feature.get();
        raw->extractor_type_id_ = extractor_type_id;
        render_object_extractor_.registerProvider(raw, extractor_type_id);

        // Insert into the slot map — auto-assigns a generational FeatureHandle.
        // (unique_ptr move transfers ownership but not address, so `raw` stays valid.)
        raw->feature_id_ = features_.insert(std::move(feature));

        // A feature is added in the ENABLED state — there is no "add disabled" path
        // today. FeatureState is the single source of truth for enabled-ness (三-1),
        // so we land it here; isEnabled() derives from it.
        raw->lifecycle_state_ = FeatureState::Enabled;

        // Enabled-state initialisation for an already-live scene.
        if (initialized_)
        {
            raw->configureMaterialPipelines(material_pipeline_);

            // Back-fill per-view state for views that ALREADY exist: a feature added
            // to a live scene must observe current views, mirroring addView()'s allocate.
            // TRANSACTIONAL (三-3): if any view's allocateViewState() fails, roll back the
            // per-view state already created for this feature and ABORT the install — the
            // caller observes NO half-attached feature (returns an invalid handle). Today
            // no built-in feature fails allocateViewState, so this is a strong-guarantee
            // safety net rather than a behaviour change.
            rebuildActiveViewCacheIfNeeded();
            std::vector<uint32_t> created;
            created.reserve(active_views_dense_.size());
            bool ok = true;
            for (auto* view : active_views_dense_)
            {
                if (ensureFeatureViewState(*raw, view->handle.index))
                    created.push_back(view->handle.index);
                else { ok = false; break; }
            }
            if (!ok)
            {
                for (uint32_t v : created)
                    releaseFeatureViewState(*raw, v);
                feature_view_states_.erase(raw->feature_id_.index);
                raw->lifecycle_state_ = FeatureState::Failed;
                render_object_extractor_.unregisterProvider(raw);
                raw->onDetachFromScene(*this);
                features_.erase(raw->feature_id_);
                markFeatureCacheDirty();
                graph_state_.valid = false;
                return FeatureHandle{};   // invalid → install rejected, no leak
            }
        }

        markFeatureCacheDirty();
        // Adding a feature changes the pass set; force graph recompile on next render.
        graph_state_.valid = false;

        return raw->feature_id_;
    }

    FeatureHandle RenderScene::addFeatureErased(
        std::unique_ptr<RenderFeature> feature,
        uint32_t extractor_type_id)
    {
        return addFeatureImpl(std::move(feature), extractor_type_id);
    }

    RenderFeature *RenderScene::getFeature(FeatureHandle feature_id) const
    {
        // tryGet rejects a stale handle (generation mismatch) as well as unknown.
        auto* slot = features_.tryGet(feature_id);
        return slot ? slot->get() : nullptr;
    }

    void RenderScene::setFeatureDescriptor(FeatureHandle feature_id, const FeatureDescriptor& descriptor) noexcept
    {
        if (auto* f = getFeature(feature_id))
            f->descriptor_ = descriptor;   // RenderScene is a friend of RenderFeature
    }

    bool RenderScene::hasFeatureOfType(FeatureTypeId type) const noexcept
    {
        if (type == kInvalidFeatureTypeId)
            return false;
        for (const auto& f : features_.values())
            if (f && f->descriptor_.type == type)
                return true;
        return false;
    }

    bool RenderScene::removeFeature(FeatureHandle feature_id)
    {
        // Public removal enforces reverse-dependency protection (三-4).
        return removeFeatureInternal(feature_id, /*check_reverse_deps=*/true);
    }

    bool RenderScene::removeFeatureInternal(FeatureHandle feature_id, bool check_reverse_deps)
    {
        if (!features_.contains(feature_id))
            return false;

        auto &ptr = features_.at(feature_id);

        // Reverse-dependency guard (三-4): refuse to remove a feature that another
        // INSTALLED feature still REQUIRES (a non-optional dependency on this type).
        // Default policy is reject — the caller must remove the dependent first —
        // which also prevents the "provider gone, consumer dangling" hazard (e.g.
        // removing Light out from under ShadowMap). Skipped for whole-scene teardown
        // (removeAllFeatures passes check=false): there every feature is going away,
        // so the guard must not block its own bulk removal. Untyped features
        // (kInvalidFeatureTypeId) are never depended upon, so they bypass the scan.
        if (check_reverse_deps)
        {
            const FeatureTypeId target_type = ptr->descriptor_.type;
            if (target_type != kInvalidFeatureTypeId)
            {
                for (const auto& other : features_.values())
                {
                    if (!other || other.get() == ptr.get())
                        continue;
                    for (const auto& dep : other->descriptor_.dependencies)
                        if (!dep.optional && dep.type == target_type)
                        {
                            std::cerr << "[RenderScene] removeFeature '" << ptr->name()
                                      << "' rejected: still required by '" << other->name()
                                      << "' (remove the dependent first).\n";
                            return false;
                        }
                }
            }
        }

        ptr->lifecycle_state_ = FeatureState::Detaching;
        render_object_extractor_.unregisterProvider(ptr.get());

        // Tear down per-view state this feature allocated BEFORE detaching it.
        // removeView() only fires deallocateViewState() on view removal, never on
        // feature removal — so without this a removed feature's per-view eviction
        // hook never runs (leak; e.g. StandardViewCamera's ViewCameraResource
        // entries). Snapshot the ids first: releaseFeatureViewState() mutates the set.
        if (auto it = feature_view_states_.find(feature_id.index); it != feature_view_states_.end())
        {
            std::vector<uint32_t> owned_views(it->second.begin(), it->second.end());
            for (uint32_t view_id : owned_views)
                releaseFeatureViewState(*ptr, view_id);
            feature_view_states_.erase(feature_id.index);
        }

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
        std::vector<FeatureHandle> ids;
        ids.reserve(features_.size());
        for (auto &f : features_.values())
            if (f)
                ids.push_back(f->feature_id_);

        // Whole-scene teardown: bypass the reverse-dependency guard — every feature
        // is going away, so a dependent still being present must not block removal.
        for (auto id : ids)
            removeFeatureInternal(id, /*check_reverse_deps=*/false);
    }

    void RenderScene::setFeatureEnabled(FeatureHandle feature_id, bool enabled)
    {
        auto *f = getFeature(feature_id);
        if (!f || f->isEnabled() == enabled)
            return;

        // Capability gate (阶段 3): a feature may declare it cannot be toggled at
        // runtime. Default descriptor → supports_runtime_disable=true, so untyped
        // features behave as before.
        if (!enabled && !f->descriptor_.supports_runtime_disable)
        {
            std::cerr << "[RenderScene] setFeatureEnabled(false) rejected: feature '"
                      << f->name() << "' declares supports_runtime_disable=false.\n";
            return;
        }

        // State machine + symmetric per-view-state management (closes PR-1's deferred
        // enable/disable case). The lifecycle_state_ transitions ARE the enable/disable
        // (isEnabled() derives from them — 三-1); per-view state is only touched for
        // features that declare they own it (creates_view_state).
        if (enabled)
        {
            f->lifecycle_state_ = FeatureState::Enabling;
            if (f->descriptor_.creates_view_state)
            {
                rebuildActiveViewCacheIfNeeded();
                std::vector<uint32_t> created;
                created.reserve(active_views_dense_.size());
                bool ok = true;
                for (auto* view : active_views_dense_)
                {
                    if (ensureFeatureViewState(*f, view->handle.index))
                        created.push_back(view->handle.index);
                    else { ok = false; break; }
                }
                if (!ok)
                {
                    // Transactional enable (三-3): roll back partial per-view state and
                    // revert to Disabled — no half-enabled feature with partial state.
                    for (uint32_t v : created)
                        releaseFeatureViewState(*f, v);
                    f->lifecycle_state_ = FeatureState::Disabled;
                    markFeatureCacheDirty();
                    graph_state_.valid = false;
                    return;
                }
            }
            f->lifecycle_state_ = FeatureState::Enabled;
        }
        else
        {
            f->lifecycle_state_ = FeatureState::Disabling;
            if (f->descriptor_.creates_view_state)
            {
                rebuildActiveViewCacheIfNeeded();
                for (auto* view : active_views_dense_)
                    releaseFeatureViewState(*f, view->handle.index);
            }
            f->lifecycle_state_ = FeatureState::Disabled;
        }

        markFeatureCacheDirty();
        graph_state_.valid = false; // trigger recompile next frame
    }

    void RenderScene::setFeatureEnabled(std::string_view feature_name, bool enabled)
    {
        // Route through the handle-based overload so the name path gets the SAME
        // capability gate (supports_runtime_disable) + FeatureState transitions +
        // per-view-state management. Previously it flipped enabled_ directly,
        // bypassing all three (三-1 single control path).
        for (auto &f : features_.values())
            if (f && f->name() == feature_name)
            {
                setFeatureEnabled(f->featureId(), enabled);
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
                f->featureId(),               // full handle (五-5)
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
    ViewHandle RenderScene::addView(const ViewCreateInfo &info)
    {
        auto view = std::make_unique<View>();
        view->current_extent = info.initial_extent;
        view->debug_name     = info.debug_name ? info.debug_name : "View";

        // SlotKeyAutoSparseSet::insert assigns a generational ViewHandle.
        ViewHandle handle = views_.insert(std::move(view));
        auto* new_view   = views_.at(handle).get();
        new_view->handle = handle;
        new_view->state  = ViewState::Active;

        // Allocate per-view UBO buffers + descriptor sets
        initViewUBO(*new_view);

        markViewCacheDirty();
        rebuildEnabledFeatureCacheIfNeeded();

        // Record per-view state for each enabled feature, via the truth source so
        // removeView()/removeFeature() release it symmetrically.
        for (auto* feat : enabled_features_dense_)
            ensureFeatureViewState(*feat, handle.index);

        return handle;
    }

    void RenderScene::removeView(ViewHandle handle)
    {
        if (!views_.contains(handle))
            return;

        auto &view = *views_.at(handle);
        // Idempotent (五-4): a view already being torn down lingers in the slot map
        // (state=Destroying) until endFrame() GC frees it fif frames later. A repeat
        // removeView in that window must NOT re-release feature state, re-notify the
        // scene registry, or re-enqueue another pending-destroy (the second record
        // would double-process the same handle at GC).
        if (view.state == ViewState::Destroying)
            return;

        // Release per-view state for EVERY feature that owns it — not just the
        // currently-enabled set. A feature disabled after this view was created
        // still holds state recorded at addView()/addFeature() time; iterating
        // enabled-only here used to leak it. releaseFeatureViewState() is a no-op
        // for a feature with no recorded state for this view.
        for (auto &feat : features_.values())
            if (feat)
                releaseFeatureViewState(*feat, handle.index);

        // Only ShadowResources subscribes to view-destroy (per-view cache
        // eviction), and it is per-scene now (Plan A) — so the scene registry
        // notify reaches it. The old global-registry notify is a dead no-op
        // (no global frame service overrides onViewDestroyed) and is dropped.
        scene_registry_.notifySceneViewDestroyed(scene_global_slot_.index, handle.index);

        // Do NOT call destroyViewUBO here — the view's ViewBuffer slot may still be
        // referenced by in-flight GPU commands for the other frames-in-flight slots.
        // Deferring to endFrame() GC ensures the GPU has finished all work before
        // the slot is freed.
        view.state = ViewState::Destroying;
        markViewCacheDirty();
        pending_destroys_.push_back({handle, 0});
    }

    View *RenderScene::getView(ViewHandle handle) noexcept
    {
        // tryGet rejects a stale handle (generation mismatch) as well as unknown.
        auto* ptr = views_.tryGet(handle);
        return ptr ? ptr->get() : nullptr;
    }

    const View *RenderScene::getView(ViewHandle handle) const noexcept
    {
        auto* ptr = views_.tryGet(handle);
        return ptr ? ptr->get() : nullptr;
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

        // ── Build + compile the NEW graph FIRST, into locals, WITHOUT touching the
        // currently-live graph or view resources (四: transactional / build-then-commit).
        // A layout with no color target yields no graph — a valid "no render" outcome.
        // A compile FAILURE returns early having mutated NOTHING, so the scene keeps
        // rendering with its last good graph instead of being left graph-less (the old
        // order retired the old graph up-front, so a failed recompile black-screened).
        std::unique_ptr<RGCompiledGraph> new_graph;
        RGResourceHandle                 new_color_handle{};

        if (layout.hasSlot(TargetSlot::SceneColor))
        {
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

            // Import every ADDITIONAL semantic slot the layout declares (beyond the
            // framework-special SceneColor / SceneDepth / ResolveColor) as an
            // externally-accessible slotted texture (阶段4 P4b). A feature writes one
            // via referenceTexture(targetSlotName(slot)); its physical image is
            // allocated by the OffscreenImagePool (already generalised over layout
            // slots) and injected per-frame through the binding — so the output is
            // readable-back / bindable, not a graph-internal transient. No-op for a
            // layout that declares only the primary slots (today's scenes).
            for (size_t si = 0; si < kTargetSlotCount; ++si)
            {
                const auto extra_slot = static_cast<TargetSlot>(si);
                if (extra_slot == TargetSlot::SceneColor ||
                    extra_slot == TargetSlot::SceneDepth ||
                    extra_slot == TargetSlot::ResolveColor)
                    continue;                      // primary slots handled above
                if (!layout.hasSlot(extra_slot))
                    continue;
                const auto& sd = layout.slot(extra_slot);
                RGImportedResourceInfo imp{};
                imp.slot             = extra_slot;
                imp.update_group     = static_cast<uint32_t>(RGUpdateGroup::GROUP_SWAPCHAIN);
                imp.initial_layout   = sd.initial_layout;
                imp.final_layout     = sd.final_layout;
                imp.preserve_content = sd.preserve_content;
                RGTextureDescription st = RGTextureDescription::Absolute(
                    1, 1, mapVkFormat(sd.format));
                (void)builder.importSlottedTexture(extra_slot, targetSlotName(extra_slot), st, imp);
            }

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
                // Commit NOTHING — old graph + view resources + last_layout stay intact
                // so the scene keeps rendering the last good graph. (四)
                return;
            }

            new_graph        = std::make_unique<RGCompiledGraph>(std::move(compiled));
            new_color_handle = backbuffer;
        }

        // ── Commit point ───────────────────────────────────────────────────────
        // Only now (new graph built, or a legit no-color layout) do we mutate
        // observable state: record the layout, retire the old graph (deferred destroy
        // after FIF frames) + invalidate the view resource states keyed on it so they
        // return to the transient pool instead of leaking VRAM (P1#37), then install
        // the new graph (null for a no-color layout).
        graph_state_.last_layout = layout;

        const RGGraphDescription* old_graph_desc = nullptr;
        if (graph_state_.graph)
        {
            old_graph_desc = &graph_state_.graph->original_graph;
            RetiredGraph retired;
            retired.graph = std::move(graph_state_.graph);
            retired.retire_frame = 0; // will be filled by endFrame tracking
            retired_graphs_.push_back(std::move(retired));
        }
        invalidateAllViewResources(old_graph_desc);

        graph_state_.graph              = std::move(new_graph);   // null for a no-color layout
        graph_state_.final_color_handle = new_color_handle;
        graph_state_.valid              = (graph_state_.graph != nullptr);

        // Optional graph dump for editor debugging — set LUX_DUMP_RG=1 in the
        // environment to write the compiled execution order + pass details to stderr.
        if (graph_state_.graph)
        {
            if (const char* dump = std::getenv("LUX_DUMP_RG"); dump && dump[0] != '0' && dump[0] != '\0')
            {
                std::cerr << "[RenderScene] '" << debug_name_ << "' graph (re)compiled:\n";
                printCompiledGraph(*graph_state_.graph, std::cerr);
            }
        }
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

        // Tear down FEATURES FIRST — before destroying views — so each feature's
        // per-(feature,view) state is released through the SAME path as explicit
        // removal. removeAllFeatures() → removeFeature() runs releaseFeatureViewState()
        // → deallocateViewState() for every owned (feature, view) pair, then detaches
        // and destroys each feature, all while views_, the scene registry and the
        // descriptor services are still alive (a feature's per-view eviction may still
        // reference them — so views must NOT be destroyed first). The previous order
        // (destroy views, then a bare onDetachFromScene loop that never touched
        // feature_view_states_) skipped deallocateViewState entirely on whole-scene
        // teardown — a per-view-state leak for features that create it (P0-2).
        removeAllFeatures();
        feature_view_states_.clear();
        enabled_features_dense_.clear();
        feature_cache_dirty_ = false;
        render_object_extractor_.clearProviders();

        // Now destroy all view UBO resources and per-view GPU state.
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
        // slotIndex() is already serial % fif, and the staging/transfer ring is
        // sized to the configured fif — so it indexes directly. (The old extra
        // "% kMaxFramesInFlight" was a redundant clamp: frame_slot < fif <= k made
        // it the identity. fif <= k is enforced at RenderContext construction.)
        const uint32_t frame_slot = stamp.slotIndex();

        // ── Transfer scheduler path ─────────────────────────────────────
        transfer_scheduler_.resetFrame(frame_slot);
        transfer_scheduler_.retireStaging(frame_slot);

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

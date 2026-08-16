#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/function/render/client/core/RenderFatal.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/gpu/transfer/TransferContributor.hpp>
#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/core/DescriptorSetLayoutContract.hpp>
#include <lux/engine/render/resources/SceneResources.hpp>
#include <lux/engine/render/gpu/descriptor/SceneDomainDescriptorSets.hpp>
#include <cassert>
// (mesh-stack + light resource includes removed — the mesh stack is owned by
//  StandardMeshStackFeature and light by LightFeature now; the core scene names
//  none of these domain types. "core stays domain-free".)
#include <lux/engine/render/graph/RGVulkanResourceAllocator.hpp>
#include <lux/engine/render/graph/RGVulkanRecorder.hpp>
#include <lux/engine/render/graph/RenderGraphCompiler.hpp>
#include <lux/engine/render/graph/RGCompiledGraph.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RGGraphResize.hpp>
#include <lux/engine/render/graph/RGDebugPrint.hpp>
#include <lux/engine/render/gpu/utils/FormatMap.hpp>
#include <lux/engine/render/renderer/FeatureTypeRegistry.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>

namespace lux::render
{
    Expected<void> RenderScene::rebaseSceneOrigin(
        const std::int64_t scene_origin_page[3]) noexcept
    {
        std::int64_t delta[3]{};
        for (std::size_t axis = 0u; axis < 3u; ++axis)
        {
            delta[axis] = scene_origin_page[axis] -
                config_.scene_origin_page[axis];
        }
        if (delta[0] == 0 && delta[1] == 0 && delta[2] == 0)
            return {};
        for (const auto* feature : features())
        {
            if (!feature->canRebaseSceneOrigin(delta))
            {
                return renderFailure<
                    err::scene::SpatialOriginRebaseRejected>();
            }
        }
        for (auto* feature : features())
            feature->rebaseSceneOrigin(delta);
        for (std::size_t axis = 0u; axis < 3u; ++axis)
            config_.scene_origin_page[axis] = scene_origin_page[axis];
        return {};
    }


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
        // Pool size = original baseline + the domain set's actual requirement.
        // The requirement is computed from the shape table (domainDescriptorCounts),
        // not padded by guesswork -- a single FEATURE domain set alone has 34 SSBOs
        // (including 16 vertex-pool slots), and that gets multiplied again by the
        // slice count; guessing would eventually undersize it and blow up in a
        // real scene.
        {
            SceneDescriptorArena::PoolSizeTemplate tmpl{};
            const uint32_t slices = std::max(1u, rctx.framesInFlight());
            for (const auto domain : SceneDomainDescriptorSets::kPerSceneDomains)
            {
                const auto c = domainDescriptorCounts(domain);
                tmpl.max_sets               += slices;
                tmpl.storage_buffer         += c.storage_buffer * slices;
                tmpl.combined_image_sampler += c.combined_image_sampler * slices;
                tmpl.uniform_buffer         += c.uniform_buffer * slices;
            }
            scene_descriptor_arena_.init(rctx.deviceContext().logicalDevice(), tmpl);
        }

        // Domain set instances (coexisting alongside the per-set ones during the
        // transition: build them first and have each owner dual-write into them,
        // verify the offsets and types line up, then switch the pipelines over).
        scene_domain_sets_ = std::make_unique<SceneDomainDescriptorSets>();
        if (!scene_domain_sets_->init(scene_descriptor_arena_,
                                      rctx.descriptorLayouts(),
                                      rctx.framesInFlight()))
        {
            // 域集是描述符的唯一写目标,也是管线唯一的绑定来源。分配失败之后本场景的
            // 所有资源描述符都不会被写入,而绑定一个从未写过的描述符集是未定义行为 ——
            // 继续跑下去只会让问题在离现场很远的地方以「有些东西不见了」的形式出现。
            //
            // 这里用 renderFatal 而不是 assert:assert 在 NDEBUG 下整条消失,而编辑器
            // 实机跑的正是带 NDEBUG 的 RelWithDebInfo —— 那等于在真正发布的配置里没有
            // 守卫。常见成因是描述符池尺寸不足或域布局缺失。
            renderFatal("RenderScene: 域描述符 set 分配失败(池尺寸不足或域布局缺失)");
        }

        // Register per-scene SceneResources in scene_registry_.
        {
            // 一次 emplace 拿到句柄,下面全程复用 —— 原先 emplace 之后又 find 两遍
            // (还判了一次空),同一个刚建出来的对象查三次。
            auto* sr = scene_registry_.emplace<SceneResources>().get();
            // 每帧维护由**安装点**登记 —— 资源自己不再继承帧接口。PostUpload:
            // 它要在其余资源之后推进(与改动前 kUploadPhase 声明的阶段一致)。
            scene_registry_.addBeginFrameHook(
                SceneResources::kUploadPhase,
                [sr](const FrameStamp& s) { sr->onFrameBeginMaintenance(s); });
            SceneResources::InitInfo si{
                .device_context = rctx.deviceContext(),
                .slices = rctx.framesInFlight(),
                .initial_scene_capacity = 8,
                .initial_view_capacity = 8,
                .arena = &scene_descriptor_arena_,
                .set_layout = rctx.descriptorLayouts().getLayout(EDescriptorSetSlot::Scene),
                // The Scene set lives in the GLOBAL domain. The offset comes from an
                // engine-level constant, sourced from the same place as the domain
                // layout -- a wrong value gets caught immediately by
                // vkUpdateDescriptorSets' dstBinding/type validation, without having
                // to wait until the pipeline switches over to notice.
                .domain_sets = scene_domain_sets_->setsFor(rdesc::EBindFrequency::GLOBAL),
                .domain_binding_offset =
                    engineSetDomainOffset(static_cast<uint32_t>(EDescriptorSetSlot::Scene)),
            };
            sr->init(si);
            sr->setDeferredQueue(&rctx.deferredDestroyQueue());
        }

        // (LightResources is NOT created here. It is owned by LightFeature, which
        // emplaces + inits it in initAndAttachTo when the scene opts in by adding
        // that feature — emplaced before ShadowResources so reverse-order shutdown
        // tears Shadow down first, ShadowResources holding a raw LightResources*.
        // A scene without LightFeature renders unlit. The core stays domain-free.)

        // (The standard 3D mesh-stack resources — InstanceResources /
        // VertexPoolRegistry / StaticVertexPoolSet / VertexProductionRegistry — are NOT
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
        scene_global_slot_ = scene_registry_.must<SceneResources>().allocateScene();

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
            transfer_scheduler_.contributors().add(
                makeTransferContributor(&scene_registry_.must<SceneResources>(), /*priority=*/10));

            // (LightResources transfer contributor is registered by LightFeature in
            // initAndAttachTo — the scene ctor no longer knows light. A scene without
            // LightFeature contributes no light SSBO flush.)
        }

        // Per-scene render-graph infrastructure
        graph_cache_ = std::make_unique<SceneGraphCache>(rctx, debug_name_);
        // 视图持有的图资源归它所有,释放要还给它。接在这里而不是 view_set_ 的构造
        // 参数里:本成员是 unique_ptr、在构造体内才建立(它的构造需要 debug_name_)。
        // 声明序保证了它活得比 view_set_ 久,所以 ~SceneViewSet() 能安全地用它。
        view_set_.setGraphCache(*graph_cache_);

        feature_set_.markCacheDirty();


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

    bool RenderScene::ensureFeatureViewState(SceneFeature& feature, uint32_t view_index)
    {
        // 记账在 SceneFeatureSet(纯数据),**调虚函数留在这里** —— allocateViewState
        // 的签名要 RenderScene&。只有登记成功才记账,保证 deallocateViewState 与之
        // 恰好一一对应。
        const uint32_t f_index = feature.featureId().index;
        if (feature_set_.ownsViewState(f_index, view_index))
            return true;
        if (!feature.allocateViewState(view_index, *this))
            return false;
        feature_set_.recordViewState(f_index, view_index);
        return true;
    }

    void RenderScene::releaseFeatureViewState(SceneFeature& feature, uint32_t view_index) noexcept
    {
        const uint32_t f_index = feature.featureId().index;
        if (!feature_set_.ownsViewState(f_index, view_index))
            return;
        feature.deallocateViewState(view_index);
        feature_set_.forgetViewState(f_index, view_index);
    }

    Expected<void> RenderScene::beginInstall(SceneFeature& feature)
    {
        // Unified install entry (3-2): dependency / conflict / multiplicity validation
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
            if (auto declared = validateDeclaredRelationships(*desc); !declared)
                return declared;
        }

        feature.scene_ = this;
        // Apply the type-level descriptor BEFORE attach so a feature can observe its
        // own descriptor during initAndAttachTo.
        if (desc && desc->valid())
            feature.descriptor_ = *desc;
        feature.lifecycle_state_ = FeatureState::Attaching;

        // attach 失败:此时还没有登记/插入任何东西,直接丢掉这个半装的特性 ——
        // 它的析构函数会释放自己在半程 attach 里分配的一切。错误原样上交,
        // 调用方(comm 的 AddFeature 回执 / createScene 的 feature_errors)
        // 拿到的是特性自己报的那一条,不是"装失败了"。
        return feature.initAndAttachTo(*this);
    }

    /// beginInstall 的第一段:只看 FeatureDescriptor 声明的**特性之间的关系**,
    /// 不碰实例、不碰 GPU。四道闸全部在 attach 之前,所以拒绝时没有任何东西要回滚。
    Expected<void> RenderScene::validateDeclaredRelationships(const FeatureDescriptor& desc) const
    {
        const std::uint32_t self = encodeFeatureType(desc.type);

        if (desc.multiplicity == FeatureMultiplicity::SinglePerScene && feature_set_.hasType(desc.type))
            return renderFailure<err::feature::AlreadyInstalled>(self);

        for (FeatureTypeId conflict : desc.conflicts)
            if (feature_set_.hasType(conflict))
                return renderFailure<err::feature::ConflictsWithInstalled>(
                    self, encodeFeatureType(conflict));

        for (const FeatureDependency& dep : desc.dependencies)
            if (!dep.optional && !feature_set_.hasType(dep.type))
                return renderFailure<err::feature::DependencyMissing>(
                    self, encodeFeatureType(dep.type));

        // EFeatureLevel negotiation: a feature declaring level_profiles must carry a
        // row for the resolved session tier AND the device must have enabled that
        // row's whitelisted features. An empty span = "any tier, no whitelist needs".
        if (desc.level_profiles.empty())
            return {};

        // deviceContext() 只有非 const 重载,而本函数只读 caps/featureLevel;
        // render_ctx_ 是 shared_ptr,常量性到指针为止。
        const auto&        device  = render_ctx_->deviceContext();
        const EFeatureLevel session = device.featureLevel();

        const auto row = std::ranges::find(desc.level_profiles, session, &FeatureLevelProfile::level);
        if (row == desc.level_profiles.end())
            return renderFailure<err::feature::LevelProfileMissing>(
                self, static_cast<std::uint32_t>(session));

        // 带的是**缺了哪些位**,不是「这一行要求了哪些位」—— 后者要消费方自己
        // 再跟 caps 比一遍才知道问题在哪。位到名字的反查见 deviceFeatureName。
        if (const std::uint32_t missing = unmetDeviceFeatures(device.caps(), row->required_features);
            missing != 0)
            return renderFailure<err::feature::LevelRequirementsUnmet>(self, missing);

        return {};
    }

    Expected<FeatureHandle> RenderScene::finishInstall(SceneFeature& feature, FeatureHandle handle)
    {
        // 容器已在 installFeature 里插好(那里才有具体类型,好让它自行判定"产不产
        // pass")。这里回填私有 id —— 本类是 SceneFeature 的 friend。
        auto* raw = &feature;
        raw->feature_id_ = handle;

        // A feature is added in the ENABLED state — there is no "add disabled" path
        // today. FeatureState is the single source of truth for enabled-ness (3-1),
        // so we land it here; isEnabled() derives from it.
        raw->lifecycle_state_ = FeatureState::Enabled;

        // Enabled-state initialisation for an already-live scene.
        if (initialized_)
        {
            // Back-fill per-view state for views that ALREADY exist: a feature added
            // to a live scene must observe current views, mirroring addView()'s allocate.
            // TRANSACTIONAL (3-3): if any view's allocateViewState() fails, roll back the
            // per-view state already created for this feature and ABORT the install — the
            // caller observes NO half-attached feature (returns an invalid handle). Today
            // no built-in feature fails allocateViewState, so this is a strong-guarantee
            // safety net rather than a behaviour change.
            std::vector<uint32_t> created;
            created.reserve(view_set_.active().size());

            const auto views = view_set_.active();
            const auto failed = std::ranges::find_if_not(views, [&](const auto* view) {
                if (!ensureFeatureViewState(*raw, view->handle.index))
                    return false;
                created.push_back(view->handle.index);
                return true;
            });

            if (failed != views.end())
            {
                for (uint32_t v : created)
                    releaseFeatureViewState(*raw, v);
                feature_set_.forgetAllViewState(raw->feature_id_.index);
                raw->lifecycle_state_ = FeatureState::Failed;
                raw->onDetachFromScene(*this);
                feature_set_.erase(raw->feature_id_);
                feature_set_.markCacheDirty();
                graph_cache_->invalidate(
                    EGraphInvalidationReason::FEATURE_TOPOLOGY);
                return renderFailure<err::memory::CapacityExhausted>();
            }
        }

        feature_set_.markCacheDirty();
        // Adding a feature changes the pass set; force graph recompile on next render.
        graph_cache_->invalidate(
            EGraphInvalidationReason::FEATURE_TOPOLOGY);

        if (feature_type_registry_ != nullptr)
            feature_type_registry_->noteInstanceAdded(raw->typeId());

        return raw->feature_id_;
    }

    Expected<FeatureHandle> RenderScene::addFeatureErased(
        std::unique_ptr<RenderFeature> feature)
    {
        // 插件路径的入参本就是 RenderFeature(见 FeatureRegistration.hpp:
        // "T must derive from RenderFeature"),类型信息不必再猜。
        return installFeature(std::move(feature));
    }

    SceneFeature *RenderScene::getFeature(FeatureHandle feature_id) const
    {
        // get() 内部用 tryGet,同时挡掉未知句柄与陈旧句柄(代数不匹配)。
        return feature_set_.get(feature_id);
    }

    void RenderScene::setFeatureDescriptor(FeatureHandle feature_id, const FeatureDescriptor& descriptor) noexcept
    {
        if (auto* f = getFeature(feature_id))
            f->descriptor_ = descriptor;   // RenderScene is a friend of SceneFeature
    }

    bool RenderScene::hasFeatureOfType(FeatureTypeId type) const noexcept
    {
        return feature_set_.hasType(type);
    }

    Expected<void> RenderScene::removeFeature(FeatureHandle feature_id)
    {
        // Public removal enforces reverse-dependency protection (3-4).
        return removeFeatureInternal(feature_id, /*check_reverse_deps=*/true);
    }

    Expected<void> RenderScene::removeFeatureInternal(FeatureHandle feature_id, bool check_reverse_deps)
    {
        auto* ptr = feature_set_.get(feature_id);
        if (ptr == nullptr)
            return renderFailure<err::feature::HandleStale>(feature_id.index, feature_id.gen);

        // Reverse-dependency guard (3-4): refuse to remove a feature that another
        // INSTALLED feature still REQUIRES (a non-optional dependency on this type).
        // Default policy is reject — the caller must remove the dependent first —
        // which also prevents the "provider gone, consumer dangling" hazard (e.g.
        // removing Light out from under ShadowMap). Skipped for whole-scene teardown
        // (removeAllFeatures passes check=false): there every feature is going away,
        // so the guard must not block its own bulk removal. Untyped features
        // (kInvalidFeatureTypeId) are never depended upon, so they bypass the scan.
        if (check_reverse_deps)
        {
            if (const SceneFeature* dependent =
                    feature_set_.firstRequiring(ptr->descriptor_.type, ptr))
                return renderFailure<err::feature::StillRequiredByAnother>(
                    encodeFeatureType(ptr->descriptor_.type),
                    encodeFeatureType(dependent->descriptor_.type));
        }

        ptr->lifecycle_state_ = FeatureState::Detaching;

        // Tear down per-view state this feature allocated BEFORE detaching it.
        // removeView() only fires deallocateViewState() on view removal, never on
        // feature removal — so without this a removed feature's per-view eviction
        // hook never runs (leak; e.g. StandardViewCamera's ViewCameraResource
        // entries). viewsOwnedBy 只快照不改账本 —— releaseFeatureViewState 的守卫要读账本,
        // releaseFeatureViewState 会改动该集合。
        for (uint32_t view_id : feature_set_.viewsOwnedBy(feature_id.index))
            releaseFeatureViewState(*ptr, view_id);
        feature_set_.forgetAllViewState(feature_id.index);

        const FeatureTypeId removed_type = ptr->typeId();
        ptr->onDetachFromScene(*this);
        // Erase (destroying the feature) while scene_ is still valid,
        // so the destructor's destroy() → renderContext() path works safely.
        feature_set_.erase(feature_id);
        if (feature_type_registry_ != nullptr)
            feature_type_registry_->noteInstanceRemoved(removed_type);
        feature_set_.markCacheDirty();
        graph_cache_->invalidate(
            EGraphInvalidationReason::FEATURE_TOPOLOGY);
        return {};
    }

    void RenderScene::removeAllFeatures()
    {
        // Collect ids first since erase during iteration may invalidate
        std::vector<FeatureHandle> ids;
        ids.reserve(feature_set_.size());
        for (auto *f : feature_set_.all())
            if (f)
                ids.push_back(f->feature_id_);

        // Whole-scene teardown: bypass the reverse-dependency guard — every feature
        // is going away, so a dependent still being present must not block removal.
        // 逐个的失败在这里无处可去,也无从处置:场景正在整体拆除,剩下的特性照拆。
        for (auto id : ids)
            (void)removeFeatureInternal(id, /*check_reverse_deps=*/false);
    }

    uint32_t RenderScene::requiredTargetSlotMask() const noexcept
    {
        uint32_t mask = 0;
        for (auto* f : feature_set_.all())
            if (f)
                mask |= f->requiredTargetSlots();
        return mask;
    }

    Expected<void> RenderScene::setFeatureEnabled(FeatureHandle feature_id, bool enabled)
    {
        auto *f = getFeature(feature_id);
        if (f == nullptr)
            return renderFailure<err::feature::HandleStale>(feature_id.index, feature_id.gen);
        if (f->isEnabled() == enabled)
            return {};

        // Capability gate: a feature may declare it cannot be toggled at
        // runtime. Default descriptor → supports_runtime_disable=true, so untyped
        // features behave as before.
        if (!enabled && !f->descriptor_.supports_runtime_disable)
            return renderFailure<err::feature::RuntimeDisableUnsupported>(
                encodeFeatureType(f->descriptor_.type));

        // State machine + symmetric per-view-state management (closes PR-1's deferred
        // enable/disable case). The lifecycle_state_ transitions ARE the enable/disable
        // (isEnabled() derives from them -- 3-1); per-view state is only touched for
        // features that declare they own it (creates_view_state).
        if (enabled)
        {
            f->lifecycle_state_ = FeatureState::Enabling;
            if (f->descriptor_.creates_view_state)
            {
                std::vector<uint32_t> created;
                created.reserve(view_set_.active().size());

                const auto views = view_set_.active();
                const auto failed = std::ranges::find_if_not(views, [&](const auto* view) {
                    if (!ensureFeatureViewState(*f, view->handle.index))
                        return false;
                    created.push_back(view->handle.index);
                    return true;
                });

                if (failed != views.end())
                {
                    // Transactional enable (3-3): roll back partial per-view state and
                    // revert to Disabled — no half-enabled feature with partial state.
                    for (uint32_t v : created)
                        releaseFeatureViewState(*f, v);
                    f->lifecycle_state_ = FeatureState::Disabled;
                    feature_set_.markCacheDirty();
                    graph_cache_->invalidate(
                        EGraphInvalidationReason::FEATURE_TOPOLOGY);
                    return renderFailure<err::memory::CapacityExhausted>();
                }
            }
            f->lifecycle_state_ = FeatureState::Enabled;
        }
        else
        {
            f->lifecycle_state_ = FeatureState::Disabling;
            if (f->descriptor_.creates_view_state)
            {
                for (auto* view : view_set_.active())
                    releaseFeatureViewState(*f, view->handle.index);
            }
            f->lifecycle_state_ = FeatureState::Disabled;
        }

        feature_set_.markCacheDirty();
        graph_cache_->invalidate(
            EGraphInvalidationReason::FEATURE_TOPOLOGY);
        return {};
    }

    Expected<void> RenderScene::setFeatureEnabled(std::string_view feature_name, bool enabled)
    {
        // Route through the handle-based overload so the name path gets the SAME
        // capability gate (supports_runtime_disable) + FeatureState transitions +
        // per-view-state management.
        for (auto *f : feature_set_.all())
            if (f && f->name() == feature_name)
                return setFeatureEnabled(f->featureId(), enabled);

        // 名字对不上任何已装特性 —— 与句柄失效同一件事:调用方指的东西不在场景里。
        return renderFailure<err::feature::HandleStale>(0u, 0u);
    }

    std::vector<RenderScene::FeatureParamDesc> RenderScene::queryFeatureParamDescs() const
    {
        return feature_set_.queryParamDescs();
    }

    // (setRenderPath 已删:零调用点,且它的头注释长期宣称"removes all features,
    //  re-adds ..."——实际只设枚举 + 置图失效,注释与实现互相矛盾多轮而无人踩到,
    //  正因为没有任何调用者。真要切管线路径走 feature 增删,不是翻一个枚举。)

    // ─────────────────────────────────────────────────────────────────────
    //  View Management
    // ─────────────────────────────────────────────────────────────────────
    ViewHandle RenderScene::addView(const ViewCreateInfo &info)
    {
        // 视图容器 + 每视图 GPU 槽由 SceneViewSet 负责;特性侧的每视图状态由场景
        // 编排 —— SceneFeature::allocateViewState 的签名要 RenderScene&,让视图集合
        // 反手持有场景引用会造出一条反向依赖,不值当。
        const ViewHandle handle = view_set_.add(info);

        // 经真相源记录每个已启用特性的每视图状态,好让 removeView()/removeFeature()
        // 对称释放。
        for (auto* feat : feature_set_.enabled())
            ensureFeatureViewState(*feat, handle.index);

        return handle;
    }

    bool RenderScene::removeView(ViewHandle handle)
    {
        // 幂等守卫必须最先:已在销毁中的视图会在槽表里逗留到 GC 释放,这个窗口内的
        // 重复调用不得重复释放特性状态、重复通知注册表、或再入队一条待销毁记录。
        if (!view_set_.isRemovable(handle))
            return false;

        // 释放**每一个**拥有该视图状态的特性 —— 不只是当前启用的那些。视图创建后被
        // 禁用的特性仍持有当时登记的状态;只遍历启用集会漏掉它们(旧实现的泄漏点)。
        // releaseFeatureViewState 对没有登记的特性是 no-op。
        for (auto *feat : feature_set_.all())
            if (feat)
                releaseFeatureViewState(*feat, handle.index);

        // 只有 ShadowResources 订阅视图销毁(每视图缓存驱逐),而它现在是每场景的
        // (Plan A),所以场景注册表的通知能到达它。旧的全局注册表通知是死的 no-op
        // (没有任何全局帧服务重写 onViewDestroyed),已删。
        scene_registry_.notifySceneViewDestroyed(scene_global_slot_.index, handle.index);

        // 此处**不**释放 GPU 槽:其它 frames-in-flight 槽位的在飞命令可能仍在引用,
        // 必须等 endFrame 的 GC 水位越过。
        view_set_.markDestroying(handle);
        return true;
    }

    View *RenderScene::getView(ViewHandle handle) noexcept
    {
        return view_set_.get(handle);
    }

    const View *RenderScene::getView(ViewHandle handle) const noexcept
    {
        return view_set_.get(handle);
    }

    void RenderScene::endViewFrame(uint32_t /*frame_slot*/)
    {
        view_set_.endViewFrame();
        // 待销毁视图的 GC 在 endFrame() 里按栅栏证实的完成水位进行。
    }

    // ─────────────────────────────────────────────────────────────────────
    //  Full Initialisation
    // ─────────────────────────────────────────────────────────────────────

    // (init() removed — RAII constructor handles all initialisation)

    // ─────────────────────────────────────────────────────────────────────
    //  Infra Accessors
    // ─────────────────────────────────────────────────────────────────────

    RGVulkanResourceAllocator &RenderScene::graphAllocator() noexcept
    {
        return graph_cache_->allocator();
    }

    RGVulkanRecorder &RenderScene::graphRecorder() noexcept
    {
        return graph_cache_->recorder();
    }

    SceneGraphState &RenderScene::graphState() noexcept
    {
        return graph_cache_->state();
    }

    const SceneGraphState &RenderScene::graphState() const noexcept
    {
        return graph_cache_->state();
    }

    void RenderScene::invalidateGraph(
        EGraphInvalidationReason reason) noexcept
    {
        graph_cache_->invalidate(reason);
    }

    void RenderScene::compileGraphTemplate(const RenderTargetLayout &layout)
    {
        // 实现已迁入 SceneGraphCache::compile(事务式 build-then-commit)。这里只做
        // 一件本层该做的事:把**场景拥有的东西**(已启用特性、全部视图、域描述符集)
        // 交给图缓存。特性与视图的所有权始终在场景,图缓存只借用一次。

        const std::span<View* const> all_views = view_set_.all();
        // 图缓存不认识特性:它只收"把 pass 声明进这个 builder"这一个操作。
        // 只拥有资源的特性其槽里没有该操作,自然不参与。
        graph_cache_->compile(layout,
                              requiredTargetSlotMask(),
                              [this](RGBuilder& b)
                              {
                                  feature_set_.contributePasses(b);
                              },
                              all_views,
                              scene_domain_sets_.get(),
                              current_stamp_.serial);
    }

    void RenderScene::retireViewResourceState(RGResourceState&& state, const RGGraphDescription* source_graph)
    {
        graph_cache_->retireViewResourceState(std::move(state), source_graph);
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

    std::span<SceneFeature* const> RenderScene::enabledFeatures() const noexcept
    {
        return feature_set_.enabled();
    }

    std::span<SceneFeature* const> RenderScene::features() const noexcept
    {
        return feature_set_.all();
    }

    // ─────────────────────────────────────────────────────────────────────
    //  Per-Frame Lifecycle
    // ─────────────────────────────────────────────────────────────────────

    void RenderScene::dumpCompiledGraph(std::ostream& os) const
    {
        graph_cache_->dump(os);
    }

    void RenderScene::dumpGpuTiming(std::ostream& os) const
    {
        auto write_json_string = [&os](std::string_view value)
        {
            os << '"';
            for (const char c : value)
            {
                switch (c)
                {
                case '"': os << "\\\""; break;
                case '\\': os << "\\\\"; break;
                case '\n': os << "\\n"; break;
                case '\r': os << "\\r"; break;
                case '\t': os << "\\t"; break;
                default: os << c; break;
                }
            }
            os << '"';
        };

        const auto& graph_telemetry = graph_cache_->telemetry();
        os << "{\"version\":2,\"graph\":{\"pending_invalidation_bits\":"
           << graph_telemetry.pending_invalidation_bits
           << ",\"compile_attempts\":" << graph_telemetry.compile_attempts
           << ",\"compile_successes\":" << graph_telemetry.compile_successes
           << ",\"compile_failures\":" << graph_telemetry.compile_failures
           << ",\"build_ns\":" << graph_telemetry.build_nanoseconds
           << ",\"compile_ns\":" << graph_telemetry.compile_nanoseconds
           << ",\"total_ns\":" << graph_telemetry.total_nanoseconds
           << ",\"retired_graph_high_water\":"
           << graph_telemetry.retired_graph_high_water
           << ",\"retired_view_resource_high_water\":"
           << graph_telemetry.retired_view_resource_high_water
           << ",\"invalidation_counts\":[";
        for (std::size_t index = 0u;
             index < graph_telemetry.invalidation_counts.size(); ++index)
        {
            if (index != 0u)
                os << ',';
            os << graph_telemetry.invalidation_counts[index];
        }
        os << "],\"compile_samples\":[";
        bool first_compile = true;
        for (const auto& sample : graph_cache_->compileHistory())
        {
            if (!first_compile)
                os << ',';
            first_compile = false;
            os << "{\"frame\":" << sample.frame_serial
               << ",\"invalidation_bits\":" << sample.invalidation_bits
               << ",\"build_ns\":" << sample.build_nanoseconds
               << ",\"compile_ns\":" << sample.compile_nanoseconds
               << ",\"total_ns\":" << sample.total_nanoseconds
               << ",\"succeeded\":"
               << (sample.succeeded ? "true" : "false") << '}';
        }
        const auto& pipeline_telemetry =
            render_ctx_->pipelineManager().telemetry();
        os << "]},\"pipeline\":{\"graphics_cache_hits\":"
           << pipeline_telemetry.graphics_cache_hits
           << ",\"graphics_cache_misses\":"
           << pipeline_telemetry.graphics_cache_misses
           << ",\"graphics_create_failures\":"
           << pipeline_telemetry.graphics_create_failures
           << ",\"graphics_create_ns\":"
           << pipeline_telemetry.graphics_create_nanoseconds
           << ",\"graphics_create_max_ns\":"
           << pipeline_telemetry.graphics_create_max_nanoseconds
           << ",\"compute_create_calls\":"
           << pipeline_telemetry.compute_create_calls
           << ",\"compute_create_failures\":"
           << pipeline_telemetry.compute_create_failures
           << ",\"compute_create_ns\":"
           << pipeline_telemetry.compute_create_nanoseconds
           << ",\"compute_create_max_ns\":"
           << pipeline_telemetry.compute_create_max_nanoseconds
           << ",\"render_pass_cache_hits\":"
           << pipeline_telemetry.render_pass_cache_hits
           << ",\"render_pass_cache_misses\":"
           << pipeline_telemetry.render_pass_cache_misses
           << "},\"views\":[";
        bool first_view = true;
        for (const auto* view : view_set_.active())
        {
            if (!view || !view->resource_state)
                continue;
            const auto& history =
                view->resource_state->record_ctx.gpu_timing_history;
            if (history.empty())
                continue;

            if (!first_view)
                os << ',';
            first_view = false;
            os << "{\"view\":" << view->handle.index
               << ",\"samples\":[";
            bool first_sample = true;
            for (const auto& snapshot : history)
            {
                if (!snapshot)
                    continue;
                if (!first_sample)
                    os << ',';
                first_sample = false;
                os << "{\"frame\":" << snapshot->frame_id
                   << ",\"available\":"
                   << (snapshot->available ? "true" : "false")
                   << ",\"total_ms\":" << snapshot->total_milliseconds
                   << ",\"passes\":[";
                bool first_pass = true;
                for (const auto& pass : snapshot->passes)
                {
                    if (!first_pass)
                        os << ',';
                    first_pass = false;
                    os << "{\"name\":";
                    write_json_string(pass.name);
                    os << ",\"ms\":" << pass.milliseconds << '}';
                }
                os << "]}";
            }
            os << "]}";
        }
        os << "]}";
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

        // 每帧维护回调由**安装点**登记(见各资源的安装处),这里只按阶段驱动。
        // 顺序 = 登记顺序 = 原先的注册顺序。
        for (uint32_t phase = 0; phase < static_cast<uint32_t>(EUploadPhase::Count); ++phase)
        {
            for (const auto& hook : scene_registry_.beginFrameHooks(static_cast<EUploadPhase>(phase)))
                hook(stamp);
        }

        // Write scene-global + per-view data into SceneResources after it has
        // advanced to this slot AND the slot fence has been waited (we are inside
        // FrameDriver::beginFrame's caller). updateView() only caches the data
        // (it runs pre-fence-wait during the drain); the actual mapped-SSBO write
        // for the current slot happens HERE so it never races the in-flight frame.
        {
            auto& scene_res = scene_registry_.must<SceneResources>();
            SceneGlobalGpuData sg{};
            sg.time_sec = scene_time_;
            sg.delta_time = scene_delta_time_;
            sg.frame_number = static_cast<uint32_t>(scene_total_frames_);
            sg.pad0 = 0.f;
            scene_res.writeSceneGlobal(scene_global_slot_, sg);

            for (auto *view : view_set_.active())
            {
                if (!view || !view->has_view_data)
                    continue;
                // Upload the neutral per-view bytes the scene staged in updateView
                // (the core does not interpret them; #27: write happens here, after
                // the fence wait, never during the request drain).
                scene_res.writeViewData(view->view_slot,
                    view->view_data_staging.data(), kViewDataStrideBytes, frame_slot);
            }
        }

        // (Large-world cell streaming is no longer driven here. It lives entirely in
        // SpatialCullFeature::onFrameBegin — which runs in the loop below — and feeds
        // its mask address through the domain-neutral instance_cull_mask_addr_ set
        // above. The general scene knows nothing about SpatialCullGrid.)

        graph_cache_->allocator().garbageCollect(stamp.serial, render_ctx_->framesInFlight() + 1);

        const FeatureFrameContext frame_ctx{
            frame_slot
        };
        for (auto* feature : feature_set_.enabled())
            feature->onFrameBegin(frame_ctx);
    }

    // RenderScene::updateView was REMOVED (the View type is no longer 3D-specific). Per-view camera data is a
    // feature domain now: StandardViewCamera's op handler fills ViewCameraResource +
    // the View's neutral GPU staging (view_data_staging / frustum_staging) directly,
    // CPU-side during the op dispatch; beginFrame uploads the staging after the slot
    // fence wait (#27). The core scene no longer names camera matrices / frustum and
    // no longer broadcasts onFrustumUpdated to every feature.

    void RenderScene::endFrame(uint64_t frame_id, uint64_t completed_serial)
    {
        // 图相关的两条退休 FIFO(视图资源 / 编译图)由图缓存回收 —— 其内部保证
        // "先视图资源、后图"的顺序(source_graph 指向那些图)。
        graph_cache_->collectRetired(frame_id, completed_serial);

        // 第三条延迟销毁 FIFO(待销毁视图)由视图集合回收,水位口径与上面两条一致。
        view_set_.collectDestroyed(frame_id, completed_serial);

        // (此处曾每帧完整遍历一遍场景服务链去调 onEndFrame —— 而**零个**资源
        //  重写过它。接口与遍历一并删除。)

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

    // (initViewUBO / destroyViewUBO / endViewFrame(View&) 已迁入 SceneViewSet ——
    //  每视图 GPU 槽的分配与释放属视图生命周期。)

    // (RenderScene::resizeView 已消亡:视图不再自持尺寸账本——
    //  current_extent 由渲染期从 target binding 派生;改尺寸走
    //  ResizeTarget 直达图像池。View 瘦身。)

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
        // teardown — a per-view-state leak for features that create it.
        removeAllFeatures();
        feature_set_.clearLedger();
        feature_set_.clear();

        // 视图集合自行清理:释放每个视图的图资源与 GPU 槽,再清空容器。
        // 必须在图缓存 shutdown 之前 —— 释放图资源要用它的 recorder/allocator。
        view_set_.shutdown();

        // 图缓存自行按序清理:退休图 -> 退休视图资源(需 recorder/allocator 尚在)
        // -> 当前图 -> recorder -> allocator。
        graph_cache_->shutdown();

        // Drop pending retirement callbacks owned by this scene before
        // scene-local resources are destroyed.
        render_ctx_->retireScheduler().purge(retire_owner_token_);

        // Free the scene-global slot, then shutdown per-scene resources.
        scene_registry_.must<SceneResources>().freeScene(scene_global_slot_);

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

} // namespace lux::render

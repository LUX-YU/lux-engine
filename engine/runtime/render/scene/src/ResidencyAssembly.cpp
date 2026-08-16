// 驻留 T11:三件套装配唯一实现。形状即 render_resource_manager_test §11
// 的接线 lambda(那里注明「即 T11 宿主参考形状」——现在收编成产品代码)。

#include <lux/engine/runtime/render/scene/ResidencyAssembly.hpp>
#include <lux/engine/runtime/render/scene/ResidencyCloseSender.hpp>

#include <lux/engine/runtime/render/scene/detail/residency/RenderResourceOps.hpp>
#include <lux/engine/runtime/render/scene/detail/residency/subservices/TextureSubservice.hpp>
#include <lux/engine/runtime/render/scene/detail/residency/subservices/MeshSubservice.hpp>
#include <lux/engine/runtime/render/scene/detail/residency/subservices/MaterialSubservices.hpp>

#include <lux/engine/runtime/assets/AssetLoadService.hpp>
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncScope.hpp>
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/RenderUploadClient.hpp>
#include <lux/engine/function/render/client/core/RenderFatal.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace lux::runtime
{
    class ResidencyAssembly::Impl final
    {
    public:
        using FailureSink = ResidencyAssembly::FailureSink;
        using AssetEventCallbacks = ResidencyAssembly::AssetEventCallbacks;

        class CallbackControl;

        Impl(lux::render::RenderControlSession& control,
             lux::render::RenderUploadClient upload,
             lux::asset::AssetManager&          assets,
             const lux::render::FeatureCatalog& catalog,
             lux::asset_runtime::AssetClient    asset_client,
             lux::exec::AsyncRuntime&           async,
             FailureSink                        failure_sink,
             TextureStreamingBudget texture_streaming);
        ~Impl();

        void onUnreferenced(const lux::asset::asset_id_t& id);
        void onInvalidated(const lux::asset::asset_id_t& id);
        void onContentChanged(const lux::asset::asset_id_t& id);
        void onAssetRegistered(const lux::asset::asset_id_t& id);

        [[nodiscard]] AssetEventCallbacks makeAssetEventCallbacks();
        [[nodiscard]] lux::ecs::ResidencyCallbacks makeCallbacks();
        void request(const lux::asset::asset_id_t& id,
                     lux::ecs::EResourceDomain     domain);
        void tickTextureStreaming();
        [[nodiscard]] std::uint64_t peekReadyBits(
            const lux::asset::asset_id_t& id) const;
        [[nodiscard]] bool hasInflight() const;
        [[nodiscard]] ResidencyCloseSnapshot closeSnapshot() const noexcept;
        void teardown();
        [[nodiscard]] ResidencyCloseReport advanceClose() noexcept;
        void subscribeClose(
            lux::cxx::move_only_function<void(ResidencyCloseReport)>
                completion) noexcept;
        void subscribeCloseOnMain(
            lux::cxx::move_only_function<void(ResidencyCloseReport)>
                completion) noexcept;
        void finishClose(ResidencyCloseReport report) noexcept;

    private:
        MaterialArtifactStore       material_store_;
        RenderResourceService       service_;
        RenderResourceStateManager  table_;
        TextureSubservice*          texture_subservice_{nullptr};
        MaterialSubservice*         material_subservice_{nullptr};
        lux::render::RenderControlSession* control_{nullptr};
        lux::render::RenderUploadClient upload_;
        lux::exec::AsyncRuntime*    async_{nullptr};
        std::unique_ptr<render_resource::Context> ctx_;
        std::unique_ptr<lux::exec::AsyncScope> residency_tasks_;
        std::uint32_t active_call_depth_{0};
        bool closing_{false};
        bool scope_close_started_{false};
        bool scope_closed_{false};
        bool closed_{false};
        std::vector<lux::cxx::move_only_function<void(ResidencyCloseReport)>>
            close_waiters_;
        std::shared_ptr<CallbackControl> callback_control_;
        lux::render::RenderRequest<lux::render::TextureMipDemandsReply>
            mip_demand_query_;
        TextureStreamingBudget texture_streaming_{};
        std::uint32_t mip_poll_counter_{0u};
    };

    class ResidencyAssembly::Impl::CallbackControl final
    {
    public:
        explicit CallbackControl(Impl& owner) noexcept
            : owner_(&owner)
        {}

        [[nodiscard]] Impl* tryOwnerForAdmission() const noexcept
        {
            return owner_ != nullptr && owner_->ctx_
                       && owner_->ctx_->acceptsNewOperations()
                       ? owner_
                       : nullptr;
        }

        [[nodiscard]] Impl* tryOwnerForCompletion() const noexcept
        {
            return owner_;
        }

        void invalidate() noexcept { owner_ = nullptr; }

    private:
        /// Main-thread-confined observer. The control block, not this pointer,
        /// is what escapes; invalidate() runs before owner dependencies die.
        Impl* owner_{nullptr};
    };

    namespace
    {
        /// Main-thread recursion fence. Residency callbacks deliberately run
        /// inline, so a user handler may re-enter another assembly entry before
        /// the outer table/service operation has unwound.
        class ActiveCallGuard final
        {
        public:
            explicit ActiveCallGuard(std::uint32_t& depth) noexcept
                : depth_(depth)
            {
                ++depth_;
            }

            ~ActiveCallGuard() noexcept { --depth_; }

            ActiveCallGuard(const ActiveCallGuard&) = delete;
            ActiveCallGuard& operator=(const ActiveCallGuard&) = delete;

        private:
            std::uint32_t& depth_;
        };

        /// 服务票据 → 不透明 RAII(析构=退订;ecs 不认识引擎层类型)。
        template <class Ticket>
        lux::ecs::ResidencyCallbacks::Ticket wrapTicket(Ticket&& t)
        {
            using T = std::decay_t<Ticket>;
            return lux::ecs::ResidencyCallbacks::Ticket(
                [owned = T(std::move(t))]() mutable noexcept
                { owned.reset(); });
        }

    } // namespace

    ResidencyAssembly::Impl::Impl(
        lux::render::RenderControlSession& control,
        lux::render::RenderUploadClient upload,
        lux::asset::AssetManager&          assets,
        const lux::render::FeatureCatalog& catalog,
        lux::asset_runtime::AssetClient    asset_client,
        lux::exec::AsyncRuntime&           async,
        FailureSink                        failure_sink,
        TextureStreamingBudget texture_streaming)
        : control_(&control)
        , upload_(std::move(upload))
        , async_(&async)
        , residency_tasks_(std::make_unique<lux::exec::AsyncScope>(async))
        , callback_control_(std::make_shared<CallbackControl>(*this))
        , texture_streaming_(texture_streaming)
    {
        texture_streaming_.query_interval_frames = std::max(
            1u, texture_streaming_.query_interval_frames);
        texture_streaming_.maximum_demand_entries = std::clamp(
            texture_streaming_.maximum_demand_entries,
            1u,
            lux::render::kTextureMipDemandBatchCapacity);
        texture_streaming_.maximum_replacement_tasks = std::max(
            1u, texture_streaming_.maximum_replacement_tasks);
        texture_streaming_.maximum_replacement_bytes = std::max(
            std::uint64_t{1u},
            texture_streaming_.maximum_replacement_bytes);
        // 查已就绪句柄(材质域解依赖用):READY 行 → bits,其余 → 0。
        HandleLookup lookup = [t = &table_](const lux::asset::asset_id_t& id)
            -> std::uint64_t
        {
            using EState  = RenderResourceStateManager::EState;
            const auto* r = t->find(id);
            return (r != nullptr && r->state == EState::READY)
                       ? r->resident.bits()
                       : 0;
        };

        // 回执 → main ownership handoff。Generation handle 不拥有 runtime；若
        // owner 顺序被破坏，reaper 会在 false 路径同步做纯补偿。
        TryPostToMain post_main =
            [main = async.mainThreadDispatcher()](
                lux::cxx::move_only_function<void()> task) noexcept
            { return main.tryDispatchToMainThread(std::move(task)); };

        auto texture_subservice = std::make_unique<TextureSubservice>(
            control,
            upload_,
            assets,
            post_main,
            texture_streaming_);
        texture_subservice_ = texture_subservice.get();
        service_.addSubservice(std::move(texture_subservice));
        service_.addSubservice(std::make_unique<MeshSubservice>(
            control, upload_, assets, catalog, post_main));
        auto material_subservice = std::make_unique<MaterialSubservice>(
            control,
            upload_,
            assets,
            catalog,
            material_store_,
            lookup,
            post_main,
            *residency_tasks_
        );
        material_subservice_ = material_subservice.get();
        service_.addSubservice(std::move(material_subservice));
        service_.addSubservice(std::make_unique<MaterialInstanceSubservice>(
            control, upload_, assets, catalog, material_store_, std::move(lookup),
            std::move(post_main)));

        const std::weak_ptr<CallbackControl> callback_control =
            callback_control_;
        auto guarded_failure_sink =
            [callback_control, sink = std::move(failure_sink)](
                const lux::ecs::RenderResourceFailed& failure) mutable
            {
                const auto control = callback_control.lock();
                auto* owner = control
                                  ? control->tryOwnerForCompletion()
                                  : nullptr;
                if (owner == nullptr)
                    return;

                ActiveCallGuard callback{owner->active_call_depth_};
                if (sink)
                    sink(failure);
            };
        ctx_ = std::make_unique<render_resource::Context>(
            table_,
            service_,
            assets,
            std::move(asset_client),
            *residency_tasks_,
            async,
            std::move(guarded_failure_sink)
        );
    }

    ResidencyAssembly::Impl::~Impl()
    {
        if (closed_)
            return;

        if (active_call_depth_ != 0 || async_->isDrainingMainThreadCompletions())
        {
            lux::render::renderFatal(
                "ResidencyAssembly destroyed from inside its own callback; "
                "retain session/executor/assembly and drive closeAsync() "
                "after that callback stack unwinds"
            );
        }

        // The RAII backstop only advances an already-live protocol once. Normal
        // composition must observe closeAsync() to terminal before destruction;
        // silently joining senders or dropping reapers here would only hide that
        // the render channel/runtime was stopped too early.
        const ResidencyCloseReport report = advanceClose();
        if (!report.terminal())
        {
            lux::render::renderFatal(
                "ResidencyAssembly destruction could not complete the live "
                "render close protocol; keep session/executor/render thread "
                "alive and drive closeAsync() at a host safe point"
            );
        }
    }

    void ResidencyAssembly::Impl::onUnreferenced(
        const lux::asset::asset_id_t& id)
    {
        if (closed_ || !ctx_->acceptsNewOperations())
            return;
        ActiveCallGuard entry{active_call_depth_};
        render_resource::onUnreferenced(*ctx_, id);
    }

    void ResidencyAssembly::Impl::onInvalidated(
        const lux::asset::asset_id_t& id)
    {
        if (closed_ || !ctx_->acceptsNewOperations())
            return;
        ActiveCallGuard entry{active_call_depth_};
        render_resource::onInvalidated(*ctx_, id);
    }

    void ResidencyAssembly::Impl::onContentChanged(
        const lux::asset::asset_id_t& id)
    {
        if (closed_ || !ctx_->acceptsNewOperations())
            return;
        ActiveCallGuard entry{active_call_depth_};
        render_resource::onContentChanged(*ctx_, id);
    }

    void ResidencyAssembly::Impl::onAssetRegistered(
        const lux::asset::asset_id_t& id)
    {
        if (closed_ || !ctx_->acceptsNewOperations())
            return;
        ActiveCallGuard entry{active_call_depth_};
        render_resource::onAssetRegistered(*ctx_, id);
    }

    ResidencyAssembly::Impl::AssetEventCallbacks
    ResidencyAssembly::Impl::makeAssetEventCallbacks()
    {
        const std::weak_ptr<CallbackControl> callback_control =
            callback_control_;
        const auto guarded = [callback_control](auto member)
        {
            return [callback_control, member](
                       const lux::asset::asset_id_t& id)
            {
                const auto control = callback_control.lock();
                auto* owner = control
                                  ? control->tryOwnerForAdmission()
                                  : nullptr;
                if (owner != nullptr)
                    (owner->*member)(id);
            };
        };

        return {
            .unreferenced = guarded(&Impl::onUnreferenced),
            .invalidated = guarded(&Impl::onInvalidated),
            .content_changed = guarded(&Impl::onContentChanged),
            .registered = guarded(&Impl::onAssetRegistered),
        };
    }

    lux::ecs::ResidencyCallbacks ResidencyAssembly::Impl::makeCallbacks()
    {
        lux::ecs::ResidencyCallbacks cbs;
        const std::weak_ptr<CallbackControl> callback_control =
            callback_control_;
        cbs.request = [callback_control](
                          const lux::asset::asset_id_t& id,
                          lux::ecs::EResourceDomain     domain)
        {
            const auto control = callback_control.lock();
            auto* owner = control
                              ? control->tryOwnerForAdmission()
                              : nullptr;
            if (owner != nullptr)
                owner->request(id, domain);
        };
        cbs.await = [callback_control](
                          const lux::asset::asset_id_t&           id,
                          lux::ecs::ResidencyCallbacks::DeliverFn deliver)
        {
            const auto control = callback_control.lock();
            auto* owner = control
                              ? control->tryOwnerForAdmission()
                              : nullptr;
            if (owner == nullptr)
                return lux::ecs::ResidencyCallbacks::Ticket{};

            // await() is registration-only today. Guard both registration and
            // eventual delivery so a future READY replay cannot reopen this
            // self-join hole by becoming synchronous.
            ActiveCallGuard entry{owner->active_call_depth_};
            return wrapTicket(owner->service_.await(
                id,
                [callback_control, d = std::move(deliver)](
                    std::uint64_t                    bits,
                    const lux::ecs::ResourceFailure* failure)
                {
                    const auto callback = callback_control.lock();
                    auto* callback_owner = callback
                                               ? callback->tryOwnerForCompletion()
                                               : nullptr;
                    if (callback_owner == nullptr)
                        return;

                    ActiveCallGuard guard{
                        callback_owner->active_call_depth_
                    };
                    d(bits, failure);
                }));
        };
        cbs.watch_invalidation =
            [callback_control](std::function<void(
                                   const std::vector<
                                       lux::asset::asset_id_t>&)> fn)
        {
            const auto control = callback_control.lock();
            auto* owner = control
                              ? control->tryOwnerForAdmission()
                              : nullptr;
            if (owner == nullptr)
                return lux::ecs::ResidencyCallbacks::Ticket{};

            ActiveCallGuard entry{owner->active_call_depth_};
            return wrapTicket(owner->service_.watchInvalidation(
                [callback_control, f = std::move(fn)](
                    const std::vector<lux::asset::asset_id_t>& ids)
                {
                    const auto callback = callback_control.lock();
                    auto* callback_owner = callback
                                               ? callback->tryOwnerForCompletion()
                                               : nullptr;
                    if (callback_owner == nullptr)
                        return;

                    ActiveCallGuard guard{
                        callback_owner->active_call_depth_
                    };
                    f(ids);
                }));
        };
        return cbs;
    }

    void ResidencyAssembly::Impl::request(
        const lux::asset::asset_id_t& id,
        lux::ecs::EResourceDomain     domain)
    {
        if (closed_ || !ctx_->acceptsNewOperations())
            return;
        ActiveCallGuard entry{active_call_depth_};
        render_resource::ensure(*ctx_, id, domain);
    }

    void ResidencyAssembly::Impl::tickTextureStreaming()
    {
        if (closing_ || closed_ || texture_subservice_ == nullptr)
            return;

        if (mip_demand_query_.valid())
        {
            if (!mip_demand_query_.isReady())
                return;
            if (const auto result = mip_demand_query_.tryResult(); result)
                texture_subservice_->applyMipDemands(result->get());
            mip_demand_query_ = {};
        }

        if ((++mip_poll_counter_ %
             texture_streaming_.query_interval_frames) != 0u)
            return;
        mip_demand_query_ = control_->request<
            lux::render::TextureMipDemandsReply>(
                lux::render::opcodes::ResourceOp,
                lux::render::type_ids::QueryTextureMipDemands,
                lux::render::QueryTextureMipDemandsPayload{
                    texture_streaming_.maximum_demand_entries});
    }

    std::uint64_t ResidencyAssembly::Impl::peekReadyBits(
        const lux::asset::asset_id_t& id) const
    {
        using EState  = RenderResourceStateManager::EState;
        const auto* r = table_.find(id);
        return (r != nullptr && r->state == EState::READY)
                   ? r->resident.bits()
                   : 0;
    }

    bool ResidencyAssembly::Impl::hasInflight() const
    {
        return render_resource::hasInflight(*ctx_);
    }

    ResidencyCloseSnapshot ResidencyAssembly::Impl::closeSnapshot() const noexcept
    {
        ResidencyCloseSnapshot snapshot{
            .closing = closing_,
            .scope_close_started = scope_close_started_,
            .scope_closed = scope_closed_,
            .active_call_depth = active_call_depth_,
            .close_waiters = close_waiters_.size(),
            .mesh_replies = service_.pendingReplies(
                lux::ecs::EResourceDomain::MESH),
            .texture_replies = service_.pendingReplies(
                lux::ecs::EResourceDomain::TEXTURE),
            .material_replies = service_.pendingReplies(
                lux::ecs::EResourceDomain::MATERIAL),
            .material_shader_replies = material_subservice_ != nullptr
                ? material_subservice_->pendingShaderReplies()
                : 0u,
            .material_upload_replies = material_subservice_ != nullptr
                ? material_subservice_->pendingUploadReplies()
                : 0u,
            .material_instance_replies = service_.pendingReplies(
                lux::ecs::EResourceDomain::MATERIAL_INSTANCE),
            .live_gpu_leases = service_.liveLeases(),
            .release_control_references =
                service_.releaseControlReferences(),
            .operation_control_references =
                ctx_->operationControlReferences(),
            .domain_owner_controls_quiescent =
                service_.ownerControlsQuiescent(),
        };
        const_cast<RenderResourceStateManager&>(table_).forEach(
            [&snapshot](const lux::asset::asset_id_t&,
                        RenderResourceStateManager::Row& row)
            {
                using EState = RenderResourceStateManager::EState;
                switch (row.state)
                {
                case EState::UNLOADED:  ++snapshot.rows_unloaded; break;
                case EState::LOADING:   ++snapshot.rows_loading; break;
                case EState::UPLOADING: ++snapshot.rows_uploading; break;
                case EState::READY:     ++snapshot.rows_ready; break;
                case EState::FAILED:    ++snapshot.rows_failed; break;
                }
            });
        return snapshot;
    }

    void ResidencyAssembly::Impl::teardown()
    {
        ActiveCallGuard entry{active_call_depth_};
        render_resource::teardown(*ctx_);
    }

    ResidencyCloseReport ResidencyAssembly::Impl::advanceClose() noexcept
    {
        if (closed_)
        {
            return {EResidencyCloseStatus::AlreadyClosed};
        }
        const auto retry = [this](EResidencyCloseStatus status) noexcept
        {
            return ResidencyCloseReport{
                .status = status,
                .pending_owner_replies = service_.pendingReplies(),
                .inflight_work = hasInflight()};
        };
        if (closing_)
            return retry(EResidencyCloseStatus::CloseInProgress);
        // A residency completion currently executing in drainMainThreadCompletions still owns
        // one child slot in residency_tasks_. Joining here would wait for this
        // very callback to return. Refuse before mutating close state; the
        // caller can retry at the next host-loop safe point.
        // Cache hits, immediate failures, READY/FAILED replay and invalidation
        // fan-out all dispatch inline. They do not necessarily pass through
        // MainThreadMailbox, but may still be inside an owner child or a table/service
        // operation which must unwind before close mutates either structure.
        if (active_call_depth_ != 0)
            return retry(EResidencyCloseStatus::CloseInProgress);

        closing_ = true;
        struct ClosingGuard final
        {
            bool& flag;
            ~ClosingGuard() noexcept { flag = false; }
        } closing_guard{closing_};

        // This is the close commitment point. A failed first preflight leaves
        // admission open; after this transition every retry remains DRAINING,
        // while senders admitted by the old generation may still complete.
        ctx_->beginDraining();

        if (!scope_close_started_)
        {
            scope_close_started_ = true;
            lux::exec::detail::subscribeScopeClose(
                *residency_tasks_,
                [this]() noexcept
                {
                    scope_closed_ = true;
                    if (!closing_)
                    {
                        const auto report = advanceClose();
                        if (report.terminal())
                            finishClose(report);
                    }
                });
        }

        if (!scope_closed_ || service_.pendingReplies() != 0)
            return retry(EResidencyCloseStatus::CloseInProgress);

        teardown();

        if (!service_.releaseQuiescent())
            lux::render::renderFatal(
                "Residency close reached terminal with a live GPU lease or "
                "upload release capability"
            );

        if (!service_.ownerControlsQuiescent())
            lux::render::renderFatal(
                "Residency close reached terminal while a domain sender, "
                "reply trampoline, or runtime control still retained a host "
                "endpoint"
            );

        if (!ctx_->operationQuiescent())
            lux::render::renderFatal(
                "Residency close joined its AsyncScope but sender generation "
                "tokens remain live"
            );
        ctx_->invalidateAfterJoin();

        callback_control_->invalidate();
        closed_ = true;
        auto report = ResidencyCloseReport{EResidencyCloseStatus::Closed};
        finishClose(report);
        return report;
    }

    void ResidencyAssembly::Impl::subscribeClose(
        lux::cxx::move_only_function<void(ResidencyCloseReport)> completion)
        noexcept
    {
        if (!completion)
            return;
        auto pending = std::make_shared<
            lux::cxx::move_only_function<void(ResidencyCloseReport)>>(
                std::move(completion));
        if (!async_->mainThreadDispatcher().tryDispatchToMainThread(
                [this, pending]() mutable noexcept
                {
                    subscribeCloseOnMain(std::move(*pending));
                }))
        {
            if (*pending)
                (*pending)(ResidencyCloseReport{
                    EResidencyCloseStatus::CloseInProgress});
        }
    }

    void ResidencyAssembly::Impl::subscribeCloseOnMain(
        lux::cxx::move_only_function<void(ResidencyCloseReport)> completion)
        noexcept
    {
        if (!completion)
            return;
        if (closed_)
        {
            completion(advanceClose());
            return;
        }
        close_waiters_.push_back(std::move(completion));
        const auto report = advanceClose();
        if (report.terminal())
            finishClose(report);
    }

    void ResidencyAssembly::Impl::finishClose(
        ResidencyCloseReport report) noexcept
    {
        if (!report.terminal() || close_waiters_.empty())
            return;
        auto waiters = std::move(close_waiters_);
        close_waiters_.clear();
        for (auto& waiter : waiters)
            if (waiter)
                waiter(report);
    }

    ResidencyAssembly::ResidencyAssembly(
        lux::render::RenderControlSession& control,
        lux::render::RenderUploadClient upload,
        lux::asset::AssetManager&          assets,
        const lux::render::FeatureCatalog& catalog,
        lux::asset_runtime::AssetClient    asset_client,
        lux::exec::AsyncRuntime&           async,
        FailureSink                        failure_sink,
        TextureStreamingBudget texture_streaming)
        : impl_(std::make_unique<Impl>(
              control,
              std::move(upload),
              assets,
              catalog,
              std::move(asset_client),
              async,
              std::move(failure_sink),
              texture_streaming
          ))
    {}

    ResidencyAssembly::~ResidencyAssembly() = default;

    void ResidencyAssembly::onUnreferenced(
        const lux::asset::asset_id_t& id)
    {
        impl_->onUnreferenced(id);
    }

    void ResidencyAssembly::onInvalidated(
        const lux::asset::asset_id_t& id)
    {
        impl_->onInvalidated(id);
    }

    void ResidencyAssembly::onContentChanged(
        const lux::asset::asset_id_t& id)
    {
        impl_->onContentChanged(id);
    }

    void ResidencyAssembly::onAssetRegistered(
        const lux::asset::asset_id_t& id)
    {
        impl_->onAssetRegistered(id);
    }

    ResidencyAssembly::AssetEventCallbacks
    ResidencyAssembly::makeAssetEventCallbacks()
    {
        return impl_->makeAssetEventCallbacks();
    }

    lux::ecs::ResidencyCallbacks ResidencyAssembly::makeCallbacks()
    {
        return impl_->makeCallbacks();
    }

    void ResidencyAssembly::request(
        const lux::asset::asset_id_t& id,
        lux::ecs::EResourceDomain     domain)
    {
        impl_->request(id, domain);
    }

    void ResidencyAssembly::tickTextureStreaming()
    {
        if (impl_)
            impl_->tickTextureStreaming();
    }

    std::uint64_t ResidencyAssembly::peekReadyBits(
        const lux::asset::asset_id_t& id) const
    {
        return impl_->peekReadyBits(id);
    }

    bool ResidencyAssembly::hasInflight() const noexcept
    {
        return impl_ && impl_->hasInflight();
    }

    ResidencyCloseSnapshot ResidencyAssembly::closeSnapshot() const noexcept
    {
        return impl_ ? impl_->closeSnapshot() : ResidencyCloseSnapshot{};
    }

    ResidencyCloseSender ResidencyAssembly::closeAsync() noexcept
    {
        return ResidencyCloseSender{*this};
    }

    void ResidencyAssembly::subscribeClose(
        lux::cxx::move_only_function<void(ResidencyCloseReport)> completion)
        noexcept
    {
        if (!impl_)
        {
            completion(ResidencyCloseReport{
                EResidencyCloseStatus::AlreadyClosed});
            return;
        }
        impl_->subscribeClose(std::move(completion));
    }

    void detail::subscribeResidencyClose(
        ResidencyAssembly& assembly,
        lux::cxx::move_only_function<void(ResidencyCloseReport)> completion)
        noexcept
    {
        assembly.subscribeClose(std::move(completion));
    }

} // namespace lux::runtime

#include <lux/engine/function/render/client/core/RenderFatal.hpp>
#include <lux/engine/render/RenderRuntime.hpp>
#include <lux/engine/render/detail/RendererThread.hpp>
#include <lux/engine/render/detail/ReplyPump.hpp>
#include <lux/engine/render/detail/UploadQueue.hpp>

#include <cmath>
#include <limits>

namespace lux::render
{
    namespace
    {
        auto fail(ERendererError code) noexcept
        {
            return lux::cxx::unexpected(RendererFailure{code});
        }

        template <class T, std::size_t Slots>
        bool clearJoinedRing(lux::cxx::BoundedSpscFrameRing<T, Slots>& ring) noexcept
        {
            // Backend join transfers both ring roles here, including historical
            // storage slots that are not part of the published queue anymore.
            ring.currentRead().clear_keep_capacity();
            while (ring.tryAcquireRead())
            {
                ring.currentRead().clear_keep_capacity();
            }
            for (std::size_t i = 0; i < Slots; ++i)
            {
                auto* slot = ring.tryBeginWrite();
                if (!slot)
                {
                    return false;
                }
                slot->clear_keep_capacity();
                if (!ring.publishWrite() || !ring.tryAcquireRead())
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace

    struct RenderRuntime::Impl final
    {
        explicit Impl(RendererConfig value)
            : config(std::move(value)), thread(config), diagnostics(config.diagnostic_capacity)
        {
        }

        const std::thread::id owner{std::this_thread::get_id()};
        RendererConfig config;
        detail::RendererThread thread;
        RenderProgramSession programs{thread.frames, thread.sync};
        RenderControlSession control{thread.controls, thread.sync};
        RenderUploadSession uploads{thread.uploads, thread.sync};
        std::shared_ptr<detail::UploadQueue> upload_queue{
            std::make_shared<detail::UploadQueue>(config.upload_capacity, config.upload_byte_capacity, thread.sync)
        };
        RenderUploadClient upload_client{RenderUploadClient::bind(upload_queue, &detail::UploadQueue::submit)};
        std::jthread worker;
        std::vector<RendererDiagnostic> diagnostics;
        std::size_t head{}, count{}, next_reply_lane{}, storage_rotations{};
        std::optional<RendererDiagnostic> terminal_diagnostic;
        bool terminal_reported{}, closing{}, retired{}, busy{};

        struct FeatureBatch final
        {
            std::vector<RenderFeatureRegistration> candidates;
            std::vector<FeatureTypeRegisteredReply> accepted;
            TRenderRequest<FeatureTypeRegisteredReply> registration;
            TRenderRequest<GenericOkReply> rollback;
            FeatureRegistrationStatus status{EFeatureRegistrationState::REGISTERING, {}};
            bool cancelled{};
        };

        std::optional<FeatureBatch> feature_batch;

        [[nodiscard]] bool featureBatchPending() const noexcept
        {
            if (!feature_batch)
            {
                return false;
            }
            const auto state = feature_batch->status.state;
            return state == EFeatureRegistrationState::REGISTERING || state == EFeatureRegistrationState::READY ||
                   state == EFeatureRegistrationState::ROLLING_BACK;
        }

        void collectFeatureReplies()
        {
            if (!featureBatchPending())
            {
                return;
            }
            auto& batch = *feature_batch;
            if (retired)
            {
                const auto error = thread.sync->terminalError();
                batch.status = {
                    EFeatureRegistrationState::FAILED,
                    error.ok() ? renderError<err::feature::InvalidRegistration>() : error
                };
                return;
            }
            if (batch.registration.valid())
            {
                if (!batch.registration.isReady())
                {
                    return;
                }
                auto value = batch.registration.tryResult();
                const auto error = value ? value->get().error : value.error();
                if (!error.ok() || (value && value->get().feature_type_id == 0))
                {
                    batch.status.error = error.ok() ? renderError<err::feature::InvalidRegistration>() : error;
                    batch.status.state = EFeatureRegistrationState::ROLLING_BACK;
                }
                else
                {
                    batch.accepted.push_back(value->get());
                }
                batch.registration = {};
            }
            if (batch.status.state == EFeatureRegistrationState::ROLLING_BACK)
            {
                if (batch.rollback.valid())
                {
                    if (!batch.rollback.isReady())
                    {
                        return;
                    }
                    auto value = batch.rollback.tryResult();
                    if (!value || value->get().code != 0 || !value->get().error.ok())
                    {
                        const auto error = value ? value->get().error : value.error();
                        batch.status.error = error.ok() ? renderError<err::feature::InvalidRegistration>() : error;
                        // A rejected rollback is a backend contract failure; close must still
                        // retire the server before any candidate code can be released.
                        thread.sync->requestStop();
                        return;
                    }
                    batch.accepted.pop_back();
                    batch.rollback = {};
                }
                if (batch.accepted.empty())
                {
                    batch.status.state =
                        batch.cancelled ? EFeatureRegistrationState::CANCELLED : EFeatureRegistrationState::FAILED;
                    return;
                }
                return;
            }
            if (batch.status.state != EFeatureRegistrationState::REGISTERING)
            {
                return;
            }
            if (batch.accepted.size() == batch.candidates.size())
            {
                batch.status.state = EFeatureRegistrationState::READY;
                return;
            }
        }

        void submitFeatureRequest(std::size_t& budget)
        {
            if (!featureBatchPending() || !budget || !control.canSubmit() || retired)
            {
                return;
            }
            auto& batch = *feature_batch;
            if (batch.registration.valid() || batch.rollback.valid())
            {
                return;
            }
            if (batch.status.state == EFeatureRegistrationState::ROLLING_BACK)
            {
                if (batch.accepted.empty())
                {
                    return;
                }
                batch.rollback = control.unregisterFeatureType(batch.accepted.back().feature_type_id);
                --budget;
            }
            else if (batch.status.state == EFeatureRegistrationState::REGISTERING &&
                     batch.accepted.size() < batch.candidates.size())
            {
                const auto& candidate = batch.candidates[batch.accepted.size()];
                batch.registration = control.registerFeatureType(candidate.factory, candidate.code_lifetime);
                --budget;
            }
        }

        RenderResult<void> check() const noexcept
        {
            if (owner != std::this_thread::get_id())
            {
                return fail(ERendererError::WRONG_THREAD);
            }
            if (busy)
            {
                return fail(ERendererError::BUSY);
            }
            return {};
        }

        void record(RendererDiagnostic value) noexcept
        {
            if (value.terminal)
            {
                terminal_diagnostic = value;
            }
            else if (count == diagnostics.size())
            {
                thread.statistics->dropped += value.occurrences;
            }
            else
            {
                diagnostics[(head + count++) % diagnostics.size()] = value;
            }
        }
    };

    RenderRuntime::RenderRuntime(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    RenderRuntime::~RenderRuntime()
    {
        if (!beginRetirement())
        {
            renderFatal("RenderRuntime destruction outside its owner thread or within a callback");
        }
        for (;;)
        {
            // Snapshot before progressing: a reply published during adoption must not be missed.
            const auto epoch = impl_->thread.sync->work_epoch.load(std::memory_order_acquire);
            auto replies = std::numeric_limits<std::size_t>::max();
            auto controls = replies, programs = replies;
            const auto closed = advanceRetirement(replies, controls, programs);
            if (!closed)
            {
                renderFatal("RenderRuntime close contract failed during destruction");
            }
            if (*closed)
            {
                break;
            }
            impl_->thread.sync->work_epoch.wait(epoch, std::memory_order_acquire);
        }
    }

    RenderResult<std::unique_ptr<RenderRuntime>> RenderRuntime::create(
        RendererConfig config,
        ValidationMessageSink diagnostics
    )
    {
        if (auto registered = registerRendererErrors(); !registered)
        {
            renderFatal("RenderRuntime error definitions conflict with the registered schema");
        }
        if (config.frame_capacity < 2 || config.frame_capacity > 3 || config.control_capacity < 2 ||
            config.control_capacity > 65536 || config.upload_capacity < 2 || config.upload_capacity > 65536 ||
            !config.upload_byte_capacity || !config.diagnostic_capacity)
        {
            return fail(ERendererError::INVALID_ARGUMENT);
        }
        auto impl = std::make_unique<Impl>(std::move(config));
        impl->programs.setErrorEventHandler(
            [stats = impl->thread.statistics](const auto& batch) { stats->dropped += batch.dropped; },
            [owner = impl.get()](const auto& event)
            {
                owner->thread.statistics->events += event.occurrences;
                owner->record(
                    {{ERendererError::DEVICE_FAILURE, event.error},
                     event.scene_index,
                     event.occurrences,
                     event.frame_serial,
                     event.seq,
                     false}
                );
            }
        );
        auto& data = *impl;
        auto started = detail::startRendererThread(data.thread, data.config, std::move(diagnostics));
        if (!started)
        {
            return lux::cxx::unexpected(started.error());
        }
        data.worker = std::move(*started);
        while (data.thread.startup.load(std::memory_order_acquire) == 0)
        {
            data.thread.startup.wait(0, std::memory_order_acquire);
        }
        if (data.thread.startup.load(std::memory_order_acquire) != 1)
        {
            data.worker.join();
            return lux::cxx::unexpected(RendererFailure{ERendererError::DEVICE_FAILURE, data.thread.startup_error});
        }
        // Publish only after native startup succeeds; failed prefixes never
        // construct a semantic Runtime requiring a partial-lifetime sentinel.
        return std::unique_ptr<RenderRuntime>(new RenderRuntime(std::move(impl)));
    }

    bool RenderRuntime::controlAvailable(std::size_t packets) const noexcept
    {
        if (impl_->owner != std::this_thread::get_id())
        {
            return false;
        }
        const bool is_unavailable = impl_->busy || impl_->closing || impl_->thread.sync->isStopping();
        if (is_unavailable)
        {
            return false;
        }
        return impl_->control.canSubmit(packets);
    }

    RenderResult<std::reference_wrapper<RenderControlSession>> RenderRuntime::control() noexcept
    {
        if (auto checked = impl_->check(); !checked)
        {
            return lux::cxx::unexpected(checked.error());
        }
        if (impl_->closing)
        {
            return fail(ERendererError::STOPPING);
        }
        return std::ref(impl_->control);
    }

    RenderResult<RenderUploadClient> RenderRuntime::upload() noexcept
    {
        if (auto checked = impl_->check(); !checked)
        {
            return lux::cxx::unexpected(checked.error());
        }
        if (impl_->closing)
        {
            return fail(ERendererError::STOPPING);
        }
        return impl_->upload_client;
    }

    const FeatureCatalog& RenderRuntime::features() const noexcept
    {
        return impl_->thread.catalog;
    }

    RenderResult<EFrameSubmit> RenderRuntime::submit(TRenderProgram<>& input) noexcept
    {
        if (auto checked = impl_->check(); !checked)
        {
            return lux::cxx::unexpected(checked.error());
        }
        if (impl_->closing || impl_->thread.sync->isStopping())
        {
            return fail(ERendererError::STOPPING);
        }
        if (!impl_->programs.trySubmitPrepared(input))
        {
            return EFrameSubmit::BACKPRESSURED;
        }
        // Four storage slots retain attachments independently of FIFO capacity
        // and GPU FIF. Idle retirement rotates that storage a bounded one turn
        // at a time, without pretending these are business updates.
        impl_->storage_rotations = TRenderProgramChannel<>::request_slot_count;
        return EFrameSubmit::SUBMITTED;
    }

    RenderResult<std::size_t> RenderRuntime::collectCompletions(std::size_t replies)
    {
        auto& data = *impl_;
        if (auto checked = data.check(); !checked)
        {
            return lux::cxx::unexpected(checked.error());
        }

        struct Gate final
        {
            bool& busy;

            ~Gate()
            {
                busy = false;
            }
        } gate{data.busy};

        data.busy = true;
        auto consumed =
            detail::pumpRendererReplies(data.control, data.programs, data.uploads, data.next_reply_lane, replies);
        const auto terminal = data.thread.sync->terminalError();
        if (!data.terminal_reported && !terminal.ok())
        {
            data.record({{ERendererError::DEVICE_FAILURE, terminal}, RenderErrorEvent::kNoScene, 1, 0, 0, true});
            data.terminal_reported = true;
        }
        if (data.thread.stopped.load(std::memory_order_acquire) && !data.retired)
        {
            if (data.worker.joinable())
            {
                data.worker.join();
                if (!clearJoinedRing(data.thread.frames->requests))
                {
                    return fail(ERendererError::CONTRACT_FAILURE);
                }
                data.programs.rawClient().retireAfterBackendStopped();
                TOperationPacket<> packet;
                while (data.thread.controls->requests.tryPop(packet) == lux::cxx::EQueuePopResult::VALUE)
                {
                    packet = {};
                }
                while (data.thread.uploads->requests.tryPop(packet) == lux::cxx::EQueuePopResult::VALUE)
                {
                    data.thread.uploads->releaseBytes(packet.accountedBytes());
                    packet = {};
                }
            }
            // Join proves there can be no later reply publication. Re-check the
            // original rings after that boundary; never replace a queued success
            // with a fabricated terminal failure, or exceed this call's budget.
            const auto remaining = replies - consumed;
            const auto published =
                detail::pumpRendererReplies(data.control, data.programs, data.uploads, data.next_reply_lane, remaining);
            consumed += published;
            if (published < remaining)
            {
                const auto stopped_error = data.thread.sync->terminalError();
                const auto failure = stopped_error.ok() ? renderError<err::comm::ChannelStopping>() : stopped_error;
                consumed += data.control.callbacks_.failPending(failure, replies - consumed);
                consumed += data.programs.rawClient().callbacks_.failPending(failure, replies - consumed);
                consumed += data.uploads.callbacks_.failPending(failure, replies - consumed);
                const bool has_callbacks = data.control.callbacks_.pendingCallbacks() ||
                                           data.programs.rawClient().callbacks_.pendingCallbacks() ||
                                           data.uploads.callbacks_.pendingCallbacks();
                data.retired = !has_callbacks;
            }
        }
        data.collectFeatureReplies();
        if (consumed)
        {
            data.thread.sync->notifyRequestStateChanged();
        }
        return consumed;
    }

    RenderResult<void> RenderRuntime::submitPending(std::size_t& controls, std::size_t& programs)
    {
        auto& data = *impl_;
        if (auto checked = data.check(); !checked)
        {
            return checked;
        }

        struct Gate final
        {
            bool& busy;

            ~Gate()
            {
                busy = false;
            }
        } gate{data.busy};

        data.busy = true;
        data.submitFeatureRequest(controls);
        // Upload forwarding consumes the explicit command-work allowance;
        // accepted uploads are never counted as consumed reply envelopes.
        controls -= data.upload_queue->poll(data.uploads, data.thread.sync->isStopping(), controls);
        if (!data.thread.sync->isStopping())
        {
            if (programs && data.storage_rotations)
            {
                TRenderProgram<> maintenance;
                RenderProgramSession::Builder builder(maintenance);
                builder.begin({});
                maintenance.kind = ERenderProgramKind::STATE_UPDATE;
                if (data.programs.trySubmitPrepared(maintenance))
                {
                    --programs;
                    if (data.storage_rotations)
                    {
                        --data.storage_rotations;
                    }
                }
            }
        }
        return {};
    }

    RenderResult<void> RenderRuntime::bindProgress(std::shared_ptr<void> owner, void (*wake)(void*) noexcept) noexcept
    {
        if (auto checked = impl_->check(); !checked)
        {
            return checked;
        }
        if (!owner || !wake || impl_->closing)
        {
            return fail(ERendererError::INVALID_ARGUMENT);
        }
        {
            impl_->thread.sync->bindExternalWake(owner, wake);
        }

        wake(owner.get()); // Fold in completions published before the connection.
        return {};
    }

    RenderResult<void> RenderRuntime::beginFeatureRegistration(std::vector<RenderFeatureRegistration> candidates)
    {
        if (auto checked = impl_->check(); !checked)
        {
            return checked;
        }
        if (impl_->closing || impl_->thread.sync->isStopping())
        {
            return fail(ERendererError::STOPPING);
        }
        if (impl_->featureBatchPending())
        {
            return fail(ERendererError::BUSY);
        }
        std::vector<FeatureTypeId> identities;
        std::vector<std::string_view> names;
        for (const auto& candidate : candidates)
        {
            const auto& factory = candidate.factory;
            const auto& descriptor = factory.descriptor;
            const bool invalid_identity = !descriptor.valid() || descriptor.canonical_name.empty() ||
                                          featureId(descriptor.canonical_name) != descriptor.type ||
                                          !descriptor.abi_version;
            const bool invalid_factory =
                !factory.create_fn || !factory.name || factory.name[0] == '\0' || factory.operation_count > 16 ||
                (factory.operation_count && (!factory.register_ops_fn || !factory.unregister_ops_fn)) ||
                (candidate.scene_configurable && !candidate.configuration.valid());
            if (invalid_identity || invalid_factory)
            {
                return fail(ERendererError::INVALID_ARGUMENT);
            }
            if (impl_->thread.catalog.find(descriptor.type) || impl_->thread.catalog.find(factory.name) ||
                std::ranges::find(identities, descriptor.type) != identities.end() ||
                std::ranges::find(names, factory.name) != names.end())
            {
                return fail(ERendererError::INVALID_ARGUMENT);
            }
            identities.push_back(descriptor.type);
            names.push_back(factory.name);
        }
        impl_->feature_batch.emplace();
        impl_->feature_batch->candidates = std::move(candidates);
        impl_->feature_batch->accepted.reserve(impl_->feature_batch->candidates.size());
        impl_->thread.sync->notifyRequestStateChanged();
        return {};
    }

    FeatureRegistrationStatus RenderRuntime::featureRegistrationStatus() const noexcept
    {
        return impl_->feature_batch ? impl_->feature_batch->status : FeatureRegistrationStatus{};
    }

    RenderResult<void> RenderRuntime::commitFeatureRegistration()
    {
        if (auto checked = impl_->check(); !checked)
        {
            return checked;
        }
        if (!impl_->feature_batch || impl_->feature_batch->status.state != EFeatureRegistrationState::READY)
        {
            return fail(ERendererError::NOT_READY);
        }
        auto& batch = *impl_->feature_batch;
        for (std::size_t index = 0; index < batch.candidates.size(); ++index)
        {
            const auto& reply = batch.accepted[index];
            auto inserted =
                impl_->thread.catalog.add(batch.candidates[index], reply.feature_type_id, {reply.ops, reply.op_count});
            // Main validated the complete input before registration; no other writer
            // can publish between begin and commit. Allocation failure terminates.
            if (!inserted)
            {
                std::terminate();
            }
        }
        batch.status.state = EFeatureRegistrationState::COMMITTED;
        return {};
    }

    RenderResult<void> RenderRuntime::cancelFeatureRegistration() noexcept
    {
        if (auto checked = impl_->check(); !checked)
        {
            return checked;
        }
        if (impl_->featureBatchPending())
        {
            impl_->feature_batch->cancelled = true;
            impl_->feature_batch->status.state = EFeatureRegistrationState::ROLLING_BACK;
            impl_->thread.sync->notifyRequestStateChanged();
        }
        return {};
    }

    RenderRuntimeStatus RenderRuntime::status() const noexcept
    {
        return {
            impl_->retired                                       ? ERenderRuntimeState::RETIRED
            : impl_->closing || impl_->thread.sync->isStopping() ? ERenderRuntimeState::STOPPING
                                                                 : ERenderRuntimeState::ACTIVE,
            impl_->thread.sync->terminalError()
        };
    }

    RendererStatistics RenderRuntime::statistics() const noexcept
    {
        const auto& stats = *impl_->thread.statistics;
        return {
            stats.frames.load(),
            stats.events.load(),
            stats.dropped.load(),
            stats.completed.load(),
            impl_->thread.frames->requests.pendingFrames(),
            static_cast<std::uint64_t>(stats.validation_errors.load())
        };
    }

    RenderResult<std::optional<RendererDiagnostic>> RenderRuntime::takeDiagnostic() noexcept
    {
        if (auto checked = impl_->check(); !checked)
        {
            return lux::cxx::unexpected(checked.error());
        }
        if (!impl_->count)
        {
            return std::exchange(impl_->terminal_diagnostic, std::nullopt);
        }
        const auto result = impl_->diagnostics[impl_->head];
        impl_->head = (impl_->head + 1) % impl_->diagnostics.size();
        --impl_->count;
        return std::optional{result};
    }

    RenderResult<void> RenderRuntime::beginRetirement() noexcept
    {
        if (auto checked = impl_->check(); !checked)
        {
            return checked;
        }
        auto cancelled = cancelFeatureRegistration();
        if (!cancelled)
        {
            return cancelled;
        }
        impl_->closing = true;
        impl_->upload_queue->stop();
        return {};
    }

    RenderResult<bool> RenderRuntime::advanceRetirement(
        std::size_t& replies,
        std::size_t& controls,
        std::size_t& programs
    )
    {
        if (!impl_->closing)
        {
            return fail(ERendererError::BUSY);
        }
        const auto adopted = collectCompletions(replies);
        if (!adopted)
        {
            return lux::cxx::unexpected(adopted.error());
        }
        replies -= *adopted;
        const auto submitted = submitPending(controls, programs);
        if (!submitted)
        {
            return lux::cxx::unexpected(submitted.error());
        }
        if (impl_->featureBatchPending() || !impl_->upload_queue->empty())
        {
            return false;
        }
        if (impl_->retired)
        {
            return true;
        }
        if (impl_->storage_rotations)
        {
            return false;
        }
        // Forwarding a packet is not completion. In particular, a full response
        // ring may keep accepted control/upload work on the backend. Keep its
        // producer alive until the original callbacks and request lanes drain.
        const bool pending_replies = impl_->control.callbacks_.pendingCallbacks() ||
                                     impl_->uploads.callbacks_.pendingCallbacks() ||
                                     impl_->programs.rawClient().callbacks_.pendingCallbacks();
        const bool pending_packets = !impl_->thread.controls->requests.empty() ||
                                     !impl_->thread.uploads->requests.empty() ||
                                     impl_->thread.frames->requests.pendingFrames();
        if (pending_replies || pending_packets)
        {
            return false;
        }
        if (!impl_->thread.sync->isStopping())
        {
            impl_->thread.sync->requestStop();
        }
        return false;
    }

} // namespace lux::render

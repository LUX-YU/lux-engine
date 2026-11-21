#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>
#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>
#include <lux/engine/render/graph/execute/RenderGraphInstance.hpp>
#include <lux/engine/render/runtime/DeviceEntry.hpp>
#include <lux/engine/render/runtime/FrameOperation.hpp>

namespace lux::render::detail
{
    FrameOperation::Workspace::Workspace(
        std::uint32_t slots, std::size_t queues, std::size_t preparation_capacity
    ) noexcept
        : submissions(slots, queues), replicas(slots)
    {
        resource_participants.reserve(preparation_capacity);
    }

    FrameOperation::FrameOperation(DeviceEntry& entry, execution::CpuScheduler cpu) noexcept
        : FrameOperation(entry.session(), entry.frameProgress(), entry.frameWorkspace(), cpu)
    {
        uploads_ = entry.uploader();
        owner_ = &entry;
    }

    FrameOperation::FrameOperation(
        DeviceSession& device, FrameProgress& progress, Workspace& workspace, execution::CpuScheduler cpu
    ) noexcept
        : device_(device), uploads_(nullptr), progress_(progress), workspace_(workspace), cpu_(cpu),
          parallel_recording_(true), owner_(nullptr)
    {
    }

    FrameOperation::~FrameOperation() noexcept
    {
        if (worker_pending_) std::terminate();
        if (active()) cancel(RenderError{kCommandCancelled});
    }

    RenderResult<bool> FrameOperation::tryBegin() noexcept
    {
        if (const auto error = device_.terminalError(); error.type) return cxx::unexpected(error);
        if (active()) return cxx::unexpected(RenderError{kFrameContract});
        auto opened = progress_.tryBegin(device_);
        if (!opened) return cxx::unexpected(opened.error());
        if (!*opened) return false;
        selection_ = **opened;
        phase_ = EPhase::UNIFORMS;
        prepared_instances_ = 0;
        waiting_for_gpu_ = false;
        submission_.emplace(device_, progress_, *selection_, workspace_.submissions);
        if (uploads_) uploads_->reservePreparationStaging(true);
        return true;
    }

    bool FrameOperation::canRun() const noexcept
    {
        if (!active() || worker_pending_) return false;
        if (cancellation_ || device_.terminalError().type) return true;
        if (phase_ == EPhase::SUBMITTING) return submission_->canSubmit();
        if (copies_flushed_) return uploads_->canSubmitPreparationCopies();
        if (!waiting_for_gpu_) return true;
        const auto& observed = workspace_.blocked_completion.serials();
        for (std::size_t i = 0; i < observed.size(); ++i)
        {
            if (device_.physicalQueue(i).completed() != observed[i]) return true;
        }
        return !device_.pendingGpuWork();
    }

    void FrameOperation::finish() noexcept
    {
        const bool unknown = cancellation_ && cancellation_->type == kOutcomeUnknown;
        for (const auto& selected : workspace_.uniforms)
        {
            owner_->uniformPool(selected.pool).versions.unpin(selected.version, unknown);
        }
        workspace_.uniforms.clear();
        for (const auto& selection : workspace_.native_bindings)
            device_.descriptors().releaseNativeBindings(selection);
        workspace_.native_bindings.clear();
        submission_.reset();
        progress_.endFrame(*selection_);
        selection_.reset();
        cancellation_.reset();
        copies_flushed_ = false;
        if (uploads_) uploads_->reservePreparationStaging(false);
    }

    void FrameOperation::settlePreparation(SubmissionPoint accepted, std::size_t ranges, RenderError error) noexcept
    {
        if (settling_) std::terminate();
        settling_ = true;
        cxx::scope_exit settled([&]() noexcept { settling_ = false; });
        if (error.type)
        {
            if (uploads_) uploads_->cancelFrameCopies();
            for (const auto& selected : workspace_.uniforms)
            {
                owner_->uniformPool(selected.pool).versions.settleUpload(selected.version, {}, error);
            }
        }
        const auto locate = [&](ReplicaCopyRange copy_ranges) noexcept -> SubmissionPoint
        {
            if (error.type || !copy_ranges.count) return {};
            const bool outside = copy_ranges.first > ranges || copy_ranges.count > ranges - copy_ranges.first;
            if (outside || !accepted.serial) std::terminate();
            return accepted;
        };
        const auto frame = replicaFrame();
        auto& replicas = workspace_.replicas[frame.slot];
        for (const auto& participant : workspace_.resource_participants)
        {
            const auto [storage, ops] = frameStorage(participant.resource);
            const bool wrong_frame = participant.frame.slot != frame.slot || participant.frame.epoch != frame.epoch;
            if (!storage || !ops || !ops->settleFrame || wrong_frame) std::terminate();
            ops->settleFrame(storage, {frame, participant.ranges, locate(participant.ranges), error});
            if (!error.type && participant.revision)
                replicas[participant.replica] = {participant.resource, participant.revision};
        }
        workspace_.resource_participants.clear();
        for (std::size_t i = 0; i < workspace_.producer_participants.size(); ++i)
        {
            const auto& participant = workspace_.producer_participants[i];
            auto& instance = *workspace_.producer_instances[i];
            const bool wrong_execution = instance.preparation_job_ != participant.job ||
                instance.preparation_generation_ != participant.generation;
            const bool wrong_frame = participant.frame.slot != frame.slot || participant.frame.epoch != frame.epoch;
            if (wrong_execution || wrong_frame) std::terminate();
            for (const auto& copy : std::span{workspace_.prepared_buffers}.subspan(participant.first, participant.count))
                instance.settlePreparedBuffer(frame.slot, copy, locate(copy.ranges), error);
        }
        workspace_.producer_participants.clear();
        workspace_.producer_instances.clear();
        workspace_.prepared_buffers.clear();
        copies_flushed_ = false;
    }

    void FrameOperation::cancel(RenderError error) noexcept
    {
        if (!active()) return;
        if (worker_pending_ || settling_)
        {
            if (!cancellation_) cancellation_ = error;
            return;
        }
        cancellation_ = error;
        settlePreparation({}, 0, error);
        submission_->rejectSuffix(error);
        finish();
    }

    exec::task<RenderResult<std::uint32_t>> FrameOperation::submitPreparation() noexcept
    {
        if (!copies_flushed_)
        {
            auto prepared = uploads_->flushFrameCopies();
            if (!prepared) co_return cxx::unexpected(prepared.error());
            copies_flushed_ = true;
        }
        if (cancellation_) co_return cxx::unexpected(*cancellation_);
        auto submitted = co_await uploads_->submitPreparationCopies();
        if (!submitted) co_return cxx::unexpected(submitted.error());
        if (!*submitted) co_return 0;
        const auto receipt = **submitted;
        submission_->acceptPreparation(receipt.point.serial);
        uploads_->acceptFrameCopies(receipt);
        for (const auto& selected : workspace_.uniforms)
        {
            owner_->uniformPool(selected.pool).versions.settleUpload(selected.version, receipt.point, {});
        }
        settlePreparation(receipt.point, receipt.ranges);
        co_return 1;
    }

    exec::task<RenderResult<std::uint32_t>> FrameOperation::run(
        std::span<RenderGraphInstance* const> instances, std::span<const ResourceInstanceId> storages,
        std::span<const SubmissionOrder> order, std::optional<UniformStateLookup> uniform_state
    ) noexcept
    {
        if (!active() || worker_pending_ || settling_)
        {
            co_return cxx::unexpected(RenderError{kFrameContract});
        }
        worker_pending_ = true;
        cxx::scope_exit settle([this]() noexcept
        {
            worker_pending_ = false;
            if (cancellation_) cancel(*cancellation_);
        });
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        if (stop.stop_requested()) cancellation_ = RenderError{kCommandCancelled};
        if (const auto error = device_.terminalError(); error.type) cancellation_ = error;
        if (cancellation_) co_return cxx::unexpected(*cancellation_);

        const auto accept_uniforms = [&](const RenderGraphInstance& instance, std::uint32_t batch,
                                         SubmissionPoint point) noexcept
        {
            for (const auto& block : instance.uniform_blocks_)
            {
                if (!std::ranges::binary_search(block.batches, batch)) continue;
                const auto selected = std::ranges::find(workspace_.uniforms, block.pool, &SelectedUniformVersion::pool);
                if (selected == workspace_.uniforms.end()) std::terminate();
                owner_->uniformPool(block.pool).versions.accept(selected->version, point);
            }
        };
        const auto batch_receipt = [&]() noexcept -> std::optional<SubmissionOperation::BatchAccepted>
        {
            if (workspace_.uniforms.empty()) return std::nullopt;
            return SubmissionOperation::BatchAccepted{accept_uniforms};
        };
        if (phase_ == EPhase::SUBMITTING)
        {
            auto accepted = co_await submission_->submit(cancellation_, batch_receipt());
            if (!accepted) cancellation_ = accepted.error();
            else if (submission_->complete()) finish();
            co_return accepted;
        }
        auto track = [&](auto result) noexcept
        {
            if (!result) cancellation_ = result.error();
            return result;
        };
        if (uploads_ && uploads_->hasFrameCopies())
        {
            auto copied = co_await submitPreparation();
            co_return track(copied);
        }
        if (phase_ == EPhase::UNIFORMS)
        {
            auto uniform = prepareUniforms(instances, uniform_state);
            if (!uniform) co_return cxx::unexpected(track(uniform).error());
            if (*uniform == EPreparationStatus::AWAITING_PROGRESS)
            {
                if (uploads_ && uploads_->hasFrameCopies())
                {
                    co_return track(co_await submitPreparation());
                }
                workspace_.blocked_completion = workspace_.uniform_completed;
                waiting_for_gpu_ = true;
                co_return 0;
            }
            waiting_for_gpu_ = false;
            phase_ = EPhase::RESOURCES;
        }
        if (!storages.empty() && !uploads_)
        {
            cancellation_ = RenderError{kDeviceArgument};
            co_return cxx::unexpected(*cancellation_);
        }
        if (phase_ == EPhase::RESOURCES)
        {
            auto resources = prepareStorages(storages);
            if (!resources) co_return cxx::unexpected(track(resources).error());
            if (uploads_ && !uploads_->hasFrameCopies()) settlePreparation();
            if (*resources == EPreparationStatus::READY) phase_ = EPhase::PRODUCER_SELECTIONS;
        }
        auto waiting = EExecutionOutcome::RESOURCE_PREPARATION_PENDING;
        if (phase_ == EPhase::PRODUCER_SELECTIONS)
        {
            for (auto* instance : instances)
            {
                if (auto prepared = instance->prepareProducers(slot()); !prepared)
                    co_return cxx::unexpected(track(prepared).error());
            }
            phase_ = EPhase::PRODUCER_UPLOADS;
        }
        if (phase_ == EPhase::PRODUCER_UPLOADS)
        {
            bool ready = true;
            for (auto* instance : instances)
            {
                const auto first = workspace_.prepared_buffers.size();
                auto staged = instance->stagePreparedBuffers(slot(), uploads_, workspace_.prepared_buffers);
                const auto count = workspace_.prepared_buffers.size() - first;
                if (count)
                {
                    workspace_.producer_participants.push_back({
                        instance->preparation_job_, instance->preparation_generation_, replicaFrame(), first, count
                    });
                    workspace_.producer_instances.push_back(instance);
                }
                if (!staged) co_return cxx::unexpected(track(staged).error());
                ready = *staged;
                if (!ready) break;
            }
            if (uploads_ && uploads_->hasFrameCopies())
                co_return track(co_await submitPreparation());
            if (!ready && !device_.pendingGpuWork())
            {
                cancellation_ = RenderError{kDeviceCapacity};
                co_return cxx::unexpected(*cancellation_);
            }
            if (ready) phase_ = EPhase::FREEZE;
        }
        if (uploads_ && uploads_->hasFrameCopies())
        {
            auto copied = co_await submitPreparation();
            co_return track(copied);
        }
        bool ready = false;
        if (phase_ == EPhase::FREEZE)
        {
            auto prepared = prepareInstances(instances);
            if (!prepared) co_return cxx::unexpected(track(prepared).error());
            ready = *prepared == EPreparationStatus::READY;
            waiting = EExecutionOutcome::HISTORY_PENDING;
        }
        if (!ready)
        {
            if (!workspace_.blocked_completion.size())
            {
                workspace_.blocked_completion = device_.makeFrontiers();
            }
            device_.readCompleted(workspace_.blocked_completion);
            waiting_for_gpu_ = true;
            for (auto* instance : instances)
                ++instance->progress_.counts[static_cast<std::size_t>(waiting)];
            co_return 0;
        }
        waiting_for_gpu_ = false;
        // Every graph now has a frozen view of its FIF replica. Workers own disjoint allocator/CB pairs.
        settlePreparation();
        if (uploads_) uploads_->reservePreparationStaging(false);
        if (auto prepared = submission_->prepare(instances, order); !prepared)
            co_return cxx::unexpected(track(prepared).error());
        phase_ = EPhase::RECORDING;
        auto recorded = co_await submission_->record(cpu_, parallel_recording_);
        if (!recorded) co_return cxx::unexpected(track(recorded).error());
        phase_ = EPhase::SUBMITTING;
        auto accepted = co_await submission_->submit(cancellation_, batch_receipt());
        if (!accepted) cancellation_ = accepted.error();
        else if (submission_->complete()) finish();
        co_return accepted;
    }

    RenderResult<FrameOperation::EPreparationStatus>
    FrameOperation::prepareUniforms(
        std::span<RenderGraphInstance* const> instances, std::optional<UniformStateLookup> lookup
    ) noexcept
    {
        if (std::ranges::none_of(instances, [](const auto* instance) noexcept
        {
            return !instance->uniform_blocks_.empty();
        })) return EPreparationStatus::READY;
        if (!uploads_ || !owner_ || !lookup) return cxx::unexpected(RenderError{kDeviceUnsupported});
        if (!workspace_.uniform_completed.size()) workspace_.uniform_completed = device_.makeFrontiers();
        device_.readCompleted(workspace_.uniform_completed);
        bool ready = true;
        for (auto* instance : instances)
        {
            for (const auto& block : instance->uniform_blocks_)
            {
                auto& pool = owner_->uniformPool(block.pool);
                auto& versions = pool.versions;
                const auto found = std::ranges::find(workspace_.uniforms, block.pool, &SelectedUniformVersion::pool);
                UniformVersionId version;
                if (found != workspace_.uniforms.end())
                {
                    version = found->version;
                }
                else
                {
                    const auto* state = (*lookup)(pool.effect, pool.node, versions.stateType());
                    if (!state) return cxx::unexpected(RenderError{kRuntimeIdentity});
                    auto selected = versions.pin(state, workspace_.uniform_completed);
                    if (!selected)
                    {
                        if (selected.error().type != kDeviceBusy) return cxx::unexpected(selected.error());
                        ready = false;
                        continue;
                    }
                    version = *selected;
                    workspace_.uniforms.push_back({block.pool, version});
                }
                if (!versions.ready(version))
                {
                    auto staged = versions.stage(version, *uploads_);
                    if (!staged)
                    {
                        const bool can_retry = staged.error().type == kDeviceCapacity &&
                            (uploads_->hasFrameCopies() || device_.pendingGpuWork());
                        if (!can_retry) return cxx::unexpected(staged.error());
                    }
                    ready = false;
                    continue;
                }
                instance->setResource(block.resource.index, slot(), versions.binding(version));
                const auto upload_serial = versions.uploadSerial(version);
                if (upload_serial > device_.queue().completed()) submission_->waitForUniformUpload(upload_serial);
            }
        }
        return ready ? EPreparationStatus::READY : EPreparationStatus::AWAITING_PROGRESS;
    }

    std::pair<void*, const StorageBindingOps*> FrameOperation::frameStorage(ResourceInstanceId id) const noexcept
    {
        if (!owner_) return {};
        auto& resources = owner_->resources().registry();
        if (resources.deviceOf(id) != &device_) return {};
        const auto* factory = resources.factory(resources.declarationOf(id));
        return {resources.resourceInstance(id), factory ? factory->storage()->bindings() : nullptr};
    }

    RenderResult<FrameOperation::EPreparationStatus>
    FrameOperation::prepareStorages(std::span<const ResourceInstanceId> storages) noexcept
    {
        const auto frame = replicaFrame();
        auto& replicas = workspace_.replicas[frame.slot];
        bool resources_ready = true;
        replicas.resize(storages.size());
        for (std::size_t i = 0; i < storages.size(); ++i)
        {
            const auto id = storages[i];
            const auto [storage, ops] = frameStorage(id);
            if (!storage || !ops || !ops->prepareFrame || !ops->settleFrame)
                return cxx::unexpected(RenderError{kStorageContract});
            const auto* source_revision = ops->preparationRevision ? ops->preparationRevision(storage) : nullptr;
            const auto revision = source_revision ? *source_revision : 0;
            const auto& applied = replicas[i];
            const bool already_prepared = revision && applied.identity == id && applied.revision == revision;
            if (already_prepared) continue;

            const auto first = uploads_->stagedFrameCopyCount();
            auto& participant = workspace_.resource_participants.emplace_back(id, i, frame, ReplicaCopyRange{first, 0});
            auto prepared = ops->prepareFrame(storage, frame, *uploads_);
            participant.ranges.count = uploads_->stagedFrameCopyCount() - first;
            if (!prepared) return cxx::unexpected(prepared.error());
            resources_ready &= *prepared;
            if (*prepared) participant.revision = revision;
        }
        return resources_ready ? EPreparationStatus::READY : EPreparationStatus::AWAITING_PROGRESS;
    }

    RenderResult<FrameOperation::EPreparationStatus>
    FrameOperation::prepareInstances(std::span<RenderGraphInstance* const> instances) noexcept
    {
        while (prepared_instances_ < instances.size())
        {
            const auto prepared = instances[prepared_instances_]->prepareFrame(slot());
            if (!prepared)
            {
                return cxx::unexpected(prepared.error());
            }
            if (!*prepared)
            {
                return EPreparationStatus::AWAITING_PROGRESS;
            }
            if (device_.description().graphicsAPI == nri::GraphicsAPI::VK)
            {
                // This call has frozen a complete replica without suspending. Another replica may
                // freeze after a later pool replacement while the round waits for History.
                auto& descriptors = device_.descriptors();
                const auto selected = descriptors.nativeBindingSelection();
                auto& selections = workspace_.native_bindings;
                const bool any_pool = std::ranges::any_of(selected, [](const auto id) noexcept { return !id.isNull(); });
                if (any_pool && (selections.empty() || selections.back() != selected))
                {
                    descriptors.retainNativeBindings(selected);
                    selections.push_back(selected);
                }
            }
            ++prepared_instances_;
        }
        return EPreparationStatus::READY;
    }
} // namespace lux::render::detail

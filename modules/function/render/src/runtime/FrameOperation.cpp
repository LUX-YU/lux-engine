#include <lux/engine/render/device/SubmissionQueue.hpp>
#include <lux/engine/render/frame/FrameError.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <lux/engine/render/device/ResourceUploader.hpp>
#include <lux/engine/render/runtime/DeviceEntry.hpp>
#include <lux/engine/render/runtime/FrameOperation.hpp>
#include <lux/engine/render/runtime/JobExecution.hpp>
#include <lux/engine/render/device/JobGpuResources.hpp>

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
        : device_(device), uploads_(nullptr), progress_(progress), workspace_(workspace), cpu_(cpu), owner_(nullptr)
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
        phase_ = EPhase::RESOURCES;
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
        for (auto* job : workspace_.frozen_jobs)
        {
            job->frames_[slot()].replaced_projections.clear();
            job->frames_[slot()].frozen_for_submission = false;
        }
        const bool unknown = cancellation_ && cancellation_->type == kOutcomeUnknown;
        workspace_.buffers.finish(unknown);
        for (const auto& selection : workspace_.native_bindings)
            device_.descriptors().releaseNativeBindings(selection);
        workspace_.native_bindings.clear();
        submission_.reset();
        workspace_.frozen_jobs.clear();
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
        if (error.type && uploads_) uploads_->cancelFrameCopies();
        workspace_.buffers.settle(accepted, error);
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
        for (const auto& participant : workspace_.producer_participants)
        {
            const bool wrong_frame = participant.frame.slot != frame.slot || participant.frame.epoch != frame.epoch;
            if (!owner_ || wrong_frame) std::terminate();
            auto& gpu = owner_->jobResources(participant.binding);
            for (const auto& copy : std::span{workspace_.prepared_buffers}.subspan(participant.first, participant.count))
                gpu.settlePreparedBuffer(frame.slot, copy, locate(copy.ranges), error);
        }
        workspace_.producer_participants.clear();
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
            auto prepared = FrameOperation::flushFrameCopies(*uploads_);
            if (!prepared) co_return cxx::unexpected(prepared.error());
            copies_flushed_ = true;
        }
        if (cancellation_) co_return cxx::unexpected(*cancellation_);
        auto submitted = co_await FrameOperation::submitPreparationCopies(*uploads_);
        if (!submitted) co_return cxx::unexpected(submitted.error());
        if (!*submitted) co_return 0;
        const auto receipt = **submitted;
        submission_->acceptPreparation(receipt.point.serial);
        FrameOperation::acceptFrameCopies(*uploads_, receipt);
        settlePreparation(receipt.point, receipt.ranges);
        co_return 1;
    }

    exec::task<RenderResult<std::uint32_t>> FrameOperation::run(
        std::span<runtime_detail::JobExecution* const> executions, std::span<const ResourceInstanceId> storages,
        std::span<const SubmissionOrder> order
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

        if (phase_ == EPhase::SUBMITTING)
        {
            auto accepted = co_await submission_->submit(cancellation_);
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
        if (!storages.empty() && !uploads_)
        {
            cancellation_ = RenderError{kDeviceArgument};
            co_return cxx::unexpected(*cancellation_);
        }
        if (phase_ == EPhase::RESOURCES)
        {
            auto resources = prepareStorages(executions, storages);
            if (!resources) co_return cxx::unexpected(track(resources).error());
            if (uploads_ && !uploads_->hasFrameCopies()) settlePreparation();
            if (*resources == EPreparationStatus::READY) phase_ = EPhase::PRODUCER_SELECTIONS;
        }
        auto waiting = EExecutionOutcome::RESOURCE_PREPARATION_PENDING;
        if (phase_ == EPhase::PRODUCER_SELECTIONS)
        {
            for (auto* job : executions)
            {
                auto prepared = job->preparation.select(job->frames_[slot()], slot(), job->output_.description);
                if (!prepared)
                    co_return cxx::unexpected(track(prepared).error());
            }
            phase_ = EPhase::PRODUCER_UPLOADS;
        }
        if (phase_ == EPhase::PRODUCER_UPLOADS)
        {
            bool ready = true;
            PreparedUploadWindow window{device_, uploads_, workspace_.prepared_buffers};
            for (auto* job : executions)
            {
                const auto first = workspace_.prepared_buffers.size();
                auto staged = window.stageFrame(job->frames_[slot()], job->graph(), job->preparation.bufferInputs());
                const auto count = workspace_.prepared_buffers.size() - first;
                if (count)
                {
                    workspace_.producer_participants.push_back({
                        job->binding_identity, replicaFrame(), first, count
                    });
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
            auto prepared = freezeJobs(executions);
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
            for (auto* job : executions)
                ++job->progress.counts[static_cast<std::size_t>(waiting)];
            co_return 0;
        }
        waiting_for_gpu_ = false;
        // Every graph now has a frozen view of its FIF replica. Workers own disjoint allocator/CB pairs.
        settlePreparation();
        if (uploads_) uploads_->reservePreparationStaging(false);
        if (auto prepared = submission_->prepare(workspace_.frozen_jobs, order); !prepared)
            co_return cxx::unexpected(track(prepared).error());
        phase_ = EPhase::RECORDING;
        RenderResult<void> recorded;
        if (!workers_)
            recorded = co_await submission_->record(cpu_, true);
        else if (workers_->compilations_)
            recorded = co_await submission_->record(workers_->independent_, false);
        else
            recorded = co_await workers_->recordings_.nest(submission_->record(cpu_, true));
        if (!recorded) co_return cxx::unexpected(track(recorded).error());
        phase_ = EPhase::SUBMITTING;
        auto accepted = co_await submission_->submit(cancellation_);
        if (!accepted) cancellation_ = accepted.error();
        else if (submission_->complete()) finish();
        co_return accepted;
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
    FrameOperation::prepareStorages(
        std::span<runtime_detail::JobExecution* const> executions, std::span<const ResourceInstanceId> storages
    ) noexcept
    {
        const auto frame = replicaFrame();
        auto& replicas = workspace_.replicas[frame.slot];
        bool resources_ready = true;
        bool sampled_completion = false;
        for (auto* job : executions)
        {
            for (const auto& input : job->preparation.constantBuffers())
            {
                if (!uploads_) return cxx::unexpected(RenderError{kDeviceUnsupported});
                if (!sampled_completion)
                {
                    if (!workspace_.blocked_completion.size()) workspace_.blocked_completion = device_.makeFrontiers();
                    device_.readCompleted(workspace_.blocked_completion);
                    sampled_completion = true;
                }
                auto selected = workspace_.buffers.prepare(input, workspace_.blocked_completion, *uploads_);
                if (!selected)
                {
                    const bool retry = selected.error().type == kDeviceCapacity &&
                        (uploads_->hasFrameCopies() || device_.pendingGpuWork());
                    if (!retry) return cxx::unexpected(selected.error());
                }
                if (!selected || !*selected)
                {
                    resources_ready = false;
                    continue;
                }
                job->frames_[frame.slot].bindResource(job->graph(), device_, input.resource.index, **selected);
            }
        }
        if (!resources_ready) return EPreparationStatus::AWAITING_PROGRESS;
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
    FrameOperation::freezeJobs(std::span<runtime_detail::JobExecution* const> executions) noexcept
    {
        auto& frozen = workspace_.frozen_jobs;
        while (frozen.size() < executions.size())
        {
            auto& job = *executions[frozen.size()];
            const auto prepared = job.freezeFrame(slot());
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
            job.frames_[slot()].frozen_for_submission = true;
            frozen.push_back(&job);
        }
        return EPreparationStatus::READY;
    }
} // namespace lux::render::detail

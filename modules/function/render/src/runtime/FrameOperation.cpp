#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>
#include <lux/engine/render/graph/execute/RenderGraphInstance.hpp>
#include <lux/engine/render/runtime/FrameOperation.hpp>

namespace lux::render::detail
{
    FrameOperation::Workspace::Workspace(
        std::uint32_t slots, std::size_t queues, std::size_t preparation_capacity
    ) noexcept
        : submissions(slots, queues), preparation(preparation_capacity), replicas(slots)
    {
    }

    FrameOperation::FrameOperation(
        DeviceSession& device,
        ResourceUploader* uploads,
        FrameProgress& progress,
        Workspace& workspace,
        execution::CpuScheduler cpu
    ) noexcept
        : device_(device), uploads_(uploads), progress_(progress), workspace_(workspace), cpu_(cpu)
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
        auto opened = progress_.tryBegin(device_, round_);
        if (opened && *opened)
        {
            prepared_instances_ = 0;
            resources_ready_ = false;
            draw_data_ready_ = false;
            waiting_for_gpu_ = false;
            submission_.emplace(device_, round_, workspace_.submissions);
            if (uploads_) uploads_->reservePreparationStaging(true);
        }
        return opened;
    }

    bool FrameOperation::canRun() const noexcept
    {
        if (!active() || worker_pending_) return false;
        if (cancellation_ || device_.terminalError().type) return true;
        if (submission_->prepared()) return submission_->canSubmit();
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
        submission_.reset();
        round_.finish();
        cancellation_.reset();
        copies_flushed_ = false;
        if (uploads_) uploads_->reservePreparationStaging(false);
    }

    void FrameOperation::settlePreparation(RenderResult<void> result) noexcept
    {
        if (!result)
        {
            for (auto& applied : workspace_.replicas[slot()])
            {
                applied.revision = 0;
            }
        }
        if (uploads_) uploads_->finishFrameCopySubmission(result);
        workspace_.preparation.finishCallbacks(slot(), result);
        copies_flushed_ = false;
    }

    void FrameOperation::cancel(RenderError error) noexcept
    {
        if (!active()) return;
        if (worker_pending_)
        {
            if (!cancellation_) cancellation_ = error;
            return;
        }
        settlePreparation(cxx::unexpected(error));
        submission_->rejectSuffix(error);
        finish();
    }

    exec::task<RenderResult<std::uint32_t>> FrameOperation::submitPreparation() noexcept
    {
        if (!copies_flushed_)
        {
            auto prepared = co_await uploads_->prepareFrameCopies();
            if (!prepared) co_return cxx::unexpected(prepared.error());
            copies_flushed_ = true;
        }
        if (cancellation_) co_return cxx::unexpected(*cancellation_);
        auto serial = co_await uploads_->submitPreparationCopies();
        if (!serial) co_return cxx::unexpected(serial.error());
        if (*serial != 0)
        {
            submission_->acceptPreparation(*serial);
            settlePreparation({});
        }
        co_return *serial != 0 ? 1u : 0u;
    }

    exec::task<RenderResult<std::uint32_t>> FrameOperation::run(
        std::span<RenderGraphInstance* const> instances, std::span<const StorageFrameUse> storages,
        std::span<const SubmissionOrder> order
    ) noexcept
    {
        if (!active() || worker_pending_)
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

        if (submission_->prepared())
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
        if (!resources_ready_)
        {
            auto resources = prepareStorages(storages);
            if (!resources) co_return cxx::unexpected(track(resources).error());
            resources_ready_ = *resources;
        }
        auto waiting = EExecutionOutcome::RESOURCE_PREPARATION_PENDING;
        if (resources_ready_ && !draw_data_ready_)
        {
            for (auto* instance : instances)
            {
                if (!instance->hasData()) continue;
                if (!uploads_)
                {
                    cancellation_ = RenderError{kDeviceArgument};
                    co_return cxx::unexpected(*cancellation_);
                }
                auto prepared = instance->prepareData(slot(), *uploads_, workspace_.preparation);
                if (!prepared || !*prepared)
                {
                    cancellation_ = prepared ? RenderError{kDeviceCapacity} : prepared.error();
                    co_return cxx::unexpected(*cancellation_);
                }
            }
            auto draws = prepareDrawLists(instances);
            if (!draws || !*draws)
            {
                cancellation_ = draws ? RenderError{kDeviceCapacity} : draws.error();
                co_return cxx::unexpected(*cancellation_);
            }
            draw_data_ready_ = true;
        }
        if (uploads_ && uploads_->hasFrameCopies())
        {
            auto copied = co_await submitPreparation();
            co_return track(copied);
        }
        bool ready = draw_data_ready_;
        if (ready)
        {
            auto prepared = prepareInstances(instances);
            if (!prepared) co_return cxx::unexpected(track(prepared).error());
            ready = *prepared;
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
        settlePreparation({});
        if (uploads_) uploads_->reservePreparationStaging(false);
        if (auto prepared = submission_->prepare(instances, order); !prepared)
            co_return cxx::unexpected(track(prepared).error());
        auto recorded = co_await submission_->record(cpu_);
        if (!recorded) co_return cxx::unexpected(track(recorded).error());
        auto accepted = co_await submission_->submit(cancellation_);
        if (!accepted) cancellation_ = accepted.error();
        else if (submission_->complete()) finish();
        co_return accepted;
    }

    RenderResult<bool> FrameOperation::prepareStorages(std::span<const StorageFrameUse> storages) noexcept
    {
        auto& replicas = workspace_.replicas[slot()];
        bool resources_ready = true;
        replicas.resize(storages.size());
        for (std::size_t i = 0; i < storages.size(); ++i)
        {
            const auto& use = storages[i];
            auto& applied = replicas[i];
            const auto revision = use.preparation_revision ? *use.preparation_revision : 0;
            const bool already_prepared = revision != 0 && !use.identity.isNull() && applied.identity == use.identity &&
                                          applied.revision == revision;
            if (already_prepared)
            {
                continue;
            }
            auto prepared = use.ops->prepareFrame(use.storage, slot(), *uploads_, workspace_.preparation);
            if (!prepared)
            {
                return cxx::unexpected(prepared.error());
            }
            resources_ready &= *prepared;
            if (*prepared)
            {
                applied = {use.identity, revision};
            }
        }
        return resources_ready;
    }

    RenderResult<bool> FrameOperation::prepareDrawLists(std::span<RenderGraphInstance* const> instances) noexcept
    {
        bool ready = true;
        for (auto* instance : instances)
        {
            if (instance->frames_[slot()].draw_lists.empty())
            {
                continue;
            }
            if (!uploads_)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            auto prepared = instance->prepareDrawListUploads(slot(), *uploads_);
            // No staging progress needs no submission callback. Terminal frame delivery also
            // settles unprepared sources, including frames with no active output.
            if (!prepared || uploads_->hasFrameCopies())
            {
                workspace_.preparation.onSubmitted(
                    instance,
                    [](void* value, std::uint32_t slot, RenderResult<void> result) noexcept
                    { static_cast<RenderGraphInstance*>(value)->finishDrawListUploads(slot, result); }
                );
            }
            if (!prepared)
            {
                return cxx::unexpected(prepared.error());
            }
            ready &= *prepared;
        }
        return ready;
    }

    RenderResult<bool> FrameOperation::prepareInstances(std::span<RenderGraphInstance* const> instances) noexcept
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
                return false;
            }
            ++prepared_instances_;
        }
        return true;
    }
} // namespace lux::render::detail

#include <algorithm>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/execution/FrameExecution.hpp>
#include <lux/engine/render/execution/FrameResources.hpp>
#include <lux/engine/render/execution/RenderExecutor.hpp>
#include <lux/engine/render/execution/RenderGraphInstance.hpp>
#include <lux/engine/render/features/FeatureBinding.hpp>
#include <lux/engine/render/graph/DumpText.hpp>
#include <lux/engine/render/graph/Range.hpp>
#include <lux/engine/render/upload/ResourceUploader.hpp>

namespace lux::render::detail
{
    FrameExecution::FrameExecution(DeviceSession& device, ResourceUploader* uploads, std::uint32_t count) noexcept
        : device_(device), uploads_(uploads), frame_count_(count)
    {
        preparation_receivers_.reserve(uploads ? uploads->windowRegionLimit() : 0);
    }

    RenderResult<std::unique_ptr<FrameExecution>> FrameExecution::create(
        DeviceSession& device,
        ResourceUploader* uploads,
        std::uint32_t count
    ) noexcept
    {
        if (count == 0 || count > 3)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        return std::unique_ptr<FrameExecution>(new FrameExecution(device, uploads, count));
    }

    FrameExecution::~FrameExecution() noexcept
    {
        if (frame_waiters_)
        {
            std::terminate();
        }
    }

    bool FrameExecution::tryBeginWrite(std::uint32_t& slot) noexcept
    {
        slot = next_frame_;
        auto& frame = frames_[slot];
        if (frame.phase == EFramePhase::FREE)
        {
            frame.accepted.serials.fill(0);
            frame.phase = EFramePhase::COLLECTING;
        }
        if (frame.phase != EFramePhase::COLLECTING)
        {
            return false;
        }
        ++frame.pending_writes;
        updateUploadAdmission();
        return true;
    }

    void FrameExecution::finishWrite(std::uint32_t slot, const SubmissionFrontiers& accepted) noexcept
    {
        auto& frame = frames_[slot];
        for (std::size_t i = 0; i < device_.queueCount(); ++i)
        {
            frame.accepted.serials[i] = std::max(frame.accepted.serials[i], accepted.serials[i]);
        }
        --frame.pending_writes;
        updateUploadAdmission();
    }

    void FrameExecution::enqueueWrite(WriteWaiter& node) noexcept
    {
        node.next = nullptr;
        if (frame_waiters_tail_)
        {
            frame_waiters_tail_->next = &node;
        }
        else
        {
            frame_waiters_ = &node;
        }
        frame_waiters_tail_ = &node;
    }

    void FrameExecution::updateUploadAdmission() noexcept
    {
        if (uploads_)
        {
            const auto& frame = frames_[next_frame_];
            const bool preparing = frame.phase == EFramePhase::PREPARING || frame.phase == EFramePhase::SUBMITTING;
            uploads_->setBackgroundTransfersAllowed(!preparing && frame.pending_writes == 0);
        }
    }

    void FrameExecution::dispatchWrites() noexcept
    {
        const auto phase = frames_[next_frame_].phase;
        const bool can_write = phase == EFramePhase::FREE || phase == EFramePhase::COLLECTING;
        auto* frame_boundary = frame_waiters_tail_;
        while (frame_waiters_ && frame_boundary)
        {
            auto* node = frame_waiters_;
            if (!can_write && !node->stopping(node->operation))
            {
                break;
            }
            frame_waiters_ = node->next;
            if (!frame_waiters_)
            {
                frame_waiters_tail_ = nullptr;
            }
            if (node == frame_boundary)
            {
                frame_boundary = nullptr;
            }
            const auto complete = node->complete;
            auto* operation = node->operation;
            complete(operation);
        }
    }

    void FrameExecution::onPreparationSubmitted(
        void* receiver,
        void (*complete)(void*, std::uint32_t, RenderResult<void>) noexcept
    ) noexcept
    {
        preparation_receivers_.emplace_back(receiver, complete);
    }

    void FrameExecution::acceptSubmission(std::uint32_t slot, const SubmissionQueue& queue) noexcept
    {
        for (std::size_t i = 0; i < device_.queueCount(); ++i)
        {
            if (&device_.physicalQueue(i) == &queue)
            {
                frames_[slot].accepted.serials[i] = std::max(frames_[slot].accepted.serials[i], queue.submitted());
                return;
            }
        }
    }

    void FrameExecution::finishPreparation(RenderResult<void> result) noexcept
    {
        if (uploads_)
        {
            uploads_->completeFrameCopies(result);
        }
        const auto boundary = preparation_receivers_.size();
        const auto slot = next_frame_;
        for (std::size_t i = 0; i < boundary; ++i)
        {
            // Remove this borrow before delivery. A callback may append work for the next
            // submission and grow the vector; no iterator or record reference crosses it.
            const auto [receiver, complete] = std::exchange(preparation_receivers_[i], {});
            complete(receiver, slot, result);
        }
        preparation_receivers_.erase(preparation_receivers_.begin(), preparation_receivers_.begin() + boundary);
    }

    void FrameExecution::sealFrame(FrameSlot& frame) noexcept
    {
        frame.phase = EFramePhase::SUBMITTED;
        frame.pending_queues = 0;
        for (auto serial : frame.accepted.serials)
        {
            frame.pending_queues += serial != 0;
        }
        if (frame.pending_queues == 0)
        {
            frame.phase = EFramePhase::FREE;
            return;
        }
        for (std::size_t i = 0; i < device_.queueCount(); ++i)
        {
            if (frame.accepted.serials[i] == 0)
            {
                continue;
            }
            device_.physicalQueue(i).arm(
                frame.accepted.serials[i],
                frame.completions[i],
                [](void* value, RenderResult<void>) noexcept
                {
                    auto& frame = *static_cast<FrameSlot*>(value);
                    if (--frame.pending_queues == 0)
                    {
                        frame.phase = EFramePhase::FREE;
                    }
                },
                &frame
            );
        }
    }

    RenderResult<std::uint32_t> FrameExecution::render(
        std::span<RenderGraphInstance* const> instances,
        std::span<const StorageFrameUse> storages,
        std::span<const std::pair<RenderGraphInstance*, FeatureBinding*>> features
    ) noexcept
    {
        if (uploads_)
        {
            uploads_->reserveFrameStaging(!instances.empty());
        }
        cxx::scope_exit update_admission([this]() noexcept { updateUploadAdmission(); });
        auto& frame = frames_[next_frame_];
        if (frame.phase == EFramePhase::SUBMITTING)
        {
            return advanceSubmissions();
        }
        if (instances.empty() && frame.phase == EFramePhase::FREE)
        {
            return 0;
        }
        auto outcome = [&](EExecutionOutcome value) noexcept -> RenderResult<std::uint32_t>
        {
            for (auto* instance : instances)
            {
                ++instance->progress_.counts[static_cast<std::size_t>(value)];
            }
            return 0;
        };
        if (frame.phase == EFramePhase::SUBMITTED)
        {
            return outcome(EExecutionOutcome::PREVIOUS_SUBMISSION_PENDING);
        }
        if (frame.phase == EFramePhase::FREE)
        {
            frame.accepted.serials.fill(0);
        }
        frame.phase = EFramePhase::PREPARING;
        if (frame.pending_writes != 0)
        {
            return outcome(EExecutionOutcome::RESOURCE_PREPARATION_PENDING);
        }
        if (instances.empty())
        {
            sealFrame(frame);
            next_frame_ = (next_frame_ + 1) % frame_count_;
            return 0;
        }
        auto& graphics = device_.queue();
        if (graphics.recording())
        {
            return outcome(EExecutionOutcome::RECORDING_BUSY);
        }
        if (!graphics.hasCapacity())
        {
            return outcome(EExecutionOutcome::QUEUE_FULL);
        }
        // This synchronous phase may fill staging and update descriptors, but may not
        // submit, suspend or dispatch callbacks. Only the following recording segment owns a CommandBuffer.
        if ((!storages.empty() || !features.empty()) && !uploads_)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        bool resources_ready = true;
        for (const auto& use : storages)
        {
            auto prepared = use.ops->prepareFrame(use.storage, next_frame_, *uploads_);
            if (!prepared)
            {
                finishPreparation(cxx::unexpected(prepared.error()));
                sealFrame(frame);
                return cxx::unexpected(prepared.error());
            }
            resources_ready &= *prepared;
        }
        bool instances_ready = resources_ready;
        auto waiting = EExecutionOutcome::RESOURCE_PREPARATION_PENDING;
        bool native_records = false;
        if (resources_ready)
        {
            for (const auto& [instance, binding] : features)
            {
                auto prepared = binding->prepare(binding->value.get(), next_frame_, *uploads_);
                if (!prepared)
                {
                    finishPreparation(cxx::unexpected(prepared.error()));
                    sealFrame(frame);
                    return cxx::unexpected(prepared.error());
                }
                if (prepared->changed_resource)
                {
                    instance->setResource(binding->resource.index, next_frame_, *prepared->changed_resource);
                }
                instances_ready &= prepared->state != FeatureBinding::EPreparation::WAITING;
                binding->prepared_copies = prepared->state == FeatureBinding::EPreparation::RECORD
                                               ? prepared->copies
                                               : std::span<const ResourceUploader::BufferCopy>{};
                native_records |= !binding->prepared_copies.empty();
            }
        }
        if (instances_ready)
        {
            for (auto* instance : instances)
            {
                const auto prepared = instance->prepareFrame(next_frame_);
                if (!prepared)
                {
                    finishPreparation(cxx::unexpected(prepared.error()));
                    sealFrame(frame);
                    return cxx::unexpected(prepared.error());
                }
                if (!*prepared)
                {
                    waiting = EExecutionOutcome::HISTORY_PENDING;
                    instances_ready = false;
                    break;
                }
            }
        }
        const bool has_copies = uploads_ && uploads_->hasFrameCopies();
        if (!instances_ready && !has_copies && !native_records)
        {
            finishPreparation(cxx::unexpected(RenderError{kDeviceBusy}));
            return outcome(waiting);
        }
        if (instances_ready)
        {
            frame.feature_copies.clear();
            for (const auto& [instance, binding] : features)
            {
                frame.feature_copies.insert(
                    frame.feature_copies.end(),
                    binding->prepared_copies.begin(),
                    binding->prepared_copies.end()
                );
                binding->prepared_copies = {};
            }
            prepareSubmissions(instances);
            return advanceSubmissions();
        }
        auto accepted = graphics.recordAndSubmit(
            [&](nri::CommandBuffer& command, std::uint32_t) noexcept -> RenderResult<void>
            {
                if (uploads_)
                {
                    uploads_->recordFrameCopies(command);
                }
                if (resources_ready)
                {
                    for (const auto& [instance, binding] : features)
                    {
                        uploads_->recordCopies(command, binding->prepared_copies);
                        binding->prepared_copies = {};
                    }
                }
                return {};
            }
        );
        if (!accepted)
        {
            finishPreparation(accepted);
            sealFrame(frame);
            return cxx::unexpected(accepted.error());
        }
        acceptSubmission(next_frame_, graphics);
        frame.graphics_serial = graphics.submitted();
        frame.recorded_instances = 0;
        frame.recorded_passes = 0;
        finishPreparation({});
        (void)outcome(waiting);
        return 1; // A preparation submission does not publish an instance or advance its history.
    }

    std::size_t FrameExecution::queueIndex(const SubmissionQueue& queue) const noexcept
    {
        for (std::size_t i = 0; i < device_.queueCount(); ++i)
        {
            if (&device_.physicalQueue(i) == &queue)
            {
                return i;
            }
        }
        std::terminate();
    }

    bool FrameExecution::submitting(const RenderGraphInstance& instance) const noexcept
    {
        const auto& frame = frames_[next_frame_];
        return frame.phase == EFramePhase::SUBMITTING &&
               std::ranges::any_of(
                   frame.instances,
                   [&](const auto& execution) { return execution.instance == &instance; }
               );
    }

    void FrameExecution::prepareSubmissions(std::span<RenderGraphInstance* const> instances) noexcept
    {
        auto& frame = frames_[next_frame_];
        frame.instances.resize(instances.size());
        frame.recording_instances.reserve(instances.size());
        frame.recorded_instances = frame.recorded_passes = 0;
        frame.preparation_serial = 0;
        for (std::uint32_t i = 0; i < instances.size(); ++i)
        {
            auto& execution = frame.instances[i];
            execution.instance = instances[i];
            execution.next_batch = 0;
            execution.serials.assign(instances[i]->plan().submissions().size(), 0);
            execution.accepted = {};
            execution.predecessors.clear();
        }
        buildImportedDependencies(instances);
        frame.phase = EFramePhase::SUBMITTING;
    }

    void FrameExecution::buildImportedDependencies(std::span<RenderGraphInstance* const> instances) noexcept
    {
        auto& frame = frames_[next_frame_];
        frame.imported_uses.clear();
        if (instances.size() < 2)
        {
            return;
        }
        for (std::uint32_t i = 0; i < instances.size(); ++i)
        {
            const auto& bindings = instances[i]->frames_[next_frame_].bindings;
            for (const auto& use : instances[i]->plan_->imported_accesses_)
            {
                const auto& binding = bindings[use.resource.index];
                auto range = use.range;
                if (auto* buffer = std::get_if<GraphBufferRange>(&range))
                {
                    buffer->offset += binding.buffer_offset;
                }
                frame.imported_uses.push_back(
                    {binding.image ? static_cast<const void*>(binding.image) : binding.buffer, range, i, use.write}
                );
            }
        }
        std::ranges::sort(
            frame.imported_uses,
            [](const auto& a, const auto& b)
            {
                if (a.physical != b.physical)
                {
                    return std::less<const void*>{}(a.physical, b.physical);
                }
                return a.instance < b.instance;
            }
        );
        for (std::size_t first = 0; first < frame.imported_uses.size();)
        {
            auto end = first + 1;
            while (end < frame.imported_uses.size() &&
                   frame.imported_uses[end].physical == frame.imported_uses[first].physical)
            {
                ++end;
            }
            const auto group = std::span(frame.imported_uses).subspan(first, end - first);
            if (std::ranges::any_of(group, &ImportedUse::write))
            {
                for (std::size_t a = 0; a < group.size(); ++a)
                {
                    for (std::size_t b = 0; b < a; ++b)
                    {
                        const bool conflict = group[a].instance != group[b].instance &&
                                              (group[a].write || group[b].write) &&
                                              overlaps(group[a].range, group[b].range);
                        if (conflict)
                        {
                            frame.instances[group[a].instance].predecessors.push_back(group[b].instance);
                        }
                    }
                }
            }
            first = end;
        }
        for (auto& execution : frame.instances)
        {
            std::ranges::sort(execution.predecessors);
            execution.predecessors.erase(
                std::unique(execution.predecessors.begin(), execution.predecessors.end()),
                execution.predecessors.end()
            );
        }
        frame.imported_uses.clear();
    }

    bool FrameExecution::submitting() const noexcept
    {
        return frames_[next_frame_].phase == EFramePhase::SUBMITTING;
    }

    void FrameExecution::cancelSubmissions(RenderError error) noexcept
    {
        auto& frame = frames_[next_frame_];
        if (frame.phase != EFramePhase::SUBMITTING)
        {
            if (frame.phase == EFramePhase::COLLECTING || frame.phase == EFramePhase::PREPARING)
            {
                // Unsubmitted replica copies can still have Sender receivers waiting for acceptance.
                // Accepted uploads retain their existing GPU completion and finishWrite paths.
                finishPreparation(cxx::unexpected(error));
            }
            return;
        }
        if (frame.preparation_serial == 0)
        {
            finishPreparation(cxx::unexpected(error));
        }
        for (auto& execution : frame.instances)
        {
            if (execution.instance && execution.next_batch != 0)
            {
                execution.instance->failedSubmission(error);
            }
            execution.instance = nullptr;
        }
        frame.feature_copies.clear();
        sealFrame(frame);
        next_frame_ = (next_frame_ + 1) % frame_count_;
        updateUploadAdmission();
    }

    bool FrameExecution::collectBatchWaits(
        const InstanceExecution& execution,
        SubmissionFrontiers& waits,
        std::array<nri::StageBits, 3>& stages
    ) const noexcept
    {
        const auto merge = [&](std::size_t queue, std::uint64_t serial, nri::StageBits stage)
        {
            if (waits.serials[queue] == 0)
            {
                stages[queue] = stage;
            }
            else
            {
                const bool has_all_stages = stages[queue] == nri::StageBits::ALL || stage == nri::StageBits::ALL;
                stages[queue] = has_all_stages ? nri::StageBits::ALL : stages[queue] | stage;
            }
            waits.serials[queue] = std::max(waits.serials[queue], serial);
        };
        const auto& frame = frames_[next_frame_];
        for (auto previous : execution.predecessors)
        {
            const auto& predecessor = frame.instances[previous];
            if (predecessor.instance)
            {
                return false;
            }
            for (std::size_t q = 0; q < device_.queueCount(); ++q)
            {
                merge(q, predecessor.accepted.serials[q], nri::StageBits::ALL);
            }
        }
        const auto batches = execution.instance->plan().submissions();
        for (const auto& dependency : batches[execution.next_batch].dependencies)
        {
            auto& producer = device_.queue(batches[dependency.producer].queue);
            const auto q = queueIndex(producer);
            merge(q, execution.serials[dependency.producer], dependency.stages);
        }
        return true;
    }

    RenderResult<std::uint32_t> FrameExecution::advanceSubmissions() noexcept
    {
        auto& frame = frames_[next_frame_];
        std::uint32_t submissions = 0;
        bool advanced = true;
        while (advanced)
        {
            advanced = false;
            for (std::uint32_t i = 0; i < frame.instances.size(); ++i)
            {
                auto& first = frame.instances[i];
                if (!first.instance)
                {
                    continue;
                }
                const auto& batch = first.instance->plan().submissions()[first.next_batch];
                auto& queue = device_.queue(batch.queue);
                if (queue.recording() || !queue.hasCapacity())
                {
                    continue;
                }
                const auto queue_index = queueIndex(queue);
                SubmissionFrontiers waits;
                std::array<nri::StageBits, 3> stages{};
                if (!collectBatchWaits(first, waits, stages))
                {
                    continue;
                }
                waits.serials[queue_index] = 0; // Same physical queue is ordered by submission.
                stages[queue_index] = nri::StageBits::ALL;
                frame.recording_instances.clear();
                frame.recording_instances.push_back(i);
                for (auto other = i + 1; other < frame.instances.size(); ++other)
                {
                    auto& candidate = frame.instances[other];
                    if (!candidate.instance ||
                        &device_.queue(candidate.instance->plan().submissions()[candidate.next_batch].queue) != &queue)
                    {
                        continue;
                    }
                    SubmissionFrontiers required;
                    std::array<nri::StageBits, 3> required_stages{};
                    if (collectBatchWaits(candidate, required, required_stages))
                    {
                        required.serials[queue_index] = 0;
                        required_stages[queue_index] = nri::StageBits::ALL;
                        if (required.serials == waits.serials && required_stages == stages)
                        {
                            frame.recording_instances.push_back(other);
                        }
                    }
                }
                std::array<nri::FenceSubmitDesc, 3> fences{};
                std::uint32_t fence_count = 0;
                for (std::size_t q = 0; q < device_.queueCount(); ++q)
                {
                    if (waits.serials[q] == 0)
                    {
                        continue;
                    }
                    auto dependency = device_.physicalQueue(q).dependency(waits.serials[q], stages[q]);
                    if (!dependency)
                    {
                        cancelSubmissions(dependency.error());
                        return cxx::unexpected(dependency.error());
                    }
                    fences[fence_count++] = *dependency;
                }
                const bool preparation = frame.preparation_serial == 0;
                auto accepted = queue.recordAndSubmit(
                    [&](nri::CommandBuffer& command, std::uint32_t) noexcept -> RenderResult<void>
                    {
                        if (preparation && uploads_)
                        {
                            uploads_->recordFrameCopies(command);
                            uploads_->recordCopies(command, frame.feature_copies);
                        }
                        for (auto selected : frame.recording_instances)
                        {
                            auto& execution = frame.instances[selected];
                            RenderExecutor::recordBatch(
                                *execution.instance,
                                command,
                                next_frame_,
                                execution.next_batch
                            );
                        }
                        return {};
                    },
                    std::span(fences).first(fence_count)
                );
                if (!accepted)
                {
                    cancelSubmissions(accepted.error());
                    return cxx::unexpected(accepted.error());
                }
                const auto serial = queue.submitted();
                acceptSubmission(next_frame_, queue);
                if (&queue == &device_.queue())
                {
                    frame.graphics_serial = serial;
                }
                for (auto selected : frame.recording_instances)
                {
                    auto& execution = frame.instances[selected];
                    execution.serials[execution.next_batch] = serial;
                    execution.accepted.serials[queue_index] = serial;
                    execution.instance->acceptedBatch(execution.next_batch, queue_index, serial);
                    if (++execution.next_batch == execution.serials.size())
                    {
                        frame.recorded_passes += execution.instance->recordedPassCount(next_frame_);
                        execution.instance->submitted(next_frame_);
                        execution.instance = nullptr;
                        ++frame.recorded_instances;
                    }
                }
                if (preparation)
                {
                    frame.preparation_serial = serial;
                    frame.feature_copies.clear();
                    finishPreparation({});
                }
                ++submissions;
                advanced = true;
                // Reconsider the whole ready set after a merged submission. Advancing a later
                // member again in this sweep would split otherwise identical instance batches.
                break;
            }
        }
        if (std::ranges::none_of(frame.instances, [](const auto& execution) { return execution.instance != nullptr; }))
        {
            sealFrame(frame);
            next_frame_ = (next_frame_ + 1) % frame_count_;
        }
        else if (submissions == 0)
        {
            for (const auto& execution : frame.instances)
            {
                if (execution.instance)
                {
                    const auto queue = execution.instance->plan().submissions()[execution.next_batch].queue;
                    const auto reason = device_.queue(queue).recording() ? EExecutionOutcome::RECORDING_BUSY
                                                                         : EExecutionOutcome::QUEUE_FULL;
                    ++execution.instance->progress_.counts[static_cast<std::size_t>(reason)];
                }
            }
        }
        return submissions;
    }

    bool FrameExecution::dumpSubmissions(DumpSink sink) const noexcept
    {
        if (!sink.write)
        {
            return false;
        }
        std::string output = "{\"kind\":\"accepted graph batches per FIF slot\",\"slots\":[";
        for (std::uint32_t i = 0; i < frame_count_; ++i)
        {
            if (i)
            {
                output += ',';
            }
            const auto& frame = frames_[i];
            output += "{\"slot\":" + std::to_string(i) + ",\"serial\":" + std::to_string(frame.graphics_serial) +
                      ",\"phase\":" + std::to_string(static_cast<unsigned>(frame.phase)) +
                      ",\"preparation_serial\":" + std::to_string(frame.preparation_serial) +
                      ",\"instances\":" + std::to_string(frame.recorded_instances) +
                      ",\"passes\":" + std::to_string(frame.recorded_passes) + ",\"accepted_frontiers\":[";
            for (std::size_t queue = 0; queue < frame.accepted.serials.size(); ++queue)
            {
                if (queue)
                {
                    output += ',';
                }
                output += std::to_string(frame.accepted.serials[queue]);
            }
            output += "],\"batch_serials\":[";
            for (std::size_t instance = 0; instance < frame.instances.size(); ++instance)
            {
                if (instance)
                {
                    output += ',';
                }
                output += '[';
                for (std::size_t batch = 0; batch < frame.instances[instance].serials.size(); ++batch)
                {
                    if (batch)
                    {
                        output += ',';
                    }
                    output += std::to_string(frame.instances[instance].serials[batch]);
                }
                output += ']';
            }
            output += "]}";
        }
        output += "]}\n";
        return sink.write(sink.destination, output);
    }

    RenderResult<bool> FrameExecution::waitForProgress(std::uint64_t timeout_ns) noexcept
    {
        const auto count = device_.queueCount();
        const auto& frame = frames_[next_frame_];
        if (frame.phase == EFramePhase::SUBMITTED)
        {
            for (std::size_t offset = 0; offset < count; ++offset)
            {
                const auto index = (wait_queue_cursor_ + offset) % count;
                auto& queue = device_.physicalQueue(index);
                const auto serial = frame.accepted.serials[index];
                if (serial > queue.completed())
                {
                    wait_queue_cursor_ = static_cast<std::uint8_t>((index + 1) % count);
                    return queue.wait(serial, timeout_ns);
                }
            }
        }
        // No submitted frame is blocking reuse. Rotate through the actual Queue frontiers
        // so background uploads and cleanup do not always wait behind physical queue zero.
        for (std::size_t offset = 0; offset < count; ++offset)
        {
            const auto index = (wait_queue_cursor_ + offset) % count;
            auto& queue = device_.physicalQueue(index);
            if (!queue.idle())
            {
                wait_queue_cursor_ = static_cast<std::uint8_t>((index + 1) % count);
                return queue.wait(queue.completed() + 1, timeout_ns);
            }
        }
        return false;
    }

} // namespace lux::render::detail

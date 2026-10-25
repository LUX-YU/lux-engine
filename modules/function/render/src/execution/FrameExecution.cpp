#include <algorithm>
#include <lux/engine/render/execution/BindingState.hpp>
#include <lux/engine/render/execution/FrameResources.hpp>
#include <lux/engine/render/execution/RenderExecutor.hpp>
#include <lux/engine/render/execution/RenderGraphInstance.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/extensions/execution/FrameExecution.hpp>
#include <lux/engine/render/extensions/features/FeatureBinding.hpp>
#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>
#include <lux/engine/render/graph/DumpText.hpp>
#include <lux/engine/render/graph/Range.hpp>
#include <tuple>

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
            frame.submitted_batches = 0;
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

    bool FrameExecution::beginFrame(std::uint32_t& slot) noexcept
    {
        auto& frame = frames_[next_frame_];
        if (frame.phase != EFramePhase::FREE)
        {
            return false;
        }
        slot = next_frame_;
        frame.accepted.serials.fill(0);
        frame.submitted_batches = 0;
        frame.phase = EFramePhase::COLLECTING;
        updateUploadAdmission();
        return true;
    }

    bool FrameExecution::inputConsumed(std::uint32_t slot) const noexcept
    {
        const auto phase = frames_[slot].phase;
        return phase == EFramePhase::FREE || phase == EFramePhase::SUBMITTED;
    }

    std::uint32_t FrameExecution::submittedBatches(std::uint32_t slot) const noexcept
    {
        return frames_[slot].submitted_batches;
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
        ++frames_[slot].submitted_batches;
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
        if (!result)
        {
            // Preparation may have staged a whole replica without accepting its copies.
            // A failed attempt cannot acknowledge that slice's source revision.
            for (auto& applied : frames_[next_frame_].prepared_storages)
            {
                applied.revision = 0;
            }
        }
        if (uploads_)
        {
            uploads_->finishFrameCopySubmission(result);
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
            frame.submitted_batches = 0;
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
            cancelSubmissions(RenderError{kDeviceArgument});
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        const auto resources_ready = prepareStorages(storages);
        if (!resources_ready)
        {
            cancelSubmissions(resources_ready.error());
            return cxx::unexpected(resources_ready.error());
        }
        bool instances_ready = *resources_ready;
        bool feature_copies = false;
        auto waiting = EExecutionOutcome::RESOURCE_PREPARATION_PENDING;
        if (*resources_ready)
        {
            const auto features_ready = prepareFeatures(features, feature_copies);
            if (!features_ready)
            {
                cancelSubmissions(features_ready.error());
                return cxx::unexpected(features_ready.error());
            }
            instances_ready = *features_ready;
        }
        if (instances_ready)
        {
            const auto prepared = prepareInstances(instances);
            if (!prepared)
            {
                cancelSubmissions(prepared.error());
                return cxx::unexpected(prepared.error());
            }
            instances_ready = *prepared;
            waiting = EExecutionOutcome::HISTORY_PENDING;
        }
        const bool has_copies = (uploads_ && uploads_->hasFrameCopies()) || feature_copies;
        if (!instances_ready && !has_copies)
        {
            finishPreparation(cxx::unexpected(RenderError{kDeviceBusy}));
            return outcome(waiting);
        }
        if (instances_ready)
        {
            collectFeatureCopies(features);
            prepareSubmissions(instances);
            return advanceSubmissions();
        }
        auto accepted = submitPreparationCopies(features, *resources_ready);
        if (accepted)
        {
            (void)outcome(waiting);
        }
        return accepted;
    }

    RenderResult<bool> FrameExecution::prepareStorages(std::span<const StorageFrameUse> storages) noexcept
    {
        auto& frame = frames_[next_frame_];
        bool resources_ready = true;
        frame.prepared_storages.resize(storages.size());
        for (std::size_t i = 0; i < storages.size(); ++i)
        {
            const auto& use = storages[i];
            auto& applied = frame.prepared_storages[i];
            const auto revision = use.preparation_revision ? *use.preparation_revision : 0;
            const bool already_prepared = revision != 0 && !use.identity.isNull() && applied.identity == use.identity &&
                                          applied.storage == use.storage && applied.revision == revision;
            if (already_prepared)
            {
                continue;
            }
            auto prepared = use.ops->prepareFrame(use.storage, next_frame_, *uploads_);
            if (!prepared)
            {
                return cxx::unexpected(prepared.error());
            }
            resources_ready &= *prepared;
            if (*prepared)
            {
                applied = {use.identity, use.storage, revision};
            }
        }
        return resources_ready;
    }

    RenderResult<bool> FrameExecution::prepareFeatures(
        std::span<const std::pair<RenderGraphInstance*, FeatureBinding*>> features,
        bool& has_copies
    ) noexcept
    {
        bool features_ready = true;
        for (const auto& [instance, binding] : features)
        {
            const auto revision = binding->preparation_revision ? *binding->preparation_revision : 0;
            if (revision != 0 && binding->applied_preparation[next_frame_] == revision)
            {
                continue;
            }
            for (std::size_t i = 0; i < binding->inputs.size(); ++i)
            {
                binding->resolved_inputs[i] = instance->resourceInput(binding->inputs[i], next_frame_);
            }
            const FeatureFrameInput input{next_frame_, instance->instanceParameters(), binding->resolved_inputs};
            auto prepared = binding->prepare(binding->value.get(), input, *uploads_);
            if (!prepared)
            {
                return cxx::unexpected(prepared.error());
            }
            for (const auto& update : prepared->resources)
            {
                instance->setResource(update.resource.index, next_frame_, update.input);
            }
            features_ready &= prepared->state != FeatureBinding::EPreparation::WAITING;
            if (prepared->state == FeatureBinding::EPreparation::READY)
            {
                binding->applied_preparation[next_frame_] = revision;
            }
            binding->prepared_copies = prepared->state == FeatureBinding::EPreparation::RECORD
                                           ? prepared->copies
                                           : std::span<const ResourceUploader::BufferCopy>{};
            has_copies |= !binding->prepared_copies.empty();
        }
        return features_ready;
    }

    RenderResult<bool> FrameExecution::prepareInstances(std::span<RenderGraphInstance* const> instances) noexcept
    {
        for (auto* instance : instances)
        {
            const auto prepared = instance->prepareFrame(next_frame_);
            if (!prepared)
            {
                return cxx::unexpected(prepared.error());
            }
            if (!*prepared)
            {
                return false;
            }
        }
        return true;
    }

    void FrameExecution::collectFeatureCopies(std::span<const std::pair<RenderGraphInstance*, FeatureBinding*>> features
    ) noexcept
    {
        auto& frame = frames_[next_frame_];
        frame.feature_copies.clear();
        for (const auto& [instance, binding] : features)
        {
            frame.feature_copies
                .insert(frame.feature_copies.end(), binding->prepared_copies.begin(), binding->prepared_copies.end());
            binding->prepared_copies = {};
        }
    }

    RenderResult<std::uint32_t> FrameExecution::submitPreparationCopies(
        std::span<const std::pair<RenderGraphInstance*, FeatureBinding*>> features,
        bool include_features
    ) noexcept
    {
        auto& frame = frames_[next_frame_];
        auto& graphics = device_.queue();
        auto accepted = graphics.recordAndSubmit(
            [&](nri::CommandBuffer& command, std::uint32_t) noexcept -> RenderResult<void>
            {
                if (uploads_)
                {
                    uploads_->recordFrameCopies(command);
                }
                if (include_features)
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
            cancelSubmissions(accepted.error());
            return cxx::unexpected(accepted.error());
        }
        acceptSubmission(next_frame_, graphics);
        frame.graphics_serial = graphics.submitted();
        frame.recorded_instances = 0;
        frame.recorded_passes = 0;
        finishPreparation({});
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
                   [&](const auto& execution)
                   { return execution.instance == &instance && execution.remaining_batches != 0; }
               );
    }

    bool FrameExecution::canChangeResources(std::span<RenderGraphInstance* const> instances) const noexcept
    {
        if (frames_[next_frame_].phase == EFramePhase::PREPARING)
        {
            return false;
        }
        return std::ranges::none_of(instances, [&](const auto* instance) { return submitting(*instance); });
    }

    SubmissionFrontiers FrameExecution::acceptedUses(std::span<RenderGraphInstance* const> instances) const noexcept
    {
        SubmissionFrontiers result;
        for (std::uint32_t slot = 0; slot < frame_count_; ++slot)
        {
            const auto& frame = frames_[slot];
            const bool used = frame.phase != EFramePhase::FREE &&
                              std::ranges::any_of(
                                  frame.instances,
                                  [&](const auto& execution)
                                  { return std::ranges::find(instances, execution.instance) != instances.end(); }
                              );
            if (used)
            {
                for (std::size_t q = 0; q < result.serials.size(); ++q)
                {
                    result.serials[q] = std::max(result.serials[q], frame.accepted.serials[q]);
                }
            }
        }
        return result;
    }

    void FrameExecution::prepareSubmissions(std::span<RenderGraphInstance* const> instances) noexcept
    {
        auto& frame = frames_[next_frame_];
        bool changed = frame.instances.size() != instances.size();
        for (std::size_t i = 0; !changed && i < instances.size(); ++i)
        {
            changed = frame.instances[i].instance != instances[i] ||
                      frame.instances[i].binding_revision != instances[i]->frames_[next_frame_].imported_revision;
        }
        if (changed)
        {
            rebuildDependencies(instances);
        }
        frame.recorded_instances = frame.recorded_passes = 0;
        frame.preparation_serial = 0;
        frame.remaining_batches = static_cast<std::uint32_t>(frame.batches.size());
        for (auto& ready : frame.ready_batches)
        {
            ready.clear();
        }
        for (auto& execution : frame.instances)
        {
            execution.remaining_batches = static_cast<std::uint32_t>(execution.instance->plan().submissions().size());
        }
        for (std::uint32_t b = 0; b < frame.batches.size(); ++b)
        {
            auto& batch = frame.batches[b];
            batch.serial = 0;
            batch.waits = {};
            batch.stages = {};
            batch.pending_predecessors = batch.predecessor_count;
            if (batch.pending_predecessors == 0)
            {
                enqueueReadyBatch(frame, b);
            }
        }
        frame.phase = EFramePhase::SUBMITTING;
    }

    void FrameExecution::rebuildDependencies(std::span<RenderGraphInstance* const> instances) noexcept
    {
        auto& frame = frames_[next_frame_];
        ++frame.dependency_rebuilds;
        frame.instances.resize(instances.size());
        frame.batches.clear();
        frame.dependencies.clear();
        for (std::uint32_t i = 0; i < instances.size(); ++i)
        {
            auto& execution = frame.instances[i];
            execution.instance = instances[i];
            execution.binding_revision = instances[i]->frames_[next_frame_].imported_revision;
            execution.first_batch = static_cast<std::uint32_t>(frame.batches.size());
            const auto batches = instances[i]->plan().submissions();
            for (std::uint32_t b = 0; b < batches.size(); ++b)
            {
                frame.batches.push_back({i, b, static_cast<std::uint32_t>(queueIndex(device_.queue(batches[b].queue)))}
                );
                for (const auto& successor : batches[b].successors)
                {
                    frame.dependencies.push_back(
                        {execution.first_batch + b, execution.first_batch + successor.consumer, successor.stages}
                    );
                }
            }
        }
        appendImportedDependencies(instances);
        std::ranges::sort(
            frame.dependencies,
            [](const auto& a, const auto& b)
            { return std::tie(a.producer, a.consumer) < std::tie(b.producer, b.consumer); }
        );
        std::size_t count = 0;
        for (const auto& edge : frame.dependencies)
        {
            const bool duplicate = count != 0 && frame.dependencies[count - 1].producer == edge.producer &&
                                   frame.dependencies[count - 1].consumer == edge.consumer;
            if (duplicate)
            {
                auto& stages = frame.dependencies[count - 1].stages;
                stages = stages == nri::StageBits::ALL || edge.stages == nri::StageBits::ALL ? nri::StageBits::ALL
                                                                                             : stages | edge.stages;
            }
            else
            {
                frame.dependencies[count++] = edge;
            }
        }
        frame.dependencies.resize(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const auto& dependency = frame.dependencies[i];
            auto& producer = frame.batches[dependency.producer];
            if (producer.successor_count++ == 0)
            {
                producer.first_successor = i;
            }
            ++frame.batches[dependency.consumer].predecessor_count;
        }
        frame.recording_batches.reserve(frame.batches.size());
        frame.wait_fences.reserve(instances.size() + 3);
        frame.signal_fences.reserve(instances.size());
        for (auto& ready : frame.ready_batches)
        {
            ready.reserve(frame.batches.size());
        }
    }

    void FrameExecution::appendImportedDependencies(std::span<RenderGraphInstance* const> instances) noexcept
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
            const auto base = frame.instances[i].first_batch;
            for (const auto& use : instances[i]->plan_->imported_accesses_)
            {
                const bool presentation_output = use.resource.index == instances[i]->plan_->output() &&
                                                 instances[i]->output_.description.kind == ETargetKind::SURFACE;
                if (presentation_output)
                {
                    continue;
                }
                const auto& binding = bindings[use.resource.index];
                auto range = use.range;
                if (auto* buffer = std::get_if<GraphBufferRange>(&range))
                {
                    buffer->offset += binding.buffer_offset;
                }
                frame.imported_uses.push_back(
                    {binding.image ? static_cast<const void*>(binding.image) : binding.buffer,
                     range,
                     i,
                     base + use.batch,
                     base + use.release_batch,
                     use.write}
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
                return std::tie(a.instance, a.batch) < std::tie(b.instance, b.batch);
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
                        const bool needs_order =
                            group[a].instance != group[b].instance && (group[a].write || group[b].write);
                        if (needs_order)
                        {
                            ++frame.imported_comparisons;
                            if (overlaps(group[a].range, group[b].range))
                            {
                                frame.dependencies.push_back({group[b].release_batch, group[a].batch});
                            }
                        }
                    }
                }
            }
            first = end;
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
                if (frame.pending_writes == 0)
                {
                    sealFrame(frame);
                    next_frame_ = (next_frame_ + 1) % frame_count_;
                    updateUploadAdmission();
                }
            }
            return;
        }
        if (frame.preparation_serial == 0)
        {
            finishPreparation(cxx::unexpected(error));
        }
        for (auto& execution : frame.instances)
        {
            if (execution.remaining_batches != 0 &&
                execution.remaining_batches != execution.instance->plan().submissions().size())
            {
                execution.instance->failedSubmission(error);
            }
            execution.remaining_batches = 0;
        }
        frame.feature_copies.clear();
        sealFrame(frame);
        next_frame_ = (next_frame_ + 1) % frame_count_;
        updateUploadAdmission();
    }

    bool FrameExecution::laterBatch(const FrameSlot& frame, std::uint32_t a, std::uint32_t b) noexcept
    {
        const auto& left = frame.batches[a];
        const auto& right = frame.batches[b];
        return std::tie(left.waits.serials, left.stages, a) > std::tie(right.waits.serials, right.stages, b);
    }

    void FrameExecution::enqueueReadyBatch(FrameSlot& frame, std::uint32_t batch) noexcept
    {
        auto& ready = frame.ready_batches[frame.batches[batch].queue];
        ready.push_back(batch);
        std::push_heap(ready.begin(), ready.end(), [&](auto a, auto b) { return laterBatch(frame, a, b); });
    }

    void FrameExecution::selectReadyBatches(FrameSlot& frame, std::uint32_t queue) noexcept
    {
        auto& ready = frame.ready_batches[queue];
        const auto first = ready.front();
        frame.recording_batches.clear();
        do
        {
            std::pop_heap(ready.begin(), ready.end(), [&](auto a, auto b) { return laterBatch(frame, a, b); });
            frame.recording_batches.push_back(ready.back());
            ready.pop_back();
        } while (!ready.empty() && frame.batches[first].waits.serials == frame.batches[ready.front()].waits.serials &&
                 frame.batches[first].stages == frame.batches[ready.front()].stages);
    }

    void FrameExecution::acceptBatch(FrameSlot& frame, std::uint32_t index, std::uint64_t serial) noexcept
    {
        auto& batch = frame.batches[index];
        batch.serial = serial;
        auto& execution = frame.instances[batch.instance];
        auto& instance = *execution.instance;
        if (!instance.output_.frames.empty())
        {
            if (auto* presentation = instance.output_.frames[next_frame_].presentation)
            {
                if (batch.local_batch == instance.plan_->output_first_batch_)
                {
                    presentation->acquired_consumed = true;
                }
                if (batch.local_batch + 1 == instance.plan_->submissions_.size())
                {
                    presentation->rendered_signalled = true;
                }
            }
        }
        execution.instance->acceptedBatch(batch.local_batch, batch.queue, serial);
        if (--execution.remaining_batches == 0)
        {
            frame.recorded_passes += execution.instance->recordedPassCount(next_frame_);
            execution.instance->submitted(next_frame_);
            ++frame.recorded_instances;
        }
        --frame.remaining_batches;
        const auto successors = std::span(frame.dependencies).subspan(batch.first_successor, batch.successor_count);
        for (const auto& edge : successors)
        {
            auto& next = frame.batches[edge.consumer];
            if (next.queue != batch.queue)
            {
                auto& waited = next.waits.serials[batch.queue];
                auto& stages = next.stages[batch.queue];
                stages = waited == 0                                                           ? edge.stages
                         : stages == nri::StageBits::ALL || edge.stages == nri::StageBits::ALL ? nri::StageBits::ALL
                                                                                               : stages | edge.stages;
                waited = std::max(waited, serial);
            }
            if (--next.pending_predecessors == 0)
            {
                enqueueReadyBatch(frame, edge.consumer);
            }
        }
    }

    RenderResult<std::uint32_t> FrameExecution::advanceSubmissions() noexcept
    {
        auto& frame = frames_[next_frame_];
        const auto queue_count = static_cast<std::uint32_t>(device_.queueCount());
        std::uint32_t submissions = 0;
        for (;;)
        {
            auto selected_queue = queue_count;
            for (std::uint32_t offset = 0; offset < queue_count; ++offset)
            {
                const auto q = (submit_queue_cursor_ + offset) % queue_count;
                const auto& queue = device_.physicalQueue(q);
                if (!frame.ready_batches[q].empty() && !queue.recording() && queue.hasCapacity())
                {
                    selected_queue = q;
                    break;
                }
            }
            if (selected_queue == queue_count)
            {
                break;
            }
            auto& queue = device_.physicalQueue(selected_queue);
            selectReadyBatches(frame, selected_queue);
            const auto& first = frame.batches[frame.recording_batches.front()];
            frame.wait_fences.clear();
            frame.signal_fences.clear();
            for (std::uint32_t q = 0; q < queue_count; ++q)
            {
                if (first.waits.serials[q] == 0)
                {
                    continue;
                }
                auto dependency = device_.physicalQueue(q).dependency(first.waits.serials[q], first.stages[q]);
                if (!dependency)
                {
                    cancelSubmissions(dependency.error());
                    return cxx::unexpected(dependency.error());
                }
                frame.wait_fences.push_back(*dependency);
            }
            for (auto index : frame.recording_batches)
            {
                const auto& batch = frame.batches[index];
                const auto& instance = *frame.instances[batch.instance].instance;
                if (instance.output_.frames.empty())
                {
                    continue;
                }
                const auto* presentation = instance.output_.frames[next_frame_].presentation;
                if (!presentation)
                {
                    continue;
                }
                if (batch.local_batch == instance.plan_->output_first_batch_ && !presentation->acquired_consumed)
                {
                    frame.wait_fences.push_back({presentation->acquired, 0, nri::StageBits::ALL});
                }
                if (batch.local_batch + 1 == instance.plan_->submissions_.size())
                {
                    frame.signal_fences.push_back({presentation->rendered, 0, nri::StageBits::ALL});
                }
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
                    BindingState bindings;
                    for (auto index : frame.recording_batches)
                    {
                        const auto& batch = frame.batches[index];
                        RenderExecutor::recordBatch(
                            *frame.instances[batch.instance].instance,
                            command,
                            next_frame_,
                            batch.local_batch,
                            bindings
                        );
                    }
                    return {};
                },
                frame.wait_fences,
                frame.signal_fences
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
            for (auto index : frame.recording_batches)
            {
                acceptBatch(frame, index, serial);
            }
            if (preparation)
            {
                frame.preparation_serial = serial;
                frame.feature_copies.clear();
                finishPreparation({});
            }
            ++submissions;
            submit_queue_cursor_ = static_cast<std::uint8_t>((selected_queue + 1) % queue_count);
        }
        if (frame.remaining_batches == 0)
        {
            sealFrame(frame);
            next_frame_ = (next_frame_ + 1) % frame_count_;
        }
        else if (submissions == 0)
        {
            // Only actual ready work can be blocked by recording/capacity; dependency-blocked
            // batches are not repeatedly scanned or misreported as Queue Full.
            for (std::uint32_t q = 0; q < queue_count; ++q)
            {
                const auto reason = device_.physicalQueue(q).recording() ? EExecutionOutcome::RECORDING_BUSY
                                                                         : EExecutionOutcome::QUEUE_FULL;
                for (auto index : frame.ready_batches[q])
                {
                    ++frame.instances[frame.batches[index].instance]
                          .instance->progress_.counts[static_cast<std::size_t>(reason)];
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
            output += "],\"dependency_rebuilds\":" + std::to_string(frame.dependency_rebuilds) +
                      ",\"imported_comparisons\":" + std::to_string(frame.imported_comparisons) +
                      ",\"dependency_edges\":" + std::to_string(frame.dependencies.size()) + ",\"batch_serials\":[";
            for (std::size_t instance = 0; instance < frame.instances.size(); ++instance)
            {
                if (instance)
                {
                    output += ',';
                }
                output += '[';
                const auto first = frame.instances[instance].first_batch;
                const auto end = instance + 1 == frame.instances.size() ? frame.batches.size()
                                                                        : frame.instances[instance + 1].first_batch;
                for (auto batch = first; batch < end; ++batch)
                {
                    if (batch != first)
                    {
                        output += ',';
                    }
                    output += std::to_string(frame.batches[batch].serial);
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

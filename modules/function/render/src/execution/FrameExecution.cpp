#include <lux/engine/render/extensions/upload/Preparation.hpp>
#include <algorithm>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/graph/execute/BindingState.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/graph/execute/RenderGraphInstance.hpp>
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
        for (auto& frame : frames_)
        {
            frame.execution = this;
            frame.preparation = std::make_unique<Preparation>(uploads ? uploads->windowRegionLimit() : 0);
            frame.accepted = device.makeFrontiers();
            frame.completions = std::make_unique<SubmissionCallback[]>(device.queueCount());
            frame.ready_batches.resize(device.queueCount());
        }
    }

    RenderResult<std::unique_ptr<FrameExecution>>
    FrameExecution::create(DeviceSession& device, ResourceUploader* uploads, std::uint32_t count) noexcept
    {
        if (count == 0 || count > 3)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        auto execution = std::unique_ptr<FrameExecution>(new FrameExecution(device, uploads, count));
        const auto& core = device.nriCore();
        for (std::uint32_t slot = 0; slot < count; ++slot)
        {
            auto& frame = execution->frames_[slot];
            auto result = core.CreateCommandAllocator(device.queue().native(), frame.preparation_allocator);
            if (result == nri::Result::SUCCESS)
            {
                result = core.CreateCommandBuffer(*frame.preparation_allocator, frame.preparation_command);
            }
            if (result != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(result));
            }
        }
        execution->owns_write_window_ = uploads != nullptr;
        execution->updateUploadAdmission();
        return execution;
    }

    FrameExecution::~FrameExecution() noexcept
    {
        if (rendering_)
        {
            std::terminate();
        }
        for (std::uint32_t slot = 0; slot < frame_count_; ++slot)
        {
            if (owns_write_window_ && uploads_->pendingWrites(slot)) std::terminate();
            auto& frame = frames_[slot];
            const auto& queue = device_.queue();
            const bool has_live_gpu_use = frame.preparation_command_serial > queue.completed() &&
                !queue.idle() && !queue.deviceLost();
            if (has_live_gpu_use) std::terminate();
            if (frame.preparation_command) device_.nriCore().DestroyCommandBuffer(frame.preparation_command);
            if (frame.preparation_allocator) device_.nriCore().DestroyCommandAllocator(frame.preparation_allocator);
        }
        if (owns_write_window_) uploads_->setWriteWindow(next_frame_, false, true);
    }

    bool FrameExecution::beginFrame(std::uint32_t& slot) noexcept
    {
        auto& frame = frames_[next_frame_];
        if (frame.phase == EFramePhase::FREE)
        {
            frame.accepted.clear();
            frame.submitted_batches = 0;
            frame.phase = EFramePhase::COLLECTING;
        }
        // Discrete resource writes may precede the frame packet. Adopt their collecting slice
        // without discarding pending writes or the accepted GPU completion frontiers.
        if (frame.phase != EFramePhase::COLLECTING)
        {
            return false;
        }
        slot = next_frame_;
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

    void FrameExecution::updateUploadAdmission() noexcept
    {
        if (uploads_)
        {
            const auto& frame = frames_[next_frame_];
            const bool preparing =
                rendering_ || frame.phase == EFramePhase::PREPARING || frame.phase == EFramePhase::SUBMITTING;
            const bool writable =
                !rendering_ && (frame.phase == EFramePhase::FREE || frame.phase == EFramePhase::COLLECTING);
            uploads_->setWriteWindow(next_frame_, writable, rendering_ || frame.phase == EFramePhase::SUBMITTING);
            uploads_->setBackgroundTransfersAllowed(!preparing);
        }
    }

    void FrameExecution::acceptSubmission(std::uint32_t slot, const SubmissionQueue& queue) noexcept
    {
        ++frames_[slot].submitted_batches;
        for (std::size_t i = 0; i < device_.queueCount(); ++i)
        {
            if (&device_.physicalQueue(i) == &queue)
            {
                frames_[slot].accepted.accept(device_.submissionPoint(i, queue.submitted()));
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
        frames_[next_frame_].preparation->finishCallbacks(next_frame_, result);
    }

    void FrameExecution::sealFrame(FrameSlot& frame) noexcept
    {
        if (uploads_) uploads_->finishWrites(next_frame_, frame.accepted);
        frame.phase = EFramePhase::SUBMITTED;
        frame.pending_queues = 0;
        for (auto serial : frame.accepted.serials())
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
            if (frame.accepted.serials()[i] == 0)
            {
                continue;
            }
            device_.physicalQueue(i).arm(
                frame.accepted.serials()[i],
                frame.completions[i],
                [](void* value, RenderResult<void>) noexcept
                {
                    auto& frame = *static_cast<FrameSlot*>(value);
                    if (--frame.pending_queues == 0)
                    {
                        frame.phase = EFramePhase::FREE;
                        frame.execution->updateUploadAdmission();
                    }
                },
                &frame
            );
        }
    }

    exec::task<RenderResult<std::uint32_t>> FrameExecution::render(
        std::span<RenderGraphInstance* const> instances,
        std::span<const StorageFrameUse> storages,
        std::span<const std::pair<RenderGraphInstance*, FeatureBinding*>> features
    ) noexcept
    {
        if (rendering_)
        {
            co_return cxx::unexpected(RenderError{kDeviceBusy});
        }
        rendering_ = true;
        cxx::scope_exit settle(
            [this]() noexcept
            {
                rendering_ = false;
                if (cancellation_)
                {
                    const auto error = *std::exchange(cancellation_, {});
                    cancelSubmissions(error);
                }
                updateUploadAdmission();
            }
        );
        updateUploadAdmission();
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        if (stop.stop_requested())
        {
            cancelSubmissions(RenderError{kUploadStopped});
            co_return cxx::unexpected(RenderError{kUploadStopped});
        }
        if (uploads_)
        {
            uploads_->reserveFrameStaging(!instances.empty());
        }
        auto& frame = frames_[next_frame_];
        if (frame.phase == EFramePhase::SUBMITTING)
        {
            co_return co_await advanceSubmissions();
        }
        if (uploads_ && uploads_->hasWrites(next_frame_) && frame.phase == EFramePhase::FREE)
        {
            frame.accepted.clear();
            frame.submitted_batches = 0;
            frame.phase = EFramePhase::COLLECTING;
        }
        if (instances.empty() && frame.phase == EFramePhase::FREE)
        {
            co_return 0;
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
            co_return outcome(EExecutionOutcome::PREVIOUS_SUBMISSION_PENDING);
        }
        if (frame.phase == EFramePhase::FREE)
        {
            frame.accepted.clear();
            frame.submitted_batches = 0;
        }
        frame.phase = EFramePhase::PREPARING;
        if (uploads_ && uploads_->pendingWrites(next_frame_) != 0)
        {
            co_return outcome(EExecutionOutcome::RESOURCE_PREPARATION_PENDING);
        }
        if (instances.empty())
        {
            sealFrame(frame);
            next_frame_ = (next_frame_ + 1) % frame_count_;
            co_return 0;
        }
        // A finite preparation window can be ready while its temporary command slot is pending.
        // Submit those exact staged ranges before preparing again; do not overwrite acceptance callbacks.
        if ((uploads_ && uploads_->hasFrameCopies()) || !frame.feature_copies.empty())
        {
            co_return co_await submitPreparationCopies(features, false);
        }
        // This synchronous phase may fill staging and update descriptors, but may not
        // submit, suspend or dispatch callbacks. Only the following recording segment owns a CommandBuffer.
        if ((!storages.empty() || !features.empty()) && !uploads_)
        {
            cancelSubmissions(RenderError{kDeviceArgument});
            co_return cxx::unexpected(RenderError{kDeviceArgument});
        }
        const auto resources_ready = prepareStorages(storages);
        if (!resources_ready)
        {
            cancelSubmissions(resources_ready.error());
            co_return cxx::unexpected(resources_ready.error());
        }
        // A complete replacement may still await submission acceptance before publishing its binding.
        // Capacity exhaustion is an error from the preparer, never a request for another partial window.
        bool instances_ready = *resources_ready;
        bool feature_copies = false;
        auto waiting = EExecutionOutcome::RESOURCE_PREPARATION_PENDING;
        if (*resources_ready)
        {
            const auto features_ready = prepareFeatures(features, feature_copies);
            if (!features_ready)
            {
                cancelSubmissions(features_ready.error());
                co_return cxx::unexpected(features_ready.error());
            }
            if (!*features_ready)
            {
                cancelSubmissions(RenderError{kDeviceCapacity});
                co_return cxx::unexpected(RenderError{kDeviceCapacity});
            }
        }
        if (instances_ready)
        {
            const auto prepared = prepareDrawLists(instances);
            if (!prepared)
            {
                cancelSubmissions(prepared.error());
                co_return cxx::unexpected(prepared.error());
            }
            if (!*prepared)
            {
                cancelSubmissions(RenderError{kDeviceCapacity});
                co_return cxx::unexpected(RenderError{kDeviceCapacity});
            }
        }
        if (instances_ready)
        {
            const auto prepared = prepareInstances(instances);
            if (!prepared)
            {
                cancelSubmissions(prepared.error());
                co_return cxx::unexpected(prepared.error());
            }
            instances_ready = *prepared;
            waiting = EExecutionOutcome::HISTORY_PENDING;
        }
        const bool has_copies = (uploads_ && uploads_->hasFrameCopies()) || feature_copies;
        if (!instances_ready && !has_copies)
        {
            finishPreparation(cxx::unexpected(RenderError{kDeviceBusy}));
            co_return outcome(waiting);
        }
        if (instances_ready)
        {
            collectFeatureCopies(features);
            auto prepared = prepareSubmissions(instances);
            if (!prepared)
            {
                cancelSubmissions(prepared.error());
                co_return cxx::unexpected(prepared.error());
            }
            co_return co_await advanceSubmissions();
        }
        auto accepted = co_await submitPreparationCopies(features, *resources_ready);
        if (accepted)
        {
            (void)outcome(waiting);
        }
        co_return accepted;
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
            auto prepared = use.ops->prepareFrame(use.storage, next_frame_, *uploads_, *frame.preparation);
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
            const auto& definition = *binding->definition;
            for (std::size_t i = 0; i < definition.inputs.size(); ++i)
            {
                binding->resolved_inputs[i] = instance->resourceInput(definition.inputs[i], next_frame_);
            }
            const FeatureFrameInput input{
                next_frame_,
                instance->instanceParameters().subspan(binding->parameter_offset, binding->parameter_bytes),
                binding->resolved_inputs
            };
            const auto prepare = std::visit(
                [](const auto& operation) noexcept
                {
                    if constexpr (std::same_as<
                                      std::remove_cvref_t<decltype(operation)>,
                                      FeatureBinding::RasterPreparation>)
                    {
                        return operation.prepare_resources;
                    }
                    else
                    {
                        return operation;
                    }
                },
                binding->preparation
            );
            auto prepared = prepare(binding->value.get(), input, *uploads_, *frames_[next_frame_].preparation);
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
                                           : std::span<const BufferCopyRegion>{};
            has_copies |= !binding->prepared_copies.empty();
        }
        return features_ready;
    }

    RenderResult<bool> FrameExecution::prepareDrawLists(std::span<RenderGraphInstance* const> instances) noexcept
    {
        bool ready = true;
        for (auto* instance : instances)
        {
            if (instance->frames_[next_frame_].draw_lists.empty())
            {
                continue;
            }
            if (!uploads_)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            auto prepared = instance->prepareDrawListUploads(next_frame_, *uploads_);
            // No staging progress needs no submission callback. Terminal frame delivery also
            // settles unprepared sources, including frames with no active output.
            if (!prepared || uploads_->hasFrameCopies())
            {
                frames_[next_frame_].preparation->onSubmitted(
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

    exec::task<RenderResult<std::uint32_t>> FrameExecution::submitPreparationCopies(
        std::span<const std::pair<RenderGraphInstance*, FeatureBinding*>> features,
        bool include_features
    ) noexcept
    {
        auto& frame = frames_[next_frame_];
        auto& graphics = device_.queue();
        if (include_features)
        {
            collectFeatureCopies(features);
        }
        if (graphics.submitting() || frame.preparation_command_serial > graphics.completed())
        {
            co_return 0;
        }
        if (uploads_ && uploads_->hasFrameCopies())
        {
            auto copied = co_await uploads_->prepareFrameCopies();
            if (!copied)
            {
                cancelSubmissions(copied.error());
                co_return cxx::unexpected(copied.error());
            }
        }
        if (cancellation_)
        {
            co_return cxx::unexpected(*cancellation_);
        }
        auto recorded = device_.resetCommandAllocator(*frame.preparation_allocator);
        if (recorded == nri::Result::SUCCESS)
        {
            recorded = device_.nriCore().BeginCommandBuffer(*frame.preparation_command, nullptr);
        }
        if (recorded == nri::Result::SUCCESS)
        {
            if (uploads_)
            {
                uploads_->recordFrameCopies(*frame.preparation_command);
            }
            recordPreservingCopies(device_.nriCore(), *frame.preparation_command, frame.feature_copies);
            recorded = device_.nriCore().EndCommandBuffer(*frame.preparation_command);
        }
        if (recorded != nri::Result::SUCCESS)
        {
            auto error = nativeError(recorded);
            cancelSubmissions(error);
            co_return cxx::unexpected(error);
        }
        auto accepted = co_await graphics.submit({&frame.preparation_command, 1});
        if (!accepted)
        {
            cancelSubmissions(accepted.error());
            co_return cxx::unexpected(accepted.error());
        }
        acceptSubmission(next_frame_, graphics);
        frame.graphics_serial = *accepted;
        frame.preparation_command_serial = *accepted;
        frame.recorded_instances = 0;
        frame.recorded_passes = 0;
        frame.feature_copies.clear();
        finishPreparation({});
        if (cancellation_)
        {
            co_return cxx::unexpected(*cancellation_);
        }
        co_return 1; // A preparation submission does not publish an instance or advance its history.
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
        auto result = device_.makeFrontiers();
        for (std::uint32_t slot = 0; slot < frame_count_; ++slot)
        {
            const auto& frame = frames_[slot];
            if (frame.phase == EFramePhase::FREE)
            {
                continue;
            }
            const bool used = std::ranges::any_of(
                frame.instances,
                [&](const auto& execution)
                { return std::ranges::find(instances, execution.instance) != instances.end(); }
            );
            if (!used)
            {
                continue;
            }
            auto graph_frontiers = device_.makeFrontiers();
            for (const auto& batch : frame.batches)
            {
                graph_frontiers.accept(device_.submissionPoint(batch.queue, batch.serial));
                if (std::ranges::find(instances, frame.instances[batch.instance].instance) != instances.end())
                {
                    result.accept(device_.submissionPoint(batch.queue, batch.serial));
                }
            }
            // The first Graphics submission includes shared preparation copies. Discrete
            // uploads not covered by the graph batch table retain their real queue frontier.
            const auto graphics = queueIndex(device_.queue());
            result.accept(device_.submissionPoint(graphics, frame.preparation_serial));
            for (std::size_t q = 0; q < result.size(); ++q)
            {
                const bool unassigned = frame.preparation_serial == 0 ||
                    frame.accepted.serials()[q] > graph_frontiers.serials()[q];
                if (unassigned)
                {
                    result.accept(device_.submissionPoint(q, frame.accepted.serials()[q]));
                }
            }
        }
        return result;
    }

    RenderResult<void> FrameExecution::prepareSubmissions(std::span<RenderGraphInstance* const> instances) noexcept
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
            if (auto rebuilt = rebuildDependencies(instances); !rebuilt)
            {
                // Do not reuse an incomplete dependency cache on a later frame.
                frame.instances.clear();
                return rebuilt;
            }
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
            batch.waits.clear();
            std::ranges::fill(batch.stages, nri::StageBits::NONE);
            batch.pending_predecessors = batch.predecessor_count;
            if (batch.pending_predecessors == 0)
            {
                enqueueReadyBatch(frame, b);
            }
        }
        frame.phase = EFramePhase::SUBMITTING;
        return {};
    }

    RenderResult<void> FrameExecution::rebuildDependencies(std::span<RenderGraphInstance* const> instances) noexcept
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
                auto& batch = frame.batches.emplace_back();
                batch.instance = i;
                batch.local_batch = b;
                batch.queue = static_cast<std::uint32_t>(queueIndex(device_.queue(batches[b].queue)));
                batch.waits = device_.makeFrontiers();
                batch.stages.resize(device_.queueCount());
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
        // Imported Target relationships can form a cycle across otherwise valid individual graphs.
        // Qualify the combined graph once when bindings change, before accepting any submission.
        std::vector<std::uint32_t> pending;
        pending.reserve(frame.batches.size());
        frame.recording_batches.clear();
        for (std::uint32_t i = 0; i < frame.batches.size(); ++i)
        {
            pending.push_back(frame.batches[i].predecessor_count);
            if (pending.back() == 0)
            {
                frame.recording_batches.push_back(i);
            }
        }
        for (std::size_t cursor = 0; cursor < frame.recording_batches.size(); ++cursor)
        {
            const auto& batch = frame.batches[frame.recording_batches[cursor]];
            for (std::uint32_t i = 0; i < batch.successor_count; ++i)
            {
                const auto destination = frame.dependencies[batch.first_successor + i].consumer;
                if (--pending[destination] == 0)
                {
                    frame.recording_batches.push_back(destination);
                }
            }
        }
        if (frame.recording_batches.size() != frame.batches.size())
        {
            return cxx::unexpected(RenderError{kGraphDependencyCycle});
        }
        frame.recording_batches.clear();
        frame.recording_batches.reserve(frame.batches.size());
        frame.wait_fences.reserve(instances.size() + 3);
        frame.signal_fences.reserve(instances.size());
        for (auto& ready : frame.ready_batches)
        {
            ready.reserve(frame.batches.size());
        }
        return {};
    }

    void FrameExecution::appendImportedDependencies(std::span<RenderGraphInstance* const> instances) noexcept
    {
        auto& frame = frames_[next_frame_];
        frame.imported_uses.clear();
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
                    if (use.whole_resource)
                    {
                        buffer->size = binding.buffer_size - buffer->offset;
                    }
                    buffer->offset += binding.buffer_offset;
                }
                frame.imported_uses.push_back(
                    {binding.image ? static_cast<const void*>(binding.image) : binding.buffer,
                     range,
                     i,
                     base + use.batch,
                     base + use.release_batch,
                     use.write,
                     use.resource.index == instances[i]->plan_->output()}
                );
                for (auto* buffer : binding.addressed_buffers)
                {
                    frame.imported_uses.push_back(
                        {buffer,
                         GraphBufferRange{0, device_.nriCore().GetBufferDesc(*buffer).size},
                         i,
                         base + use.batch,
                         base + use.release_batch,
                         use.write}
                    );
                }
            }
        }
        for (std::uint32_t i = 0; i < instances.size(); ++i)
        {
            const auto& instance = *instances[i];
            const auto& physical = instance.frames_[next_frame_];
            const auto base = frame.instances[i].first_batch;
            for (std::uint32_t batch = 0; batch < instance.plan_->submissions_.size(); ++batch)
            {
                const auto& submission = instance.plan_->submissions_[batch];
                for (std::uint32_t pass = submission.first_pass; pass < submission.first_pass + submission.pass_count;
                     ++pass)
                {
                    const auto* raster = instance.plan_->passes()[pass].work.getIf<RasterCommands>();
                    if (!raster)
                    {
                        continue;
                    }
                    const auto* dynamic = std::get_if<RasterListCommands>(&raster->draw);
                    const auto* streamed = std::get_if<IndexedListCommands>(&raster->draw);
                    const auto* list = dynamic    ? &physical.raster_draws[dynamic->data_index]
                                       : streamed ? &physical.draw_lists[streamed->data_index].draws
                                                  : nullptr;
                    if (list)
                    {
                        for (const auto& read : list->reads)
                        {
                            frame.imported_uses.push_back(
                                {read.physical, read.range, i, base + batch, base + batch, false}
                            );
                        }
                    }
                }
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
                if (a.target_output != b.target_output)
                {
                    return a.target_output; // A sampled Target is produced before another instance consumes it.
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
                        const bool needs_order = group[a].batch != group[b].batch && (group[a].write || group[b].write);
                        if (needs_order)
                        {
                            ++frame.imported_comparisons;
                            if (overlaps(group[a].range, group[b].range))
                            {
                                if (group[b].release_batch != group[a].batch)
                                {
                                    frame.dependencies.push_back({group[b].release_batch, group[a].batch});
                                }
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
        if (rendering_)
        {
            if (!cancellation_)
            {
                cancellation_ = error;
            }
            return; // Actual native result must be adopted before retiring this frame's borrowed payloads.
        }
        auto& frame = frames_[next_frame_];
        if (frame.phase != EFramePhase::SUBMITTING)
        {
            if (frame.phase == EFramePhase::COLLECTING || frame.phase == EFramePhase::PREPARING)
            {
                // Unsubmitted replica copies can still have Sender receivers waiting for acceptance.
                // Accepted uploads retain their existing GPU completion and finishWrite paths.
                finishPreparation(cxx::unexpected(error));
                frame.feature_copies.clear();
                if (!uploads_ || uploads_->pendingWrites(next_frame_) == 0)
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
        return std::tie(left.waits.serials(), left.stages, a) > std::tie(right.waits.serials(), right.stages, b);
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
        } while (!ready.empty() && frame.batches[first].waits == frame.batches[ready.front()].waits &&
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
                    presentation->acquire_wait_submitted = true;
                }
                if (batch.local_batch + 1 == instance.plan_->submissions_.size())
                {
                    presentation->render_signal_submitted = true;
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
                const auto waited = next.waits.serials()[batch.queue];
                auto& stages = next.stages[batch.queue];
                stages = waited == 0                                                           ? edge.stages
                         : stages == nri::StageBits::ALL || edge.stages == nri::StageBits::ALL ? nri::StageBits::ALL
                                                                                               : stages | edge.stages;
                next.waits.accept(device_.submissionPoint(batch.queue, serial));
            }
            if (--next.pending_predecessors == 0)
            {
                enqueueReadyBatch(frame, edge.consumer);
            }
        }
    }

    exec::task<RenderResult<std::uint32_t>> FrameExecution::advanceSubmissions() noexcept
    {
        auto& frame = frames_[next_frame_];
        const bool has_staged_copies = uploads_ && uploads_->hasFrameCopies();
        const bool has_pending_preparation =
            frame.preparation_serial == 0 && (has_staged_copies || !frame.feature_copies.empty());
        if (has_pending_preparation && frame.preparation_command_serial > device_.queue().completed())
        {
            co_return 0;
        }
        const auto queue_count = static_cast<std::uint32_t>(device_.queueCount());
        std::uint32_t submissions = 0;
        for (;;)
        {
            auto selected_queue = queue_count;
            for (std::uint32_t offset = 0; offset < queue_count; ++offset)
            {
                const auto q = (submit_queue_cursor_ + offset) % queue_count;
                const auto& queue = device_.physicalQueue(q);
                if (!frame.ready_batches[q].empty() && !queue.submitting())
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
                if (first.waits.serials()[q] == 0)
                {
                    continue;
                }
                auto dependency = device_.physicalQueue(q).dependency(first.waits.serials()[q], first.stages[q]);
                if (!dependency)
                {
                    cancelSubmissions(dependency.error());
                    co_return cxx::unexpected(dependency.error());
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
                if (batch.local_batch == instance.plan_->output_first_batch_ && !presentation->acquire_wait_submitted)
                {
                    frame.wait_fences.push_back({presentation->acquired, 0, instance.plan_->output_wait_stages_});
                }
                if (batch.local_batch + 1 == instance.plan_->submissions_.size())
                {
                    frame.signal_fences.push_back({presentation->rendered, 0, nri::StageBits::ALL});
                }
            }
            const bool preparation = frame.preparation_serial == 0;
            frame.recording_commands.clear();
            const bool has_copies = preparation && (has_staged_copies || !frame.feature_copies.empty());
            if (has_copies)
            {
                if (has_staged_copies)
                {
                    auto copied = co_await uploads_->prepareFrameCopies();
                    if (!copied)
                    {
                        cancelSubmissions(copied.error());
                        co_return cxx::unexpected(copied.error());
                    }
                }
                if (cancellation_)
                {
                    co_return cxx::unexpected(*cancellation_);
                }
                auto result = device_.resetCommandAllocator(*frame.preparation_allocator);
                if (result == nri::Result::SUCCESS)
                {
                    result = device_.nriCore().BeginCommandBuffer(*frame.preparation_command, nullptr);
                }
                if (result == nri::Result::SUCCESS)
                {
                    if (has_staged_copies) uploads_->recordFrameCopies(*frame.preparation_command);
                    recordPreservingCopies(device_.nriCore(), *frame.preparation_command, frame.feature_copies);
                    result = device_.nriCore().EndCommandBuffer(*frame.preparation_command);
                }
                if (result != nri::Result::SUCCESS)
                {
                    const auto error = nativeError(result);
                    cancelSubmissions(error);
                    co_return cxx::unexpected(error);
                }
                frame.recording_commands.push_back(frame.preparation_command);
            }
            for (auto index : frame.recording_batches)
            {
                const auto& batch = frame.batches[index];
                auto commands = frame.instances[batch.instance].instance->recordBatch(next_frame_, batch.local_batch);
                if (!commands)
                {
                    cancelSubmissions(commands.error());
                    co_return cxx::unexpected(commands.error());
                }
                frame.recording_commands.insert(frame.recording_commands.end(), commands->begin(), commands->end());
            }
            auto accepted = co_await queue.submit(frame.recording_commands, frame.wait_fences, frame.signal_fences);
            if (!accepted)
            {
                cancelSubmissions(accepted.error());
                co_return cxx::unexpected(accepted.error());
            }
            const auto serial = *accepted;
            if (has_copies)
            {
                frame.preparation_command_serial = serial;
            }
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
            if (cancellation_)
            {
                co_return cxx::unexpected(*cancellation_);
            }
            submit_queue_cursor_ = (selected_queue + 1) % queue_count;
        }
        if (frame.remaining_batches == 0)
        {
            sealFrame(frame);
            next_frame_ = (next_frame_ + 1) % frame_count_;
        }
        else if (submissions == 0)
        {
            // Graph commands own their slots. Only an unconfirmed submission on a physical
            // queue can defer ready work; dependency-blocked batches are not rescanned.
            for (std::uint32_t q = 0; q < queue_count; ++q)
            {
                const auto reason = EExecutionOutcome::RECORDING_BUSY;
                for (auto index : frame.ready_batches[q])
                {
                    ++frame.instances[frame.batches[index].instance]
                          .instance->progress_.counts[static_cast<std::size_t>(reason)];
                }
            }
        }
        co_return submissions;
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
            for (std::size_t queue = 0; queue < frame.accepted.size(); ++queue)
            {
                if (queue)
                {
                    output += ',';
                }
                output += std::to_string(frame.accepted.serials()[queue]);
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
                const auto serial = frame.accepted.serials()[index];
                if (serial > queue.completed())
                {
                    wait_queue_cursor_ = (index + 1) % count;
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
            if (queue.submitted() > queue.completed())
            {
                wait_queue_cursor_ = (index + 1) % count;
                return queue.wait(queue.completed() + 1, timeout_ns);
            }
        }
        return false;
    }

} // namespace lux::render::detail

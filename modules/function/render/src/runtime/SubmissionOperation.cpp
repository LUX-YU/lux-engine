#include <algorithm>
#include <array>
#include <bit>
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
#include <chrono>
#endif
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/NativeResult.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/graph/DumpText.hpp>
#include <lux/engine/render/graph/Range.hpp>
#include <lux/engine/render/graph/execute/RenderGraphInstance.hpp>
#include <lux/engine/render/runtime/SubmissionOperation.hpp>
#include <tuple>

namespace lux::render::detail
{
    SubmissionOperation::RasterReads SubmissionOperation::Workspace::takeRasterReads() noexcept
    {
        return std::exchange(raster_reads_, {});
    }

    SubmissionOperation::Workspace::Workspace(std::uint32_t count, std::size_t queues) noexcept : slots_(count)
    {
        for (auto& slot : slots_)
        {
            slot.ready_batches.resize(queues);
        }
    }

    std::uint32_t SubmissionOperation::Workspace::submittedBatches(std::uint32_t slot) const noexcept
    {
        return slots_[slot].submitted_batches;
    }

    SubmissionOperation::SubmissionOperation(DeviceSession& device, FrameProgress::Round& round,
        Workspace& workspace) noexcept
        : device_(device), round_(round), workspace_(workspace), slot_(workspace.slots_[round.context().slot_index])
    {
        slot_.graph_started = false;
        slot_.submitted_batches = 0;
        slot_.preparation_serial = 0;
        slot_.uniform_upload_serial = 0;
        slot_.remaining_batches = 0;
        slot_.recorded_passes = slot_.recorded_instances = 0;
    }

    void SubmissionOperation::accept(const SubmissionQueue& queue) noexcept
    {
        ++slot_.submitted_batches;
        round_.accept(device_.submissionPoint(device_.queueIndex(queue), queue.submitted()));
    }

    void SubmissionOperation::acceptPreparation(std::uint64_t serial) noexcept
    {
        slot_.preparation_serial = serial;
        slot_.graphics_serial = serial;
        accept(device_.queue());
    }

    bool SubmissionOperation::refersTo(const RenderGraphInstance& instance) const noexcept
    {
        return slot_.graph_started &&
            std::ranges::find(slot_.instances, &instance, &InstanceExecution::instance) != slot_.instances.end();
    }

    void SubmissionOperation::rejectSuffix(RenderError error) noexcept
    {
        if (!slot_.graph_started) return;
        for (auto& execution : slot_.instances)
        {
            if (execution.remaining_batches != 0 &&
                execution.remaining_batches != execution.instance->plan().submissions().size())
            {
                execution.instance->failedSubmission(error);
            }
            execution.remaining_batches = 0;
        }
        slot_.remaining_batches = 0;
    }

    bool SubmissionOperation::hasPresentationBoundary(const BatchExecution& batch) const noexcept
    {
        const auto& instance = *slot_.instances[batch.instance].instance;
        return instance.output_.description.kind == ETargetKind::SURFACE &&
            (batch.local_batch == instance.plan_->output_first_batch_ ||
             batch.local_batch + 1 == instance.plan_->submissions().size());
    }

    exec::task<RenderResult<void>> SubmissionOperation::record(execution::CpuScheduler cpu, bool parallel) noexcept
    {
        if (slot_.batches.empty()) co_return RenderResult<void>{};
        const auto caller = co_await stdexec::read_env(stdexec::get_scheduler);
        const auto frame = slotIndex();
        slot_.recordings.assign(slot_.batches.size(), {});
        const auto record_batch = [this, frame](std::size_t i) noexcept
        {
            const auto& batch = slot_.batches[i];
            auto result = slot_.instances[batch.instance].instance->recordFrozenBatch(frame, batch.local_batch);
            if (result) slot_.recordings[i].commands = *result;
            else slot_.recordings[i].error = result.error();
        };
        const auto deliver = [caller](auto sender) noexcept
        {
            return std::move(sender) |
                stdexec::upon_stopped(
                    []() noexcept -> RenderResult<void> { return cxx::unexpected(RenderError{kCpuStopped}); }
                ) |
                stdexec::continues_on(caller) | stdexec::upon_error(NativeDeliveryFailure<void>{});
        };
        RenderResult<void> recorded;
        if (parallel)
        {
            recorded = co_await deliver(
                stdexec::schedule(cpu) | stdexec::bulk(stdexec::par, slot_.batches.size(), record_batch) |
                stdexec::then([]() noexcept -> RenderResult<void> { return {}; })
            );
        }
        else
        {
            recorded = co_await deliver(stdexec::schedule(cpu) | stdexec::then(
                [record_batch, count = slot_.batches.size()]() noexcept -> RenderResult<void>
                {
                    for (std::size_t i = 0; i < count; ++i) record_batch(i);
                    return {};
                }
            ));
        }
        if (!recorded) co_return recorded;
        for (const auto& result : slot_.recordings)
        {
            if (result.error.type) co_return cxx::unexpected(result.error);
        }
        co_return RenderResult<void>{};
    }

    RenderResult<void> SubmissionOperation::prepare(
        std::span<RenderGraphInstance* const> instances, std::span<const SubmissionOrder> order
    ) noexcept
    {
        auto& frame = slot_;
        bool changed = frame.instances.size() != instances.size() || !std::ranges::equal(frame.order, order);
        for (std::size_t i = 0; !changed && i < instances.size(); ++i)
        {
            changed = frame.instances[i].instance != instances[i] ||
                      frame.instances[i].binding_revision != instances[i]->frames_[slotIndex()].imported_revision;
        }
        if (changed)
        {
            if (auto rebuilt = rebuildDependencies(instances, order); !rebuilt)
            {
                // Do not reuse an incomplete dependency cache on a later frame.
                frame.instances.clear();
                return rebuilt;
            }
        }
        frame.recorded_instances = frame.recorded_passes = 0;
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
        frame.graph_started = true;
        return {};
    }

    RenderResult<void> SubmissionOperation::rebuildDependencies(
        std::span<RenderGraphInstance* const> instances, std::span<const SubmissionOrder> order
    ) noexcept
    {
        auto& frame = slot_;
        ++frame.dependency_rebuilds;
        frame.instances.resize(instances.size());
        frame.order.assign(order.begin(), order.end());
        frame.batches.clear();
        frame.dependencies.clear();
        for (std::uint32_t i = 0; i < instances.size(); ++i)
        {
            auto& execution = frame.instances[i];
            execution.instance = instances[i];
            execution.binding_revision = instances[i]->frames_[slotIndex()].imported_revision;
            execution.first_batch = static_cast<std::uint32_t>(frame.batches.size());
            const auto batches = instances[i]->plan().submissions();
            for (std::uint32_t b = 0; b < batches.size(); ++b)
            {
                auto& batch = frame.batches.emplace_back();
                batch.instance = i;
                batch.local_batch = b;
                batch.queue = static_cast<std::uint32_t>(device_.queueIndex(device_.queue(batches[b].queue)));
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
        // A whole-Job order connects the actual tails and heads of the physical queues.
        constexpr auto roles = static_cast<std::size_t>(EQueueRole::TRANSFER) + 1;
        std::array<std::uint32_t, roles> tails, heads;
        for (const auto& edge : order)
        {
            if (edge.producer >= instances.size() || edge.consumer >= instances.size())
                return cxx::unexpected(RenderError{kJobIdentity});
            std::ranges::fill(tails, kGraphInvalidIndex);
            std::ranges::fill(heads, kGraphInvalidIndex);
            const auto producer_first = frame.instances[edge.producer].first_batch;
            const auto consumer_first = frame.instances[edge.consumer].first_batch;
            const auto producer_count = instances[edge.producer]->plan().submissions().size();
            const auto consumer_count = instances[edge.consumer]->plan().submissions().size();
            for (std::uint32_t b = 0; b < producer_count; ++b)
            {
                const auto index = producer_first + b;
                tails[frame.batches[index].queue] = index;
            }
            for (std::uint32_t b = 0; b < consumer_count; ++b)
            {
                const auto index = consumer_first + b;
                auto& head = heads[frame.batches[index].queue];
                head = std::min(head, index);
            }
            for (auto tail : tails)
            {
                if (tail == kGraphInvalidIndex) continue;
                for (auto head : heads)
                {
                    if (head != kGraphInvalidIndex)
                    {
                        frame.dependencies.push_back({tail, head});
                    }
                }
            }
        }
        if (auto imported = appendImportedDependencies(instances); !imported) return imported;
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

    RenderResult<void> SubmissionOperation::appendImportedDependencies(std::span<RenderGraphInstance* const> instances) noexcept
    {
        auto& frame = slot_;
        frame.imported_uses.clear();
        for (std::uint32_t i = 0; i < instances.size(); ++i)
        {
            const auto& bindings = instances[i]->frames_[slotIndex()].bindings;
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
                    if (use.whole_resource && binding.buffer)
                    {
                        if (buffer->offset >= binding.buffer_size)
                            return cxx::unexpected(RenderError{kDeviceArgument});
                        buffer->size = binding.buffer_size - buffer->offset;
                    }
                    buffer->offset += binding.buffer_offset;
                }
                if (const auto* physical = binding.image ? static_cast<const void*>(binding.image) : binding.buffer)
                {
                    frame.imported_uses.push_back(
                        {physical, range, i, base + use.batch, base + use.release_batch, use.write,
                         use.resource.index == instances[i]->plan_->output(), use.resource}
                    );
                }
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
            const auto& physical = instance.frames_[slotIndex()];
            const auto base = frame.instances[i].first_batch;
            for (std::uint32_t batch = 0; batch < instance.plan_->submissions_.size(); ++batch)
            {
                const auto& submission = instance.plan_->submissions_[batch];
                for (auto source : submission.shared_draw_reads)
                {
                    const auto& list = physical.raster_draws[source];
                    for (const auto& read : list.reads)
                    {
                        frame.imported_uses.push_back({read.physical, read.range, i, base + batch, base + batch, false});
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
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        const auto overlap_start = std::chrono::steady_clock::now();
#endif
        for (std::size_t first = 0; first < frame.imported_uses.size();)
        {
            auto end = first + 1;
            while (end < frame.imported_uses.size() &&
                   frame.imported_uses[end].physical == frame.imported_uses[first].physical)
            {
                ++end;
            }
            const auto group = std::span(frame.imported_uses).subspan(first, end - first);
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
            if (group.size() >= frame.imported_group_counts.size())
            {
                frame.imported_group_counts.resize(group.size() + 1);
            }
            ++frame.imported_group_counts[group.size()];
#endif
            if (std::ranges::any_of(group, &ImportedUse::write))
            {
                for (std::size_t a = 0; a < group.size(); ++a)
                {
                    for (std::size_t b = 0; b < a; ++b)
                    {
                        const bool already_compiled = group[a].instance == group[b].instance &&
                            group[a].logical.index != kGraphInvalidIndex && group[a].logical == group[b].logical;
                        const bool needs_order = !already_compiled && group[a].batch != group[b].batch &&
                            (group[a].write || group[b].write);
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
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        frame.imported_overlap_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - overlap_start
        ).count();
#endif
        frame.imported_uses.clear();
        return {};
    }

    bool SubmissionOperation::laterBatch(const Slot& frame, std::uint32_t a, std::uint32_t b) noexcept
    {
        const auto& left = frame.batches[a];
        const auto& right = frame.batches[b];
        return std::tie(left.waits.serials(), left.stages, a) > std::tie(right.waits.serials(), right.stages, b);
    }

    void SubmissionOperation::enqueueReadyBatch(Slot& frame, std::uint32_t batch) noexcept
    {
        auto& ready = frame.ready_batches[frame.batches[batch].queue];
        ready.push_back(batch);
        std::push_heap(ready.begin(), ready.end(), [&](auto a, auto b) { return laterBatch(frame, a, b); });
    }

    void SubmissionOperation::selectReadyBatches(Slot& frame, std::uint32_t queue) noexcept
    {
        auto& ready = frame.ready_batches[queue];
        const auto first = ready.front();
        frame.recording_batches.clear();
        do
        {
            std::pop_heap(ready.begin(), ready.end(), [&](auto a, auto b) { return laterBatch(frame, a, b); });
            frame.recording_batches.push_back(ready.back());
            ready.pop_back();
        } while (!ready.empty() && !hasPresentationBoundary(frame.batches[first]) &&
                 !hasPresentationBoundary(frame.batches[ready.front()]) &&
                 frame.batches[first].waits == frame.batches[ready.front()].waits &&
                 frame.batches[first].stages == frame.batches[ready.front()].stages);
    }

    void SubmissionOperation::acceptBatch(Slot& frame, std::uint32_t index, std::uint64_t serial) noexcept
    {
        auto& batch = frame.batches[index];
        batch.serial = serial;
        auto& execution = frame.instances[batch.instance];
        auto& instance = *execution.instance;
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        const auto& lists = instance.frames_[slotIndex()].raster_draws;
        const auto& plan = instance.plan_->submissions_[batch.local_batch];
        for (auto source : plan.shared_draw_reads)
        {
            for (const auto& read : lists[source].reads)
            {
                if (read.usage == EGraphUse::STORAGE_READ)
                {
                    ++workspace_.raster_reads_.buffer_ranges;
                    workspace_.raster_reads_.buffer_bytes += std::get<GraphBufferRange>(read.range).size;
                }
                else if (read.usage == EGraphUse::SAMPLED)
                {
                    const auto& range = std::get<GraphImageRange>(read.range);
                    ++workspace_.raster_reads_.image_ranges;
                    workspace_.raster_reads_.image_subresources +=
                        std::uint64_t(range.mip_count) * range.layer_count * std::popcount(range.aspects);
                }
                else if (read.usage == EGraphUse::VERTEX || read.usage == EGraphUse::INDEX)
                {
                    ++workspace_.raster_reads_.geometry_ranges;
                    workspace_.raster_reads_.geometry_bytes += std::get<GraphBufferRange>(read.range).size;
                }
            }
        }
#endif
        const auto point = device_.submissionPoint(batch.queue, serial);
        if (!instance.output_.frames.empty())
        {
            const auto& output = instance.output_.frames[slotIndex()];
            if (output.accepted_uses) output.accepted_uses->accept(point);
            if (auto* presentation = output.presentation)
            {
                if (batch.local_batch == instance.plan_->output_first_batch_)
                {
                    if (presentation->phase != EPresentationPhase::READY) std::terminate();
                    *presentation->acquire_use = point;
                    presentation->phase = EPresentationPhase::WAIT_ACCEPTED;
                }
                if (batch.local_batch + 1 == instance.plan_->submissions_.size())
                {
                    presentation->phase = EPresentationPhase::SUBMITTED;
                }
            }
        }
        instance.acceptedBatch(batch.local_batch, point);
        if (--execution.remaining_batches == 0)
        {
            frame.recorded_passes += execution.instance->recordedPassCount(slotIndex());
            execution.instance->submitted(slotIndex());
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

    std::uint32_t SubmissionOperation::readyQueue() const noexcept
    {
        const auto queue_count = static_cast<std::uint32_t>(device_.queueCount());
        for (std::uint32_t offset = 0; offset < queue_count; ++offset)
        {
            const auto q = (workspace_.queue_cursor_ + offset) % queue_count;
            if (!slot_.ready_batches[q].empty() && !device_.physicalQueue(q).submitting())
            {
                return q;
            }
        }
        return queue_count;
    }

    bool SubmissionOperation::canSubmit() const noexcept
    {
        return complete() || readyQueue() != device_.queueCount();
    }

    exec::task<RenderResult<std::uint32_t>> SubmissionOperation::submit(const std::optional<RenderError>& cancel) noexcept
    {
        auto& frame = slot_;
        if (cancel) co_return cxx::unexpected(*cancel);
        const auto queue_count = static_cast<std::uint32_t>(device_.queueCount());
        std::uint32_t submissions = 0;
        for (;;)
        {
            const auto selected_queue = readyQueue();
            if (selected_queue == queue_count)
            {
                break;
            }
            auto& queue = device_.physicalQueue(selected_queue);
            selectReadyBatches(frame, selected_queue);
            const auto& first = frame.batches[frame.recording_batches.front()];
            frame.wait_fences.clear();
            frame.signal_fences.clear();
            const auto graphics = device_.queueIndex(device_.queue());
            const auto preparation_serial = std::max(frame.preparation_serial, frame.uniform_upload_serial);
            if (selected_queue != graphics && preparation_serial > first.waits.serials()[graphics])
            {
                auto wait = device_.queue().dependency(preparation_serial, nri::StageBits::ALL);
                if (!wait) co_return cxx::unexpected(wait.error());
                frame.wait_fences.push_back(*wait);
            }
            for (std::uint32_t q = 0; q < queue_count; ++q)
            {
                const bool covered_by_preparation =
                    q == graphics && selected_queue != graphics && preparation_serial > first.waits.serials()[q];
                if (first.waits.serials()[q] == 0 || covered_by_preparation)
                {
                    continue;
                }
                auto dependency = device_.physicalQueue(q).dependency(first.waits.serials()[q], first.stages[q]);
                if (!dependency)
                {
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
                const auto* presentation = instance.output_.frames[slotIndex()].presentation;
                if (!presentation)
                {
                    continue;
                }
                if (batch.local_batch == instance.plan_->output_first_batch_ && presentation->phase == EPresentationPhase::READY)
                {
                    frame.wait_fences.push_back({presentation->acquired, 0, instance.plan_->output_wait_stages_});
                }
                if (batch.local_batch + 1 == instance.plan_->submissions_.size())
                {
                    frame.signal_fences.push_back({presentation->rendered, 0, nri::StageBits::ALL});
                }
            }
            frame.recording_commands.clear();
            for (auto index : frame.recording_batches)
            {
                const auto commands = frame.recordings[index].commands;
                frame.recording_commands.insert(frame.recording_commands.end(), commands.begin(), commands.end());
            }
            auto accepted = co_await queue.submit(frame.recording_commands, frame.wait_fences, frame.signal_fences);
            if (!accepted)
            {
                co_return cxx::unexpected(accepted.error());
            }
            const auto serial = *accepted;
            accept(queue);
            if (&queue == &device_.queue())
            {
                frame.graphics_serial = serial;
            }
            for (auto index : frame.recording_batches)
            {
                acceptBatch(frame, index, serial);
            }
            ++submissions;
            if (cancel) co_return cxx::unexpected(*cancel);
            workspace_.queue_cursor_ = (selected_queue + 1) % queue_count;
        }
        if (frame.remaining_batches != 0 && submissions == 0)
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

    SubmissionOperation::InstanceUse
    SubmissionOperation::Workspace::acceptedFrameUse(
        std::uint32_t slot, const RenderGraphInstance& instance, SubmissionFrontiers& uses
    ) const noexcept
    {
        uses.clear();
        InstanceUse result;
        const auto& frame = slots_[slot];
        if (!frame.graph_started) return result;
        const auto execution = std::ranges::find(frame.instances, &instance, &InstanceExecution::instance);
        if (execution == frame.instances.end()) return result;
        const auto& plan = instance.plan();
        const auto batches = std::span(frame.batches).subspan(execution->first_batch, plan.submissions().size());
        result.completed = !batches.empty();
        const auto& conditions = instance.frames_[slot].enabled_regions;
        for (const auto& batch : batches)
        {
            if (!batch.serial)
            {
                result.completed = false;
                continue;
            }
            uses.accept(SubmissionPoint{instance.session_, batch.queue, batch.serial});
            const auto& submission = plan.submissions()[batch.local_batch];
            const auto passes = plan.passes().subspan(submission.first_pass, submission.pass_count);
            result.wrote_output |= std::ranges::any_of(passes, [&](const auto& pass) noexcept
            {
                const auto region = pass.condition_region;
                return pass.writes_output && (region == kGraphInvalidIndex || conditions[region]);
            });
        }
        return result;
    }

    SubmissionFrontiers SubmissionOperation::Workspace::acceptedUses(
        const DeviceSession& device, const FrameProgress& progress,
        std::span<RenderGraphInstance* const> instances
    ) const noexcept
    {
        auto result = device.makeFrontiers();
        auto completed = device.makeFrontiers();
        device.readCompleted(completed);
        for (std::uint32_t slot = 0; slot < slots_.size(); ++slot)
        {
            const auto& frame = slots_[slot];
            if (!frame.graph_started || progress.frontiers(slot).reachedBy(completed))
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
            auto graph_frontiers = device.makeFrontiers();
            for (const auto& batch : frame.batches)
            {
                graph_frontiers.accept(device.submissionPoint(batch.queue, batch.serial));
                if (std::ranges::find(instances, frame.instances[batch.instance].instance) != instances.end())
                {
                    result.accept(device.submissionPoint(batch.queue, batch.serial));
                }
            }
            // The first Graphics submission includes shared preparation copies. Discrete
            // uploads not covered by the graph batch table retain their real queue frontier.
            const auto graphics = device.queueIndex(device.queue());
            result.accept(device.submissionPoint(graphics, frame.preparation_serial));
            const auto& accepted = progress.frontiers(slot);
            for (std::size_t q = 0; q < result.size(); ++q)
            {
                const bool unassigned = frame.preparation_serial == 0 ||
                    accepted.serials()[q] > graph_frontiers.serials()[q];
                if (unassigned)
                {
                    result.accept(device.submissionPoint(q, accepted.serials()[q]));
                }
            }
        }
        return result;
    }

    bool SubmissionOperation::Workspace::dumpSubmissions(const FrameProgress& progress, DumpSink sink) const noexcept
    {
        if (!sink.write)
        {
            return false;
        }
        std::string output = "{\"kind\":\"accepted graph batches per FIF slot\",\"slots\":[";
        for (std::uint32_t i = 0; i < slots_.size(); ++i)
        {
            if (i)
            {
                output += ',';
            }
            const auto& frame = slots_[i];
            output += "{\"slot\":" + std::to_string(i) + ",\"serial\":" + std::to_string(frame.graphics_serial) +
                      ",\"preparation_serial\":" + std::to_string(frame.preparation_serial) +
                      ",\"instances\":" + std::to_string(frame.recorded_instances) +
                      ",\"passes\":" + std::to_string(frame.recorded_passes) + ",\"accepted_frontiers\":[";
            const auto& accepted = progress.frontiers(i);
            for (std::size_t queue = 0; queue < accepted.size(); ++queue)
            {
                if (queue)
                {
                    output += ',';
                }
                output += std::to_string(accepted.serials()[queue]);
            }
            output += "],\"dependency_rebuilds\":" + std::to_string(frame.dependency_rebuilds) +
                      ",\"imported_comparisons\":" + std::to_string(frame.imported_comparisons);
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
            output += ",\"imported_overlap_ns\":" + std::to_string(frame.imported_overlap_ns) +
                      ",\"imported_groups_by_size\":[";
            for (std::size_t size = 0; size < frame.imported_group_counts.size(); ++size)
            {
                if (size) output += ',';
                output += std::to_string(frame.imported_group_counts[size]);
            }
            output += ']';
#endif
            output += ",\"dependency_edges\":" + std::to_string(frame.dependencies.size()) + ",\"batch_serials\":[";
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
} // namespace lux::render::detail

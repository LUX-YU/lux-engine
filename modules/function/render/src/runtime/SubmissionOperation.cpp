#include <lux/cxx/core/scope_exit.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/NativeResult.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/graph/execute/FrameExecutor.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/graph/DumpText.hpp>
#include <lux/engine/render/graph/StageScope.hpp>
#include <lux/engine/render/runtime/JobExecution.hpp>
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

    SubmissionOperation::SubmissionOperation(
        DeviceSession& device, FrameProgress& progress, FrameProgress::SlotSelection selection, Workspace& workspace
    ) noexcept
        : device_(device), progress_(progress), selection_(selection), workspace_(workspace),
          slot_(workspace.slots_[selection.frame.slot_index])
    {
        slot_.graph_started = false;
        slot_.submitted_batches = 0;
        slot_.preparation_serial = 0;
        slot_.remaining_batches = 0;
        slot_.recorded_passes = slot_.recorded_instances = 0;
    }

    SubmissionOperation::~SubmissionOperation() noexcept
    {
        slot_.recordings.clear();
        slot_.recording_commands.clear();
        slot_.wait_fences.clear();
        slot_.signal_fences.clear();
    }

    void SubmissionOperation::accept(const SubmissionQueue& queue, std::uint64_t serial) noexcept
    {
        ++slot_.submitted_batches;
        progress_.adopt(selection_, device_.submissionPoint(device_.queueIndex(queue), serial));
    }

    void SubmissionOperation::acceptPreparation(std::uint64_t serial) noexcept
    {
        slot_.preparation_serial = serial;
        slot_.graphics_serial = serial;
        accept(device_.queue(), serial);
    }

    void SubmissionOperation::rejectSuffix(RenderError error) noexcept
    {
        if (!slot_.graph_started) return;
        for (std::size_t i = 0; i < slot_.jobs.size(); ++i)
        {
            auto& execution = slot_.jobs[i];
            if (execution.remaining_batches != 0 &&
                execution.remaining_batches != jobs_[i]->graph().submissions().size())
            {
                jobs_[i]->submission_error_ = error;
            }
            execution.remaining_batches = 0;
        }
        slot_.remaining_batches = 0;
    }

    bool SubmissionOperation::hasPresentationBoundary(const BatchExecution& batch) const noexcept
    {
        const auto& job = *jobs_[batch.instance];
        return job.output_.description.kind == ETargetKind::SURFACE &&
            (batch.local_batch == job.graph().output_first_batch_ ||
             batch.local_batch + 1 == job.graph().submissions().size());
    }

    exec::task<RenderResult<void>> SubmissionOperation::record(execution::CpuScheduler cpu, bool parallel) noexcept
    {
        if (slot_.batches.empty()) co_return RenderResult<void>{};
        const auto caller = co_await stdexec::read_env(stdexec::get_scheduler);
        auto& frames = slot_.recording_frames;
        cxx::scope_exit release_inputs([&]() noexcept { frames.clear(); });
        frames.reserve(jobs_.size());
        slot_.recordings.resize(slot_.batches.size());
        for (const auto* job : jobs_)
        {
            const auto* input = job->frames_[slotIndex()].recordingFrame();
            if (!input) co_return cxx::unexpected(RenderError{kDeviceArgument});
            frames.push_back(input);
        }
        const auto record_batch = [
            inputs = std::span<const FrozenRecordingFrame* const>{frames}, results = std::span{slot_.recordings},
            batches = std::span<const BatchExecution>{slot_.batches}, &device = std::as_const(device_)
        ](std::size_t i) noexcept
        {
            auto& result = results[i];
            const auto& batch = batches[i];
            auto recorded = FrameExecutor::recordBatch(*inputs[batch.instance], device, batch.local_batch);
            result.error = recorded ? RenderError{} : recorded.error();
            result.commands = recorded ? *recorded : std::span<nri::CommandBuffer* const>{};
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
        std::span<runtime_detail::JobExecution* const> jobs, std::span<const SubmissionOrder> order
    ) noexcept
    {
        auto& frame = slot_;
        jobs_ = jobs;
        bool changed = frame.jobs.size() != jobs_.size() || !std::ranges::equal(frame.order, order);
        for (std::size_t i = 0; !changed && i < jobs_.size(); ++i)
        {
            changed = frame.jobs[i].binding != jobs_[i]->binding_identity;
        }
        if (changed)
        {
            if (auto rebuilt = rebuildDependencies(order); !rebuilt)
            {
                // Do not reuse an incomplete dependency cache on a later frame.
                frame.jobs.clear();
                return rebuilt;
            }
        }
        frame.recorded_instances = frame.recorded_passes = 0;
        frame.remaining_batches = static_cast<std::uint32_t>(frame.batches.size());
        for (auto& ready : frame.ready_batches)
        {
            ready.clear();
        }
        for (std::size_t i = 0; i < jobs_.size(); ++i)
        {
            auto& receipt = frame.jobs[i].result;
            receipt.completed = false;
            receipt.wrote_output = false;
            receipt.accepted.clear();
            frame.jobs[i].remaining_batches = static_cast<std::uint32_t>(jobs_[i]->graph().submissions().size());
        }
        for (std::uint32_t b = 0; b < frame.batches.size(); ++b)
        {
            auto& batch = frame.batches[b];
            batch.serial = 0;
            batch.waits.clear();
            std::ranges::fill(batch.stages, nri::StageBits::NONE);
            const auto& job = *jobs_[batch.instance];
            for (auto id : job.graph().submissions_[batch.local_batch].imported_resources)
            {
                const auto* prepared = job.frames_[slotIndex()].selectedUses(id.index).preparation;
                if (!prepared) continue;
                for (std::uint32_t q = 0; q < prepared->size(); ++q)
                {
                    const auto serial = prepared->serials()[q];
                    if (q == batch.queue || serial <= device_.physicalQueue(q).completed()) continue;
                    batch.waits.accept(device_.submissionPoint(q, serial));
                    batch.stages[q] = nri::StageBits::ALL;
                }
            }
            batch.pending_predecessors = batch.predecessor_count;
            if (batch.pending_predecessors == 0)
            {
                enqueueReadyBatch(frame, b);
            }
        }
        frame.graph_started = true;
        return {};
    }

    RenderResult<void> SubmissionOperation::rebuildDependencies(std::span<const SubmissionOrder> order) noexcept
    {
        auto& frame = slot_;
        ++frame.dependency_rebuilds;
        frame.jobs.resize(jobs_.size());
        frame.order.assign(order.begin(), order.end());
        frame.batches.clear();
        frame.dependencies.clear();
        for (std::uint32_t i = 0; i < jobs_.size(); ++i)
        {
            auto& execution = frame.jobs[i];
            execution.binding = jobs_[i]->binding_identity;
            if (!execution.result.accepted.size()) execution.result.accepted = device_.makeFrontiers();
            execution.result.first_batch = static_cast<std::uint32_t>(frame.batches.size());
            const auto batches = jobs_[i]->graph().submissions();
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
                        {execution.result.first_batch + b, execution.result.first_batch + successor.consumer,
                         successor.stages}
                    );
                }
            }
        }
        for (const auto& edge : order)
        {
            if (edge.producer >= jobs_.size() || edge.consumer >= jobs_.size())
                return cxx::unexpected(RenderError{kJobIdentity});
            const auto& producer = jobs_[edge.producer]->graph();
            const auto& consumer = jobs_[edge.consumer]->graph();
            const bool target_edge = edge.target_input != kGraphInvalidIndex;
            if (target_edge && edge.target_input >= consumer.targetImports().size())
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            const auto& tails = target_edge ? producer.last_output_batches_ : producer.last_batches_;
            const auto& heads = target_edge ? consumer.targetImports()[edge.target_input].first_reads :
                consumer.first_batches_;
            const auto producer_base = frame.jobs[edge.producer].result.first_batch;
            const auto consumer_base = frame.jobs[edge.consumer].result.first_batch;
            for (auto tail : tails.batches)
            {
                if (tail == kGraphInvalidIndex) continue;
                for (auto head : heads.batches)
                    if (head != kGraphInvalidIndex)
                        frame.dependencies.push_back({producer_base + tail, consumer_base + head});
            }
        }
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
                stages = combineStages(stages, edge.stages);
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
        frame.wait_fences.reserve(jobs_.size() + 3);
        frame.signal_fences.reserve(jobs_.size());
        for (auto& ready : frame.ready_batches)
        {
            ready.reserve(frame.batches.size());
        }
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

    void SubmissionOperation::acceptBatch(
        Slot& frame, std::uint32_t index, std::uint64_t serial
    ) noexcept
    {
        auto& batch = frame.batches[index];
        batch.serial = serial;
        auto& execution = frame.jobs[batch.instance];
        auto& job = *jobs_[batch.instance];
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        const auto& lists = job.frames_[slotIndex()].raster_draws;
        const auto& plan = job.graph().submissions()[batch.local_batch];
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
        auto& receipt = execution.result;
        receipt.accepted.accept(point);
        if (!receipt.wrote_output)
        {
            const auto& graph = job.graph();
            const auto& submitted = graph.submissions()[batch.local_batch];
            const auto regions = std::span(graph.output_write_regions_).subspan(
                submitted.first_output_region, submitted.output_region_count
            );
            const auto& enabled = job.frames_[slotIndex()].enabled_regions;
            receipt.wrote_output = submitted.writes_output ||
                std::ranges::any_of(regions, [&](auto region) { return enabled[region] != 0; });
        }
        if (!job.output_.frames.empty())
        {
            const auto& output = job.output_.frames[slotIndex()];
            if (output.accepted_uses) output.accepted_uses->accept(point);
            if (auto* presentation = output.presentation)
            {
                if (batch.local_batch == job.graph().output_first_batch_)
                {
                    if (presentation->phase != EPresentationPhase::READY) std::terminate();
                    *presentation->acquire_use = point;
                    presentation->phase = EPresentationPhase::WAIT_ACCEPTED;
                }
                if (batch.local_batch + 1 == job.graph().submissions_.size())
                {
                    presentation->phase = EPresentationPhase::SUBMITTED;
                }
            }
        }
        job.acceptedBatch(slotIndex(), batch.local_batch, point);
        if (--execution.remaining_batches == 0)
        {
            frame.recorded_passes += job.frames_[slotIndex()].enabled_pass_count;
            job.submitted(slotIndex());
            receipt.completed = true;
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
                stages = waited == 0 ? edge.stages : combineStages(stages, edge.stages);
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

    exec::task<RenderResult<std::uint32_t>>
    SubmissionOperation::submit(const std::optional<RenderError>& cancel) noexcept
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
            const auto preparation_serial = frame.preparation_serial;
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
                const auto& job = *jobs_[batch.instance];
                if (job.output_.frames.empty())
                {
                    continue;
                }
                const auto* presentation = job.output_.frames[slotIndex()].presentation;
                if (!presentation)
                {
                    continue;
                }
                if (batch.local_batch == job.graph().output_first_batch_ && presentation->phase == EPresentationPhase::READY)
                {
                    frame.wait_fences.push_back({presentation->acquired, 0, job.graph().output_wait_stages_});
                }
                if (batch.local_batch + 1 == job.graph().submissions_.size())
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
            accept(queue, serial);
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
                    ++jobs_[frame.batches[index].instance]->progress.counts[static_cast<std::size_t>(reason)];
                }
            }
        }
        co_return submissions;
    }

    const SubmissionOperation::JobSubmissionResult* SubmissionOperation::Workspace::acceptedFrameUse(
        std::uint32_t slot, std::size_t ordinal, ExecutionBindingId identity
    ) const & noexcept
    {
        const auto& frame = slots_[slot];
        if (!frame.graph_started || ordinal >= frame.jobs.size()) return nullptr;
        const auto& job = frame.jobs[ordinal];
        return job.binding == identity ? &job.result : nullptr;
    }

    bool SubmissionOperation::Workspace::acceptedPass(
        std::uint32_t slot, const runtime_detail::JobExecution& job, const JobSubmissionResult& receipt,
        std::uint32_t pass
    ) const noexcept
    {
        if (receipt.first_batch == kGraphInvalidIndex) return false;
        const auto& command = job.graph().passes()[pass];
        const auto& batch = slots_[slot].batches[receipt.first_batch + command.submission];
        return batch.serial && (command.condition_region == kGraphInvalidIndex ||
            job.frames_[slot].enabled_regions[command.condition_region]);
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
            output += "],\"dependency_rebuilds\":" + std::to_string(frame.dependency_rebuilds);
            output += ",\"dependency_edges\":" + std::to_string(frame.dependencies.size()) + ",\"batch_serials\":[";
            for (std::size_t instance = 0; instance < frame.jobs.size(); ++instance)
            {
                if (instance)
                {
                    output += ',';
                }
                output += '[';
                const auto first = frame.jobs[instance].result.first_batch;
                const auto end = instance + 1 == frame.jobs.size() ? frame.batches.size()
                                                                        : frame.jobs[instance + 1].result.first_batch;
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

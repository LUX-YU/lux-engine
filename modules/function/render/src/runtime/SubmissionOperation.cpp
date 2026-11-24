#include <lux/engine/render/device/SubmissionQueue.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/NativeResult.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/graph/execute/FrameExecutor.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
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
        DeviceSession& device, FrameProgress& progress, FrameContext selection, Workspace& workspace
    ) noexcept
        : device_(device), progress_(progress), selection_(selection), workspace_(workspace),
          slot_(workspace.slots_[selection.slot_index])
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
        slot_.submit_commands.clear();
        slot_.wait_fences.clear();
        slot_.signal_fences.clear();
    }

    void SubmissionOperation::accept(SubmissionPoint point) noexcept
    {
        ++slot_.submitted_batches;
        progress_.adopt(selection_, point);
    }

    void SubmissionOperation::acceptPreparation(SubmissionPoint point) noexcept
    {
        accept(point);
        slot_.preparation_serial = point.serial;
        slot_.graphics_serial = point.serial;
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
                settleBufferUses(i);
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
            const auto* input = job->frame_bindings.frames[slotIndex()].recordingFrame();
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

    void SubmissionOperation::BatchExecution::addWait(SubmissionPoint point, nri::StageBits scope) noexcept
    {
        if (point.queue == queue) return;
        auto& covered = stages[point.queue];
        covered = waits.serials()[point.queue] ? combineStages(covered, scope) : scope;
        waits.accept(point);
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
            if (auto rebuilt = rebuildDependencies(order); !rebuilt) return rebuilt;
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
            frame.jobs[i].remaining_batches = static_cast<std::uint32_t>(jobs_[i]->graph().submissions().size());
        }
        for (std::uint32_t b = 0; b < frame.batches.size(); ++b)
        {
            auto& batch = frame.batches[b];
            batch.serial = 0;
            batch.acceptance_unknown = false;
            batch.waits.clear();
            std::ranges::fill(batch.stages, nri::StageBits::NONE);
            const auto& job = *jobs_[batch.instance];
            const auto addWaits = [&](const SubmissionFrontiers* prepared, nri::StageBits stages) noexcept
            {
                if (!prepared) return;
                for (std::uint32_t q = 0; q < prepared->size(); ++q)
                {
                    const auto serial = prepared->serials()[q];
                    if (serial <= device_.physicalQueue(q).completed()) continue;
                    batch.addWait(device_.submissionPoint(q, serial), stages);
                }
            };
            const auto& frozen = job.frame_bindings.frames[slotIndex()];
            for (const auto [resource, stages] : job.graph().submissions_[batch.local_batch].resource_frontiers)
                if (stages != nri::StageBits::NONE) addWaits(frozen.bindings[resource].queue_uses.preparation, stages);
            for (const auto& read : frozen.targetReads(batch.local_batch))
                if (read.wait_stages != nri::StageBits::NONE) addWaits(read.uses.preparation, read.wait_stages);
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
        std::uint32_t total_batches = 0;
        for (const auto* job : jobs_)
        {
            const auto count = job->graph().submissions().size();
            if (count > UINT32_MAX - total_batches) return cxx::unexpected(RenderError{kRuntimeCapacity});
            total_batches += static_cast<std::uint32_t>(count);
        }
        // Admission has already sorted the selected product DAG. Validate indices before replacing the FIF cache.
        for (const auto& edge : order)
        {
            if (edge.consumer >= jobs_.size() || edge.producer >= jobs_.size())
                return cxx::unexpected(RenderError{kJobIdentity});
            if (edge.producer == edge.consumer) return cxx::unexpected(RenderError{kGraphDependencyCycle});
            const bool invalid_order = edge.producer > edge.consumer;
            const bool invalid_batch = edge.consumer_batch >= jobs_[edge.consumer]->graph().submissions().size();
            if (invalid_order || invalid_batch)
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
        }
        ++frame.dependency_rebuilds;
        frame.jobs.resize(jobs_.size());
        frame.batches.resize(total_batches);
        frame.order.assign(order.begin(), order.end());
        std::uint32_t next_batch = 0;
        for (std::uint32_t i = 0; i < jobs_.size(); ++i)
        {
            auto& execution = frame.jobs[i];
            execution.readers = {};
            execution.binding = jobs_[i]->binding_identity;
            execution.result.first_batch = next_batch;
            const auto batches = jobs_[i]->graph().submissions();
            for (std::uint32_t b = 0; b < batches.size(); ++b)
            {
                auto& batch = frame.batches[next_batch++];
                batch.instance = i;
                batch.local_batch = b;
                batch.queue = static_cast<std::uint32_t>(device_.queueIndex(device_.queue(batches[b].queue)));
                if (!batch.waits.size())
                {
                    batch.waits = device_.makeFrontiers();
                    batch.stages.resize(device_.queueCount());
                }
                batch.predecessor_count = static_cast<std::uint32_t>(batches[b].dependencies.size());
            }
        }
        auto& reads = frame.product_reads;
        reads.clear();
        reads.reserve(order.size());
        for (const auto& edge : order)
        {
            const auto base = frame.jobs[edge.consumer].result.first_batch;
            reads.push_back({edge.producer, base + edge.consumer_batch, edge.stages});
        }
        const auto key = [](const ProductRead& read) noexcept
        {
            return std::pair{read.producer_job, read.consumer_batch};
        };
        std::ranges::sort(reads, {}, key);
        std::size_t count = 0;
        for (const auto& read : reads)
        {
            if (count && key(reads[count - 1]) == key(read))
                reads[count - 1].stages = combineStages(reads[count - 1].stages, read.stages);
            else
                reads[count++] = read;
        }
        reads.resize(count);
        for (std::size_t first = 0; first < count;)
        {
            const auto producer = reads[first].producer_job;
            auto end = first;
            do
            {
                ++frame.batches[reads[end].consumer_batch].predecessor_count;
                ++end;
            } while (end < count && reads[end].producer_job == producer);
            frame.jobs[producer].readers = std::span{reads}.subspan(first, end - first);
            first = end;
        }
        frame.selected_batches.reserve(frame.batches.size());
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
        frame.selected_batches.clear();
        do
        {
            std::pop_heap(ready.begin(), ready.end(), [&](auto a, auto b) { return laterBatch(frame, a, b); });
            frame.selected_batches.push_back(ready.back());
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
        const auto& frozen = job.frame(slotIndex());
        const auto& plan = job.graph().submissions()[batch.local_batch];
        for (auto p : plan.raster_list_passes)
        {
            const auto& pass = job.graph().passes()[p];
            if (!frozen.passEnabled(pass)) continue;
            const auto& list = std::get<RasterListCommands>(job.graph().rasterCommands(pass)->draw);
            for (const auto& read : frozen.raster_draws[list.data_index].reads)
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
        const auto& submitted = job.graph().submissions()[batch.local_batch];
        if (!receipt.wrote_output)
        {
            const auto& enabled = job.frame_bindings.frames[slotIndex()].enabled_regions;
            receipt.wrote_output = submitted.writes_output ||
                std::ranges::any_of(submitted.output_regions, [&](auto region) { return enabled[region] != 0; });
        }
        if (!job.output_.frames.empty())
        {
            const auto& output = job.output_.frames[slotIndex()];
            if ((submitted.accesses_output || output.presentation) && output.accepted_uses)
                output.accepted_uses->accept(point);
            if (submitted.accesses_output && output.producer_uses) output.producer_uses->accept(point);
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
            frame.recorded_passes += job.frame_bindings.frames[slotIndex()].enabled_pass_count;
            job.submitted(slotIndex());
            receipt.completed = true;
            ++frame.recorded_instances;
        }
        --frame.remaining_batches;
        const auto arrive = [&](std::uint32_t consumer) noexcept
        {
            if (--frame.batches[consumer].pending_predecessors == 0) enqueueReadyBatch(frame, consumer);
        };
        for (const auto& edge : submitted.successors)
        {
            const auto consumer = receipt.first_batch + edge.consumer;
            frame.batches[consumer].addWait(point, edge.stages);
            arrive(consumer);
        }
        if (!receipt.completed) return;
        settleBufferUses(batch.instance);
        if (execution.readers.empty()) return;

        // Admission links CURRENT_RUN readers only to a sampled offscreen output. Release the
        // receipt after the whole Job is accepted, with waits for this physical image's uses.
        const auto& writes = job.output_.frames[slotIndex()].producer_uses->serials();
        for (std::uint32_t q = 0; q < writes.size(); ++q)
        {
            if (!writes[q]) continue;
            const auto image_use = device_.submissionPoint(q, writes[q]);
            for (const auto& read : execution.readers)
                frame.batches[read.consumer_batch].addWait(image_use, read.stages);
        }
        for (const auto& read : execution.readers) arrive(read.consumer_batch);
    }

    void SubmissionOperation::settleBufferUses(std::size_t instance) noexcept
    {
        const auto& receipt = slot_.jobs[instance];
        const auto& job = *jobs_[instance];
        const auto& products = job.buffer_products;
        if (products.empty()) return;

        const auto batches = std::span{slot_.batches}.subspan(
            receipt.result.first_batch, job.graph().submissions().size()
        );
        // Buffer pins protect the backing until settlement. FIFO lets one accepted point per physical queue
        // cover the entire prefix, including whole-buffer barriers. An unknown submission has no serial.
        for (const auto tail : job.graph().last_batches_.batches)
        {
            if (tail == kGraphInvalidIndex) continue;
            const auto queue = batches[tail].queue;
            for (std::size_t end = std::size_t{tail} + 1; end != 0; --end)
            {
                const auto& batch = batches[end - 1];
                if (batch.queue != queue || !batch.serial) continue;
                const auto point = device_.submissionPoint(queue, batch.serial);
                for (const auto& product : products) product.accept(point);
                break;
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
    SubmissionOperation::submit(const RenderError& cancel) noexcept
    {
        auto& frame = slot_;
        if (cancel.type) co_return cxx::unexpected(cancel);
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
            if (const auto terminal = queue.terminalError(); terminal.type) co_return cxx::unexpected(terminal);
            selectReadyBatches(frame, selected_queue);
            const auto& first = frame.batches[frame.selected_batches.front()];
            frame.wait_fences.clear();
            frame.signal_fences.clear();
            const auto graphics = device_.queueIndex(device_.queue());
            for (std::uint32_t q = 0; q < queue_count; ++q)
            {
                auto serial = first.waits.serials()[q];
                auto stages = first.stages[q];
                if (frame.preparation_serial && q == graphics && selected_queue != graphics)
                {
                    // A later graph signal covers the preparation serial on this queue, but its
                    // narrower stage mask cannot cover preparation work used by other stages.
                    serial = std::max(serial, frame.preparation_serial);
                    stages = nri::StageBits::ALL;
                }
                if (!serial) continue;
                auto dependency = device_.physicalQueue(q).dependency(serial, stages);
                if (!dependency)
                {
                    co_return cxx::unexpected(dependency.error());
                }
                frame.wait_fences.push_back(*dependency);
            }
            for (auto index : frame.selected_batches)
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
            frame.submit_commands.clear();
            for (auto index : frame.selected_batches)
            {
                const auto commands = frame.recordings[index].commands;
                frame.submit_commands.insert(frame.submit_commands.end(), commands.begin(), commands.end());
            }
            auto accepted = co_await queue.submit(frame.submit_commands, frame.wait_fences, frame.signal_fences);
            if (!accepted)
            {
                const auto failure = accepted.error();
                if (failure.type == kOutcomeUnknown)
                {
                    for (const auto index : frame.selected_batches) frame.batches[index].acceptance_unknown = true;
                }
                co_return cxx::unexpected(failure);
            }
            const auto serial = *accepted;
            accept(device_.submissionPoint(selected_queue, serial));
            if (&queue == &device_.queue())
            {
                frame.graphics_serial = serial;
            }
            const auto completed_before = frame.recorded_instances;
            for (auto index : frame.selected_batches)
            {
                acceptBatch(frame, index, serial);
            }
            ++submissions;
            if (cancel.type) co_return cxx::unexpected(cancel);
            workspace_.queue_cursor_ = (selected_queue + 1) % queue_count;
            // Publish finished Jobs before another native queue operation can suspend this round.
            if (frame.recorded_instances != completed_before) break;
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

    bool SubmissionOperation::Workspace::mayHaveExecuted(
        std::uint32_t slot, const JobSubmissionResult& receipt, std::uint32_t local_batch
    ) const noexcept
    {
        if (receipt.first_batch == kGraphInvalidIndex) return false;
        const auto& batch = slots_[slot].batches[receipt.first_batch + local_batch];
        return batch.serial || batch.acceptance_unknown;
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
            std::size_t edge_count = 0;
            for (const auto& batch : frame.batches) edge_count += batch.predecessor_count;
            output += ",\"dependency_edges\":" + std::to_string(edge_count) + ",\"batch_serials\":[";
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
                    const auto& receipt = frame.batches[batch];
                    output += receipt.acceptance_unknown ? "null" : std::to_string(receipt.serial);
                }
                output += ']';
            }
            output += "]}";
        }
        output += "]}\n";
        return sink.write(sink.destination, output);
    }
} // namespace lux::render::detail

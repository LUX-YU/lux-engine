#include <lux/engine/render/device/RasterProgramUpdate.hpp>
#include <lux/engine/render/frame/FrameData.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>

namespace lux::render
{
    void
    RenderServer::Impl::queryFrameInputLayout(const RenderJobId& id, QueryRenderJobInputLayout::Response& output) noexcept
    {
        const auto* job = runtime_->jobs_.find(id);
        if (!job || !job->control.running())
        {
            output.error = {job ? kJobStopped : kJobIdentity};
            return;
        }
        output = {{}, job->execution->input_layout};
    }

    RenderResult<void> RenderServer::Impl::adoptFrameData(const JobExecution& execution, const FrameData& packet) noexcept
    {
        const auto layout = packet.layoutKey();
        if (!packet.sealed() || !layout.valid() || layout != execution.input_layout.key())
        {
            return cxx::unexpected(RenderError{kFrameContract});
        }
        const auto has_updates = [&](const auto& input) noexcept
        {
            return std::ranges::any_of(execution.columnsFor(input), [&](auto column) noexcept
            {
                return packet.column(column).count != 0;
            });
        };
        for (const auto& input : execution.inputs)
        {
            if (!has_updates(input)) continue;
            if (runtime_->resources_.declarationOf(input.identity) != input.call_slot)
            {
                return cxx::unexpected(RenderError{kStorageIdentity});
            }
            if (runtime_->resources_.hasExclusiveCommand(input.identity))
            {
                return cxx::unexpected(RenderError{kDeviceBusy});
            }
        }
        for (const auto& input : execution.inputs)
        {
            if (!has_updates(input)) continue;
            const auto& call = runtime_->resources_.factory(input.call_slot)->frame_call;
            auto* object = runtime_->resources_.resourceInstance(input.identity);
            if (auto applied = call.invoke(object, packet, execution.columnsFor(input)); !applied) return applied;
        }
        return {};
    }

    void RenderServer::Impl::updateFrameData(const UpdateFrameData::Request& input, AckResponse& output) noexcept
    {
        auto* job = runtime_->jobs_.find(input.job);
        if (!job) { output.error = {kJobIdentity}; return; }
        if (!job->control.running()) { output.error = {kJobStopped}; return; }
        if (!input.data) { output.error = {kFrameContract}; return; }
        const auto& execution = *job->execution;
        if (const auto error = runtime_->device(execution.device)->admissionError(); error.type)
        {
            output.error = error;
            return;
        }
        const auto updated = adoptFrameData(execution, *input.data);
        if (updated) job->last_run_error = {};
        output.error = updated ? RenderError{} : updated.error();
    }

    bool RenderServer::Impl::beginFrame(detail::DeviceEntry& entry) noexcept
    {
        auto& round = entry.round;
        auto& progress = entry.frameProgress();
        const auto& inputs = round.inputs;
        const bool structure_changed = inputs.revision != job_structure_revision_;
        if (structure_changed) rebuildFrameInputs(entry);
        const auto proposed_slot = progress.nextSlot();
        if (const auto selection = selectFrameInputs(entry, proposed_slot); selection.type)
        {
            for (const auto& job : inputs.jobs)
            {
                auto* live = runtime_->jobs_.find(job.id);
                if (live && live->control.executionGeneration() == job.execution_generation)
                    live->control.rejectUnadmitted(selection);
            }
            return false;
        }
        if (round.outputs.empty()) return false;
        const auto release_pins = [&]
        {
            for (const auto& output : round.outputs)
            {
                const auto& execution = *runtime_->jobs_.find(output.job)->execution;
                runtime_->resources_.unpinBufferUses(execution.productUses(), proposed_slot);
            }
        };
        if (!progress.canBegin(entry.session()))
        {
            release_pins();
            return false;
        }

        round.operation.emplace(entry, cpu_scheduler_);
        round.operation->useRecordingWorkers(recording_workers_);
        auto opened = round.operation->tryBegin();
        if (!opened || !*opened)
        {
            round.operation.reset();
            release_pins();
            if (!opened)
            {
                for (const auto& output : round.outputs)
                    runtime_->jobs_.find(output.job)->control.rejectUnadmitted(opened.error());
            }
            return false;
        }
        const auto context = round.operation->context();
        round.frame = context;
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        round.opened_at = std::chrono::steady_clock::now();
        rounds_opened_.fetch_add(1, std::memory_order_relaxed);
#endif
        round.stage = DeviceRound::EStage::ACQUIRING_OUTPUTS;
        for (auto& output : round.outputs)
        {
            output.submitted = false;
            auto& job = *runtime_->jobs_.find(output.job);
            auto& control = job.control;
            if (!control.tryAdmit(context.frame_index, context.slot_index)) std::terminate();
            if (output.execution_generation != control.executionGeneration()) std::terminate();
            round.instances.push_back(&job.execution->instance);
            ++round.run_count;
        }
        if (auto applied = selectGraphInputs(entry, round.slot()); !applied) round.error = applied.error();
        return true;
    }

    void RenderServer::Impl::finishFrame(detail::DeviceEntry& entry, RenderError error) noexcept
    {
        auto& round = entry.round;
        const auto slot = round.slot();
        if (round.operation->active())
        {
            round.operation->cancel(error);
        }
        round.operation.reset();
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        const auto held_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - round.opened_at
        ).count();
        slot_cpu_hold_ns_.fetch_add(held_ns, std::memory_order_relaxed);
        const auto reads = entry.frameWorkspace().submissions.takeRasterReads();
        raster_buffer_ranges_.fetch_add(reads.buffer_ranges, std::memory_order_relaxed);
        raster_buffer_bytes_.fetch_add(reads.buffer_bytes, std::memory_order_relaxed);
        raster_image_ranges_.fetch_add(reads.image_ranges, std::memory_order_relaxed);
        raster_image_subresources_.fetch_add(reads.image_subresources, std::memory_order_relaxed);
        raster_geometry_ranges_.fetch_add(reads.geometry_ranges, std::memory_order_relaxed);
        raster_geometry_bytes_.fetch_add(reads.geometry_bytes, std::memory_order_relaxed);
#endif
        round.error = {};
        const auto completed = std::exchange(round.run_count, 0);
        const auto failure = error.type ? error : RenderError{kJobState};
        for (auto& output : std::span{round.outputs}.first(completed))
        {
            auto& job = *runtime_->jobs_.find(output.job);
            runtime_->resources_.unpinBufferUses(job.execution->productUses(), slot);
            job.last_run_error = output.submitted ? RenderError{} : failure;
            output.completion = job.control.finishRun(
                output.execution_generation, output.submitted ? RenderResult<void>{} : cxx::unexpected(failure)
            );
            retireStoppedExecution(job);
        }
        round.instances.clear();
        round.clearReadSelections();
        for (auto& output : std::span{round.outputs}.first(completed))
        {
            if (auto completion = std::exchange(output.completion, std::nullopt))
                std::move(*completion).deliver();
        }
    }

    bool RenderServer::Impl::progressProgramPreparations(detail::DeviceEntry& entry) noexcept
    {
        auto& round = entry.round;
        if (round.operation || stop_requested_.load() || entry.admissionError().type) return false;

        if (entry.pendingProgramBindings().empty()) return false;
        bool progressed = false;
        for (const auto binding : entry.pendingProgramBindings())
        {
            auto* job = runtime_->jobs_.find(binding.job);
            if (!job || !job->execution || job->execution->binding_identity != binding) continue;
            if (!job->control.eligible() || !job->control.hasRequest()) continue;
            auto& programs = job->execution->programs;
            const auto error = programs.reportableError();
            if (error.type && programs.nextBuild() == kGraphInvalidIndex)
            {
                job->control.rejectUnadmitted(error);
                progressed = true;
                break;
            }
        }
        if (entry.program_update) return progressed;
        const auto pending = entry.pendingProgramBindings();
        for (std::size_t offset = 0; offset < pending.size(); ++offset)
        {
            const auto index = (round.preparation_cursor + offset) % pending.size();
            const auto binding = pending[index];
            auto* job = runtime_->jobs_.find(binding.job);
            if (!job || !job->execution || job->execution->binding_identity != binding) continue;
            if (!job->readyToRun() || !jobTargetReady(*job)) continue;
            auto& execution = *job->execution;
            const auto build = execution.programs.nextBuild();
            if (build == kGraphInvalidIndex) continue;
            round.preparation_cursor = (index + 1) % pending.size();
            startProgramPreparation(entry, execution, build);
            return true;
        }
        return progressed;
    }

    void RenderServer::Impl::startProgramPreparation(
        detail::DeviceEntry& entry, JobExecution& execution, std::uint32_t build
    ) noexcept
    {
        entry.program_update = std::make_unique<detail::RasterProgramUpdate>(
            entry.session(), runtime_->resources_, execution.binding_identity
        );
        auto& update = *entry.program_update;
        update.build = build;
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        program_preparations_.fetch_add(1, std::memory_order_relaxed);
#endif
        // All user callbacks, snapshots and graph linkage finish on the owner before the operation leaves it.
        if (auto selected = execution.programs.select(update); !selected)
        {
            update.returned = selected.error();
            return;
        }
        const bool needs_native = std::ranges::any_of(update.streams, [](const auto& stream) noexcept
        {
            return !stream.programs.empty();
        });
        if (!needs_native)
        {
            update.returned = RenderError{};
            (void)settleProgramCompilation(entry);
            return;
        }
        const auto scheduler = server_context_.get_scheduler(cxx::execution::ScheduleMode::delivery);
        auto prepared = recording_workers_.beginCompilation() |
            stdexec::let_value([op = &update, cpu = program_scheduler_]() noexcept { return op->compile(cpu); }) |
            stdexec::upon_error([](std::exception_ptr) noexcept -> RenderResult<void> { std::terminate(); }) |
            stdexec::upon_stopped([]() noexcept -> RenderResult<void>
            {
                return cxx::unexpected(RenderError{kCommandCancelled});
            }) |
            stdexec::continues_on(scheduler) |
            stdexec::upon_error([](cxx::execution::ScheduleError) noexcept -> RenderResult<void>
            {
                std::terminate();
            }) |
            stdexec::then([this, op = &update](RenderResult<void> result) noexcept
            {
                recording_workers_.endCompilation();
                op->returned = result ? RenderError{} : result.error();
            });
        stdexec::spawn(
            std::move(prepared), command_scope_.get_token(),
            stdexec::env{stdexec::prop{stdexec::get_scheduler, scheduler}}
        );
    }

    bool RenderServer::Impl::settleProgramCompilation(detail::DeviceEntry& entry) noexcept
    {
        auto& op = entry.program_update;
        if (!op) return false;
        auto* job = runtime_->jobs_.find(op->binding.job);
        const bool installed = job && job->execution && job->execution->device == entry.id &&
            job->execution->binding_identity == op->binding;
        auto* execution = installed ? job->execution.get() : nullptr;
        const bool running = installed && job->control.running();
        const bool stale = running && !execution->programs.current(*op);
        if (!running || stale || stop_requested_.load() || entry.admissionError().type) op->stop.request_stop();
        if (!op->returned || !op->publishReturnedUses()) return false;
        const bool discarded = op->stop.stop_requested();
        if (running && !discarded && execution->programs.inputsBusy(op->build)) return false;
        const bool installs_bindings = running && !discarded && !op->returned->type;
        if (installs_bindings && executionTargetsResizing(*execution)) return false;
        if (execution && !discarded) execution->settlePrograms(*op);
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - op->started
        ).count();
        program_preparation_ns_.fetch_add(ns, std::memory_order_relaxed);
        if (discarded) discarded_preparations_.fetch_add(1, std::memory_order_relaxed);
#endif
        op.reset();
        return true;
    }

    void RenderServer::Impl::submitFrameRendering(detail::DeviceEntry& entry) noexcept
    {
        auto& round = entry.round;
        round.pending = true;
        const auto delivery = server_context_.get_scheduler(cxx::execution::ScheduleMode::delivery);
        stdexec::spawn(
            stdexec::starts_on(delivery, submitCurrentFrame(entry)) |
                stdexec::upon_error([](auto) noexcept -> RenderResult<void> { std::terminate(); }) |
                stdexec::upon_stopped(
                    []() noexcept -> RenderResult<void> { return cxx::unexpected(RenderError{kCommandCancelled}); }
                ) |
                stdexec::continues_on(delivery) |
                stdexec::upon_error([](auto) noexcept -> RenderResult<void> { std::terminate(); }) |
                stdexec::then(
                    [this, &entry](RenderResult<void> result) noexcept
                    {
                        auto& round = entry.round;
                        round.pending = false;
                        if (!result && !round.error.type) round.error = result.error();
                        if (round.error.type || !round.operation->active()) finishFrame(entry, round.error);
                    }
                ),
            command_scope_.get_token()
        );
    }

    bool RenderServer::Impl::progressFrame(detail::DeviceEntry& entry) noexcept
    {
        auto& round = entry.round;
        if (!round.operation)
        {
            return !stop_requested_.load() && !entry.admissionError().type && beginFrame(entry);
        }
        if (round.pending) return false;
        if (round.error.type)
        {
            finishFrame(entry, round.error);
            return true;
        }
        if (round.stage == DeviceRound::EStage::ACQUIRING_OUTPUTS)
        {
            const auto acquired = acquireFrameOutputs(entry, round.slot());
            if (!acquired)
            {
                finishFrame(entry, acquired.error());
                return true;
            }
            if (!*acquired) return false;
            round.stage = DeviceRound::EStage::SUBMITTING;
        }
        if (round.stage == DeviceRound::EStage::SUBMITTING && round.operation->canRun())
        {
            submitFrameRendering(entry);
            return true;
        }
        return false;
    }
} // namespace lux::render

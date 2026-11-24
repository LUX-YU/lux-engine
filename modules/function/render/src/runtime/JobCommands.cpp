#include <lux/engine/render/device/JobGpuResources.hpp>
#include <algorithm>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>

namespace lux::render
{
    void RenderServer::Impl::createJob(const CreateRenderJob::Request& request, JobCreated& reply) noexcept
    {
        auto* scene = runtime_->scenes_.find(request.scene);
        auto* view = scene ? scene->findView(request.view) : nullptr;
        if (!view || !runtime_->effects_.find(request.effect))
        {
            reply.error = {kJobIdentity};
            return;
        }
        if (!isValidJobCadence(request.cadence))
        {
            reply.error = {kJobState};
            return;
        }
        const auto id = runtime_->jobs_.emplace(RenderJobContent{request.scene, request.view, request.effect}, request.cadence);
        runtime_->jobs_.find(id)->setId(id);
        view->jobs.push_back(id);
        ++job_structure_revision_;
        reply = {{}, id};
    }

    RenderResult<std::vector<RenderJobId>>
    RenderServer::Impl::orderProductConsumers(const JobExecution& candidate) const noexcept
    {
        auto jobs = running_jobs_;
        const auto changed = candidate.binding_identity.job;
        if (std::ranges::find(jobs, changed) == jobs.end()) jobs.push_back(changed);
        const auto execution = [&](RenderJobId id) -> const JobExecution& {
            return id == changed ? candidate : *runtime_->jobs_.find(id)->execution;
        };
        FrameInputs::TargetSlots writers;
        for (std::uint32_t i = 0; i < jobs.size(); ++i) writers.insert(execution(jobs[i]).target, i);

        ProductOrder order;
        order.begin(jobs.size());
        for (std::uint32_t i = 0; i < jobs.size(); ++i)
        {
            for (const auto& input : execution(jobs[i]).target_images)
                if (input.read == EProductRead::CURRENT_RUN)
                    if (const auto* writer = writers.tryGet(input.target)) order.addRead(*writer, i);
        }
        if (!order.finish()) return cxx::unexpected(RenderError{kJobCycle});
        std::vector<RenderJobId> ordered;
        ordered.reserve(jobs.size());
        for (const auto i : order.ordered()) ordered.push_back(jobs[i]);
        return ordered;
    }

    void RenderServer::Impl::setJobCadence(const SetRenderJobCadence::Request& input, AckResponse& reply) noexcept
    {
        auto* job = runtime_->jobs_.find(input.job);
        if (!job) { reply.error = {kJobIdentity}; return; }
        if (job->running())
        {
            if (const auto failure = runtime_->device(job->execution->device)->admissionError(); failure.type)
            {
                reply.error = failure;
                return;
            }
        }
        const auto result = job->setCadence(input.cadence);
        if (result)
        {
            job->last_run_error = {};
        }
        reply.error = result ? RenderError{} : result.error();
    }

    void RenderServer::Impl::releaseExecutionResources(
        std::unique_ptr<JobExecution> execution, std::unique_ptr<detail::JobGpuResources> physical,
        std::vector<ResourceInstanceId>& resources
    ) noexcept
    {
        if (!physical || physical->generation != execution->binding_identity.generation) std::terminate();
        execution.reset();
        auto private_resources = std::move(physical->private_resources);
        physical.reset();
        resources.clear();
        ResourceSet::retireJob(std::move(private_resources));
    }

    void RenderServer::Impl::destroyExecution(
        std::unique_ptr<JobExecution> execution, std::unique_ptr<detail::JobGpuResources> physical,
        std::vector<ResourceInstanceId>& resources
    ) noexcept
    {
        if (!execution)
        {
            if (physical || !resources.empty()) std::terminate();
            return;
        }
        for (const auto& link : execution->projection_subscriptions)
            runtime_->resources_.unbindGraphProjection(link, execution->binding_identity);
        releaseExecutionResources(std::move(execution), std::move(physical), resources);
    }

    void RenderServer::Impl::retireStoppedExecution(RenderJob& job) noexcept
    {
        if (!job.running() && canChangeExecution(job))
        {
            if (job.execution)
            {
                auto* device = runtime_->device(job.execution->device);
                auto native = device->takeJobResources(job.execution->binding_identity);
                destroyExecution(std::move(job.execution), std::move(native), job.resources);
            }
            job.binding = {};
        }
    }

    void RenderServer::Impl::stopJob(RenderJob& job, RenderError cause) noexcept
    {
        const auto id = job.id();
        if (job.execution)
        {
            if (auto* resources = job.execution->privateResources()) resources->close();
            targetState(*runtime_->targets_.find(job.execution->target)).releaseWriter(id);
        }
        if (std::erase(running_jobs_, id) != 0) ++job_structure_revision_;
        job.closeAdmission();
        rejectPendingRun(job, cause);
        retireStoppedExecution(job);
    }

    void RenderServer::Impl::handleStopJob(const RenderJobId& id, AckResponse& reply) noexcept
    {
        auto* job = runtime_->jobs_.find(id);
        if (!job) { reply.error = {kJobIdentity}; return; }
        cancelDeferredRuns(id);
        stopJob(*job, {kJobStopped});
        releaseInputWindows();
        reply = {};
    }

    void RenderServer::Impl::removeJob(const RenderJobId& id, AckResponse& reply) noexcept
    {
        auto* job = runtime_->jobs_.find(id);
        if (!job) { reply.error = {kJobIdentity}; return; }
        if (!job->canRemove() || !canChangeExecution(*job))
        {
            reply.error = {kJobBusy};
            return;
        }
        auto& view = *runtime_->scenes_.find(job->content.scene)->findView(job->content.view);
        std::erase(view.jobs, id);
        retireStoppedExecution(*job);
        runtime_->jobs_.erase(id);
        ++job_structure_revision_;
        reply = {};
    }

    bool RenderServer::Impl::canChangeExecution(const RenderJob& job) const noexcept
    {
        if (job.hasRun() || job.hasRequest()) return false;
        if (!job.execution) return true;
        const auto& execution = *job.execution;
        return !executionTargetsResizing(execution);
    }

    bool RenderServer::Impl::executionTargetsResizing(const JobExecution& execution) const noexcept
    {
        if (targetState(*runtime_->targets_.find(execution.target)).pending_replacement.has_value()) return true;
        return std::ranges::any_of(execution.target_images, [&](const auto& input)
        {
            return targetState(*runtime_->targets_.find(input.target)).pending_replacement.has_value();
        });
    }

    RenderResult<void> RenderServer::Impl::validateJobBinding(
        const JobStartRequest& request, const RenderJob& job, const compiled::RenderGraph& graph,
        const VRenderTarget* replacement
    ) const noexcept
    {
        const auto device = request.device;
        const auto target_id = request.target;
        const auto* target = runtime_->targets_.find(target_id);
        if (!target) return cxx::unexpected(RenderError{kRuntimeIdentity});
        const bool wrong_device = targetState(*target).device != device ||
            (job.execution && job.execution->device != device);
        if (wrong_device) return cxx::unexpected(RenderError{kJobState});
        const auto& state = targetState(*target);
        if (!state.writer.isNull() && state.writer != job.id())
            return cxx::unexpected(RenderError{kTargetInUse});
        // The resize owner holds this writer across WSI retirement. Every ordinary install still waits for it.
        const bool owns_replacement = state.pending_replacement == request.effect && state.writer == job.id();
        const bool busy = replacement ? !owns_replacement || job.hasRun() || job.hasRequest()
                                      : state.pending_replacement.has_value() || !canChangeExecution(job);
        if (busy || hasDeferredRun(job.id()))
            return cxx::unexpected(RenderError{kJobBusy});
        if (frameUsesTarget(target_id)) return cxx::unexpected(RenderError{kJobBusy});
        if (!targetAvailable(replacement ? *replacement : *target)) return cxx::unexpected(RenderError{kRuntimeState});
        const bool writes_target = graph.output() != kGraphInvalidIndex;
        if (std::holds_alternative<detail::SwapchainTarget>(*target) && !writes_target)
            return cxx::unexpected(RenderError{kRuntimeUnsupported});
        if (auto valid = request.binding.inputs.viewport.validate(); !valid) return valid;
        return {};
    }

    AckResponse RenderServer::Impl::startJob(StartRenderJob::Request&& request) noexcept
    {
        const auto* job = runtime_->jobs_.find(request.job);
        if (!job) return AckResponse{RenderError{kJobIdentity}};
        return buildJobExecution({
            request.device, request.job, job->content.effect, request.target, std::move(request.binding),
            EJobStartMode::START
        });
    }

    AckResponse RenderServer::Impl::rebindJob(RebindRenderJobEffect::Request&& request) noexcept
    {
        auto* job = runtime_->jobs_.find(request.job);
        if (!job || !runtime_->effects_.find(request.effect)) return AckResponse{RenderError{kJobIdentity}};
        if (!job->running())
        {
            if (!job->canRemove() || !canChangeExecution(*job))
                return AckResponse{RenderError{kJobBusy}};
            if (!request.device.isNull() || !request.target.isNull() || request.binding)
                return AckResponse{RenderError{kJobState}};
            job->content.effect = request.effect;
            return AckResponse{};
        }
        const auto target = request.target.isNull() ? job->execution->target : request.target;
        auto binding = request.binding ? std::move(*request.binding) : job->binding;
        return buildJobExecution({
            request.device, request.job, request.effect, target, std::move(binding), EJobStartMode::REBIND
        });
    }

    AckResponse RenderServer::Impl::buildJobExecution(JobStartRequest request, VRenderTarget* replacement) noexcept
    {
        auto& [device, id, effect_id, target_id, binding, mode] = request;
        const bool rebind = mode == EJobStartMode::REBIND;
        auto& job = *runtime_->jobs_.find(id);
        auto* effect = runtime_->effects_.find(effect_id);
        if (!effect) return AckResponse{RenderError{kJobIdentity}};
        auto* entry = runtime_->device(device);
        if (!entry) return AckResponse{RenderError{kRuntimeIdentity}};
        if (const auto failure = entry->admissionError(); failure.type) return AckResponse{failure};
        if (stop_requested_.load()) return AckResponse{RenderError{kCommandCancelled}};
        const auto* device_graph = effect->graphForDevice(device);
        if (!device_graph) return AckResponse{RenderError{kJobState}};
        const auto* selected = device_graph->graph.get();
        if (auto valid = validateJobBinding(request, job, *selected, replacement); !valid)
            return AckResponse{valid.error()};
        const auto generation = job.reserveGeneration(rebind);
        if (!generation) return AckResponse{generation.error()};
        std::ranges::sort(binding.target_images, {}, &TargetImageInput::binding);
        std::ranges::sort(binding.resources, {}, &JobResourceInput::binding);
        const bool reads_own_run = std::ranges::any_of(binding.target_images, [&](const auto& image) noexcept
        {
            return image.target == target_id && image.read == EProductRead::CURRENT_RUN;
        });
        if (reads_own_run)
            return AckResponse{RenderError{kCommandContract}};
        auto& target = *runtime_->targets_.find(target_id);
        const auto output = targetOutput(replacement ? *replacement : target);
        const auto& description = output.description;
        if (effect->output != description.outputContract() || !selected->compatible(description.graphRequirements()))
            return AckResponse{RenderError{kDeviceArgument}};
        const JobExecution::Installation installation{device, effect_id, target_id, {id, *generation}};
        auto physical = entry->prepareJobResources(installation.identity);
        auto candidate = std::make_unique<JobExecution>(*physical, *selected, installation, output);
        candidate->device_graph = device_graph;
        ResourceRegistry::BindingChanges subscriptions;
        auto& registry = runtime_->resources_;
        registry.beginBindings(subscriptions);
        cxx::scope_exit discard([&]() noexcept { registry.endBindings(subscriptions); });
        cxx::scope_exit rollback([&]() noexcept
        {
            releaseExecutionResources(std::move(candidate), std::move(physical), subscriptions.resources);
        });
        const ViewScope scope{job.content.scene, job.content.view};
        auto resources = bindEffectResources(*candidate, subscriptions, scope, binding.resources);
        if (!resources) return AckResponse{resources.error()};
        if (auto assembled = assembleExecution(*candidate, subscriptions, binding, *resources); !assembled)
            return AckResponse{assembled.error()};

        auto ordered = orderProductConsumers(*candidate);
        if (!ordered) return AckResponse{ordered.error()};
        if (auto capacity = registry.reserveBindings(subscriptions); !capacity) return AckResponse{capacity.error()};

        std::ranges::sort(subscriptions.resources, {}, detail::resourceOrder);
        registry.collectFrameDependencies(subscriptions.resources, candidate->indirect_resources);
        // All fallible work has finished. The target and writer generation become visible on the same owner turn.
        if (replacement) adoptTarget(target, std::move(*replacement));
        auto retired_resources = entry->installJobResources(id, std::move(physical));
        auto retired = std::exchange(job.execution, std::move(candidate));
        rollback.release();
        job.publishGeneration(*generation);
        registry.publishBindings(subscriptions, job.execution->projection_subscriptions, job.resources);
        targetState(target).writer = id;
        if (retired && retired->target != target_id)
        {
            targetState(*runtime_->targets_.find(retired->target)).releaseWriter(id);
        }
        job.content.effect = effect_id;
        job.binding = std::move(binding);
        job.last_run_error = {};
        running_jobs_ = std::move(*ordered);
        ++job_structure_revision_;
        destroyExecution(std::move(retired), std::move(retired_resources), subscriptions.resources);
        return AckResponse{};
    }

    void RenderServer::Impl::updateJobInputs(const UpdateJobInputs::Request& input, AckResponse& reply) noexcept
    {
        auto* job = runtime_->jobs_.find(input.job);
        if (!job) { reply.error = {kJobIdentity}; return; }
        if (!job->running()) { reply.error = {kJobStopped}; return; }
        if (const auto failure = runtime_->device(job->execution->device)->admissionError(); failure.type)
        {
            reply.error = failure;
            return;
        }
        if (!canChangeExecution(*job)) { reply.error = {kJobBusy}; return; }
        if (auto valid = input.inputs.viewport.validate(); !valid) { reply.error = valid.error(); return; }
        auto& execution = *job->execution;
        if (auto applied = execution.setConditions(input.inputs.conditions); !applied)
        {
            reply.error = applied.error();
            return;
        }
        job->binding.inputs = input.inputs;
        execution.viewport = input.inputs.viewport;
        job->last_run_error = {};
        reply = {};
    }

    void RenderServer::Impl::invalidateHistory(const InvalidateHistory::Request& input, AckResponse& reply) noexcept
    {
        auto* job = runtime_->jobs_.find(input.job);
        if (!job) { reply.error = {kJobIdentity}; return; }
        if (!job->running()) { reply.error = {kJobStopped}; return; }
        if (const auto failure = runtime_->device(job->execution->device)->admissionError(); failure.type)
        {
            reply.error = failure;
            return;
        }
        if (!canChangeExecution(*job)) { reply.error = {kJobBusy}; return; }
        const auto result = job->execution->invalidateHistory(input.previous);
        reply.error = result ? RenderError{} : result.error();
    }

    void RenderServer::Impl::rejectWindow(RequestRenderJob::Request& input, ResponseRoute route, RenderError error) noexcept
    {
        response_outbox_.emplaceResponse<JobSubmitted>(route, JobSubmitted{
            error, input.job, 0, 0, std::move(input.updates)
        });
        for (auto it = input.additional_runs.begin(); it != input.additional_runs.end(); ++it)
        {
            if (it->reply.isNull() || it->reply == route.request_id) continue;
            const auto duplicate = std::find_if(input.additional_runs.begin(), it, [&](const auto& earlier)
            {
                return earlier.reply == it->reply;
            });
            if (duplicate != it) continue;
            response_outbox_.emplaceResponse<JobSubmitted>({route.requester_id, it->reply}, JobSubmitted{error, it->job});
        }
    }

    void RenderServer::Impl::requestJob(RequestRenderJob::Request&& input, ResponseRoute route) noexcept
    {
        DeviceSessionId device;
        RenderError error;
        for (auto it = input.additional_runs.begin(); it != input.additional_runs.end(); ++it)
        {
            const bool repeats_first = it->job == input.job || it->reply == route.request_id;
            const bool repeats_peer = std::any_of(input.additional_runs.begin(), it, [&](const auto& earlier)
            {
                return earlier.job == it->job || earlier.reply == it->reply;
            });
            if (it->reply.isNull() || repeats_first || repeats_peer) error = {kCommandContract};
        }
        input.forEachRun(route.request_id, [&](const RequestRenderJob::Run& run) noexcept
        {
            if (error.type) return;
            const auto* job = runtime_->jobs_.find(run.job);
            if (!job) error = {kJobIdentity};
            else if (!job->running() || !targetReadyFor(*runtime_->targets_.find(job->execution->target), run.job))
                error = {kJobStopped};
            else if (job->hasRequest()) error = {kJobBusy};
            else if (!device.isNull() && device != job->execution->device) error = {kJobState};
            else
            {
                device = job->execution->device;
                error = runtime_->device(device)->admissionError();
            }
        });
        if (!error.type && inputQueueFull()) error = {kRuntimeCapacity};
        if (error.type)
        {
            rejectWindow(input, route, error);
            return;
        }
        const bool grouped = !input.additional_runs.empty();
        auto& window = input_windows_.emplace_back(
            route.requester_id, RequestRenderJob::Run{input.job, route.request_id},
            std::move(input.additional_runs)
        );
        runtime_->jobs_.find(input.job)->request(response_outbox_, route, std::move(input.updates), grouped);
        for (const auto& run : window.additional_runs)
            runtime_->jobs_.find(run.job)->request(response_outbox_, {route.requester_id, run.reply}, {}, true);
    }

    void RenderServer::Impl::rejectPendingRun(RenderJob& job, RenderError error) noexcept
    {
        if (!job.waitsForAdoption())
        {
            job.rejectUnadmitted(error);
            return;
        }
        // Before adoption, every Run needs the same update window. After adoption each Job owns its reply.
        for (const auto& window : input_windows_)
        {
            if (window.additional_runs.empty()) continue;
            const auto matches = [&](const auto& run) noexcept
            {
                return run.job == job.id() && job.awaits({window.requester, run.reply});
            };
            if (!matches(window.lead) && !std::ranges::any_of(window.additional_runs, matches)) continue;
            window.forEachRun([&](const auto& run) noexcept
            {
                auto* member = runtime_->jobs_.find(run.job);
                if (member && member->awaits({window.requester, run.reply})) member->rejectUnadmitted(error);
            });
            return;
        }
        job.rejectUnadmitted(error);
    }

    void RenderServer::Impl::bindJobCommands() noexcept
    {
        bindFrameworkCommand<CreateRenderJob, &Impl::createJob>();
        bindFrameworkCommand<StartRenderJob, &Impl::startJob>();
        bindFrameworkCommand<StopRenderJob, &Impl::handleStopJob>();
        bindFrameworkCommand<RemoveRenderJob, &Impl::removeJob>();
        bindFrameworkCommand<RebindRenderJobEffect, &Impl::rebindJob>();
        bindFrameworkCommand<SetRenderJobCadence, &Impl::setJobCadence>();
        run_command_ = runtime_->command_registry_.registerRoutedCommand<RequestRenderJob, &Impl::requestJob>(*this);
        bindFrameworkCommand<UpdateJobInputs, &Impl::updateJobInputs>();
        bindFrameworkCommand<InvalidateHistory, &Impl::invalidateHistory>();
    }

} // namespace lux::render

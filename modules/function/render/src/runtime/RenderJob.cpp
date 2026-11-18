#include <algorithm>
#include <exec/create.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/graph/compile/GraphCompiler.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>

namespace lux::render
{
    void RenderServer::Impl::bindJobCommands() noexcept
    {
        bindFrameworkCommand<CreateRenderJob, &Impl::createJob>();
        bindFrameworkCommand<StartRenderJob, &Impl::startJob>();
        bindFrameworkCommand<StopRenderJob, &Impl::handleStopJob>();
        bindFrameworkCommand<RemoveRenderJob, &Impl::removeJob>();
        bindFrameworkCommand<RebindRenderJobEffect, &Impl::rebindJob>();
        bindFrameworkCommand<SetRenderJobCadence, &Impl::setJobCadence>();
        bindFrameworkCommand<SetRenderJobDependencies, &Impl::setJobDependencies>();
        bindFrameworkCommand<RequestRenderJob, &Impl::requestJob>();
        bindFrameworkCommand<UpdateJobInputs, &Impl::updateJobInputs>();
        bindFrameworkCommand<InvalidateHistory, &Impl::invalidateHistory>();
    }

    void RenderServer::Impl::createJob(const CreateRenderJob::Request& request, JobCreated& reply) noexcept
    {
        auto* scene = domain_->scenes_.find(request.scene);
        auto* view = scene ? scene->views.find(request.view) : nullptr;
        if (!view || !domain_->effects_.find(request.effect))
        {
            reply.error = {kJobIdentity};
            return;
        }
        if (!isValidJobCadence(request.cadence))
        {
            reply.error = {kJobState};
            return;
        }
        const auto id = domain_->jobs_.emplace(RenderJobContent{request.scene, request.view, request.effect}, request.cadence);
        domain_->jobs_.find(id)->control.setId(id);
        view->jobs.push_back(id);
        ++job_structure_revision_;
        reply = {{}, id};
    }

    std::vector<detail::JobRelationEntry> RenderServer::Impl::jobRelations() noexcept
    {
        std::vector<detail::JobRelationEntry> entries;
        entries.reserve(domain_->jobs_.size());
        for (auto& job : domain_->jobs_)
        {
            entries.push_back({job.control.id(), &domain_->scenes_.find(job.content.scene)->device, &job.relations});
        }
        return entries;
    }

    void RenderServer::Impl::setJobDependencies(const SetRenderJobDependencies::Request& input, AckResponse& reply)
        noexcept
    {
        const auto entries = jobRelations();
        const auto changed = detail::setJobDependencies(
            entries, input.job, input.predecessors, input.successors, running_jobs_
        );
        if (changed) ++job_structure_revision_;
        reply.error = changed ? RenderError{} : changed.error();
    }

    void RenderServer::Impl::setJobCadence(const SetRenderJobCadence::Request& input, AckResponse& reply) noexcept
    {
        auto* job = domain_->jobs_.find(input.job);
        if (!job) { reply.error = {kJobIdentity}; return; }
        const auto result = job->control.setCadence(input.cadence);
        if (result)
        {
            job->last_run_error = {};
        }
        reply.error = result ? RenderError{} : result.error();
    }

    void RenderServer::Impl::retireStoppedExecution(RenderJob& job) noexcept
    {
        if (!job.control.running() && !job.hasExecutionBorrows())
        {
            job.execution.reset();
        }
    }

    void RenderServer::Impl::stopJob(RenderJob& job) noexcept
    {
        const auto id = job.control.id();
        if (const auto* starting = job.control.pendingStart())
        {
            if (auto* target = domain_->targets_.find(starting->target()))
            {
                auto& state = targetState(*target);
                if (state.reservation == starting) state.reservation = nullptr;
            }
        }
        if (job.execution)
        {
            if (auto& active = job.execution->programs.active) active->stop.request_stop();
            targetState(*domain_->targets_.find(job.execution->target)).releaseWriter(id);
        }
        if (std::erase(running_jobs_, id) != 0) ++job_structure_revision_;
        job.control.stop();
        retireStoppedExecution(job);
    }

    void RenderServer::Impl::handleStopJob(const RenderJobId& id, AckResponse& reply) noexcept
    {
        auto* job = domain_->jobs_.find(id);
        if (!job) { reply.error = {kJobIdentity}; return; }
        stopJob(*job);
        reply = {};
    }

    void RenderServer::Impl::removeJob(const RenderJobId& id, AckResponse& reply) noexcept
    {
        auto* job = domain_->jobs_.find(id);
        if (!job) { reply.error = {kJobIdentity}; return; }
        if (!job->control.canRemove() || job->hasExecutionBorrows())
        {
            reply.error = {kJobBusy};
            return;
        }
        for (auto previous : job->relations.predecessors)
        {
            std::erase(domain_->jobs_.find(previous)->relations.successors, id);
        }
        for (auto next : job->relations.successors)
        {
            std::erase(domain_->jobs_.find(next)->relations.predecessors, id);
        }
        auto& view = *domain_->scenes_.find(job->content.scene)->views.find(job->content.view);
        std::erase(view.jobs, id);
        domain_->jobs_.erase(id);
        ++job_structure_revision_;
        reply = {};
    }

    bool RenderServer::Impl::canChangeExecution(const RenderJob& job) const noexcept
    {
        if (job.hasExecutionBorrows() || job.control.hasRequest())
        {
            return false;
        }
        if (!job.execution)
        {
            return true;
        }
        auto& execution = *job.execution;
        const bool output_resizing = targetState(*domain_->targets_.find(execution.target)).resizing;
        const bool input_resizing = std::ranges::any_of(execution.target_images, [&](const auto& input)
        {
            return targetState(*domain_->targets_.find(input.target)).resizing;
        });
        return !output_resizing && !input_resizing;
    }

    RenderResult<void> RenderServer::Impl::validateJobBinding(
        const RenderJob& job,
        const RenderEffect& effect,
        TargetId target_id,
        const JobBinding& binding,
        const void* reservation
    ) const noexcept
    {
        const auto* target = domain_->targets_.find(target_id);
        if (!target) return cxx::unexpected(RenderError{kRuntimeIdentity});
        const auto& session = domain_->scenes_.find(job.content.scene)->device.session();
        if (!targetBelongsTo(*target, session)) return cxx::unexpected(RenderError{kJobState});
        const auto& state = targetState(*target);
        if (!state.writer.isNull() && state.writer != job.control.id())
            return cxx::unexpected(RenderError{kTargetInUse});
        if (state.resizing || state.reservation != reservation || !canChangeExecution(job))
            return cxx::unexpected(RenderError{kJobBusy});
        if (frameUsesTarget(target_id)) return cxx::unexpected(RenderError{kJobBusy});
        if (!targetAvailable(*target)) return cxx::unexpected(RenderError{kRuntimeState});
        const bool writes_target = std::ranges::any_of(effect.graph.outputs(),
            [](const auto& output) noexcept { return output.target.has_value(); });
        if (std::holds_alternative<detail::SwapchainTarget>(*target) && !writes_target)
            return cxx::unexpected(RenderError{kRuntimeUnsupported});
        if (auto valid = binding.inputs.viewport.validate(); !valid) return valid;
        for (const auto& input : binding.target_images)
        {
            const auto* source = domain_->targets_.find(input.target);
            if (!source || !targetDescription(*source).sampled)
                return cxx::unexpected(RenderError{kRuntimeIdentity});
            if (!targetBelongsTo(*source, session)) return cxx::unexpected(RenderError{kJobState});
            if (targetState(*source).resizing) return cxx::unexpected(RenderError{kDeviceBusy});
        }
        if (targetDependencyCycle(target_id, binding.target_images, job.control.id()))
            return cxx::unexpected(RenderError{kCommandContract});
        return {};
    }

    exec::task<AckResponse> RenderServer::Impl::startJob(StartRenderJob::Request&& request) noexcept
    {
        const auto* job = domain_->jobs_.find(request.job);
        if (!job) co_return AckResponse{RenderError{kJobIdentity}};
        co_return co_await buildJobExecution(
            request.job, job->content.effect, request.target, std::move(request.binding), false
        );
    }

    exec::task<AckResponse> RenderServer::Impl::rebindJob(RebindRenderJobEffect::Request&& request) noexcept
    {
        auto* job = domain_->jobs_.find(request.job);
        if (!job || !domain_->effects_.find(request.effect)) co_return AckResponse{RenderError{kJobIdentity}};
        if (!job->control.running())
        {
            if (!job->control.canRemove() || job->hasExecutionBorrows())
                co_return AckResponse{RenderError{kJobBusy}};
            if (!request.target.isNull() || request.binding)
                co_return AckResponse{RenderError{kJobState}};
            job->content.effect = request.effect;
            co_return AckResponse{};
        }
        const auto target = request.target.isNull() ? job->execution->target : request.target;
        auto binding = request.binding ? std::move(*request.binding) : job->binding;
        co_return co_await buildJobExecution(request.job, request.effect, target, std::move(binding), true);
    }

    exec::task<AckResponse> RenderServer::Impl::buildJobExecution(
        RenderJobId id, EffectId effect_id, TargetId target_id, JobBinding binding, bool rebind
    ) noexcept
    {
        auto& job = *domain_->jobs_.find(id);
        auto* effect = domain_->effects_.find(effect_id);
        if (!effect) co_return AckResponse{RenderError{kJobIdentity}};
        if (auto valid = validateJobBinding(job, *effect, target_id, binding, nullptr); !valid)
            co_return AckResponse{valid.error()};
        detail::JobControl::Start operation;
        if (auto begun = job.control.beginStart(target_id, rebind, operation); !begun)
            co_return AckResponse{begun.error()};
        targetState(*domain_->targets_.find(target_id)).reservation = &operation;
        cxx::scope_exit release_target([&]() noexcept
        {
            if (auto* target = domain_->targets_.find(target_id))
            {
                auto& state = targetState(*target);
                if (state.reservation == &operation) state.reservation = nullptr;
            }
        });
        if (!effect->tryBorrowExecution()) co_return AckResponse{RenderError{kRuntimeCapacity}};
        cxx::scope_exit release_effect([effect]() noexcept { effect->releaseExecution(); });
        auto& view = *domain_->scenes_.find(job.content.scene)->views.find(job.content.view);
        auto dependencies = resolveJobResources(*effect, view);
        if (!dependencies) co_return AckResponse{dependencies.error()};
        storages_created_.store(domain_->resources_.creations(), std::memory_order_relaxed);
        auto& entry = view.scene.device;
        auto plan = co_await detail::GraphCompiler::compile(
            entry.session(), cpu_scheduler_, effect->graph, effect->analysis, &effect->output,
            detail::ETransientAllocation::ALIAS_MEMORY, effect->parameter_groups
        );
        if (stop_requested_.load()) co_return AckResponse{RenderError{kCommandCancelled}};
        if (operation.cancelled()) co_return AckResponse{RenderError{kJobStopped}};
        if (!plan) co_return AckResponse{plan.error()};
        if (auto valid = validateJobBinding(job, *effect, target_id, binding, &operation); !valid)
            co_return AckResponse{valid.error()};
        if (!resourcesAvailable(*dependencies)) co_return AckResponse{RenderError{kDeviceBusy}};
        auto& target = *domain_->targets_.find(target_id);
        if (!plan->compatible(targetDescription(target))) co_return AckResponse{RenderError{kDeviceArgument}};
        auto candidate = std::make_unique<JobExecution>(
            *effect, entry.session(), entry.frameProgress().frameCount(), std::move(*plan), target_id, targetOutput(target)
        );
        release_effect.release();
        if (auto assembled = assembleExecution(*candidate, view, *dependencies, binding); !assembled)
            co_return AckResponse{assembled.error()};

        auto* previous = job.execution.get();
        const auto channels = effect->input_schema.channels();
        const auto same_input = [](const auto& a, const auto& b) noexcept
        {
            return a.identity == b.identity && std::ranges::equal(a.columns, b.columns);
        };
        const bool can_reuse_layout = previous && previous->target == target_id &&
            std::ranges::equal(previous->input_layout.channels(), channels) &&
            std::ranges::equal(previous->inputs, candidate->inputs, same_input);
        if (can_reuse_layout)
        {
            candidate->input_layout = previous->input_layout;
        }
        else
        {
            if (next_input_layout_revision_ == UINT64_MAX) co_return AckResponse{RenderError{kRuntimeCapacity}};
            auto layout = FrameLayout::create(channels, next_input_layout_revision_++);
            if (!layout) co_return AckResponse{layout.error()};
            candidate->input_layout = std::move(*layout);
        }
        if (auto published = operation.publish(); !published) co_return AckResponse{published.error()};
        targetState(target).writer = id;
        if (previous && previous->target != target_id)
        {
            targetState(*domain_->targets_.find(previous->target)).releaseWriter(id);
        }
        job.execution = std::move(candidate);
        job.content.effect = effect_id;
        job.binding = std::move(binding);
        job.last_run_error = {};
        if (!rebind)
        {
            running_jobs_.push_back(id);
            auto ordered = detail::orderRunningJobs(jobRelations(), running_jobs_);
            if (!ordered) std::terminate(); // The logical relations were validated before publication.
            running_jobs_ = std::move(*ordered);
        }
        ++job_structure_revision_;
        co_return AckResponse{};
    }

    void RenderServer::Impl::updateJobInputs(const UpdateJobInputs::Request& input, AckResponse& reply) noexcept
    {
        auto* job = domain_->jobs_.find(input.job);
        if (!job) { reply.error = {kJobIdentity}; return; }
        if (!job->control.running()) { reply.error = {kJobStopped}; return; }
        if (job->control.pendingStart() || !canChangeExecution(*job)) { reply.error = {kJobBusy}; return; }
        if (auto valid = input.inputs.viewport.validate(); !valid) { reply.error = valid.error(); return; }
        auto& execution = *job->execution;
        if (auto applied = execution.instance.setInputs(input.inputs.parameters, input.inputs.conditions); !applied)
        {
            reply.error = applied.error();
            return;
        }
        job->binding.inputs = input.inputs;
        execution.instance.setViewport(input.inputs.viewport);
        execution.camera_revision = 0;
        job->last_run_error = {};
        reply = {};
    }

    void RenderServer::Impl::invalidateHistory(const InvalidateHistory::Request& input, AckResponse& reply) noexcept
    {
        auto* job = domain_->jobs_.find(input.job);
        if (!job) { reply.error = {kJobIdentity}; return; }
        if (!job->control.running()) { reply.error = {kJobStopped}; return; }
        if (!canChangeExecution(*job)) { reply.error = {kJobBusy}; return; }
        const auto result = job->execution->instance.invalidateHistory(input.previous);
        reply.error = result ? RenderError{} : result.error();
    }

    bool RenderServer::Impl::jobTargetReady(const RenderJob& job) const noexcept
    {
        const auto& target = *domain_->targets_.find(job.execution->target);
        const auto& state = targetState(target);
        return state.enabled && !state.resizing && state.writer == job.control.id() && targetAvailable(target);
    }

    exec::task<JobSubmitted> RenderServer::Impl::requestJob(RequestRenderJob::Request&& input) noexcept
    {
        const auto id = input.job;
        std::optional<detail::JobControl::Request> request;
        co_return co_await exec::create<stdexec::set_value_t(JobSubmitted)>(
            [this, id, data = input.data, &request](auto& context) noexcept
            {
                auto* job = domain_->jobs_.find(id);
                if (!job || !job->control.running() || !jobTargetReady(*job))
                {
                    const auto error = job ? kJobStopped : kJobIdentity;
                    stdexec::set_value(std::move(context.receiver), JobSubmitted{RenderError{error}, id});
                    return;
                }
                auto& pending = request.emplace(
                    &context,
                    [](void* state, RenderResult<detail::JobReceipt> result) noexcept
                    {
                        auto& completed = *static_cast<std::remove_reference_t<decltype(context)>*>(state);
                        const JobSubmitted reply = result
                            ? JobSubmitted{{}, result->job_id, result->frame_index, result->slot_index}
                            : JobSubmitted{result.error()};
                        stdexec::set_value(std::move(completed.receiver), reply);
                    },
                    data
                );
                if (auto registered = job->control.request(pending); !registered)
                {
                    stdexec::set_value(std::move(context.receiver), JobSubmitted{registered.error(), id});
                    return;
                }
            }
        );
    }

} // namespace lux::render

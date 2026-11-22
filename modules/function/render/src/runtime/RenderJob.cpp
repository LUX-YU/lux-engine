#include <algorithm>
#include <exec/create.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/resource/view/ViewParameterStorage.hpp>
#include <lux/engine/render/resources/view/Resource.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>

namespace lux::render
{
    void RenderServer::Impl::createJob(const CreateRenderJob::Request& request, JobCreated& reply) noexcept
    {
        auto* scene = runtime_->scenes_.find(request.scene);
        auto* view = scene ? scene->views.find(request.view) : nullptr;
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
        runtime_->jobs_.find(id)->control.setId(id);
        view->jobs.push_back(id);
        ++job_structure_revision_;
        reply = {{}, id};
    }

    std::vector<detail::JobRelationEntry> RenderServer::Impl::jobRelations(const JobExecution* candidate) noexcept
    {
        std::vector<detail::JobRelationEntry> entries;
        entries.reserve(runtime_->jobs_.size());
        for (auto& job : runtime_->jobs_)
        {
            const auto id = job.control.id();
            const auto* execution = candidate && candidate->binding_identity.job == id ? candidate : job.execution.get();
            const auto device = execution ? execution->device : DeviceSessionId{};
            auto& entry = entries.emplace_back(detail::JobRelationEntry{id, device, &job.relations, {}});
            if (!execution || (execution != candidate && !job.control.running())) continue;
            const auto add_source = [&](RenderJobId writer)
            {
                if (runtime_->jobs_.find(writer) && std::ranges::find(entry.products, writer) == entry.products.end())
                    entry.products.push_back(writer);
            };
            for (const auto& image : execution->target_images)
            {
                const auto* target = runtime_->targets_.find(image.target);
                auto writer = target ? targetState(*target).writer : RenderJobId{};
                if (candidate && candidate->target == image.target) writer = candidate->binding_identity.job;
                else if (candidate && writer == candidate->binding_identity.job) writer = {};
                add_source(writer);
            }
            for (const auto& link : execution->storage_bindings)
                if (link.publication) add_source(link.publication->writer);
        }
        return entries;
    }

    void RenderServer::Impl::setJobDependencies(const SetRenderJobDependencies::Request& input, AckResponse& reply)
        noexcept
    {
        const auto entries = jobRelations();
        const auto changed = detail::setJobDependencies(entries, input, running_jobs_);
        if (changed) ++job_structure_revision_;
        reply.error = changed ? RenderError{} : changed.error();
    }

    void RenderServer::Impl::setJobCadence(const SetRenderJobCadence::Request& input, AckResponse& reply) noexcept
    {
        auto* job = runtime_->jobs_.find(input.job);
        if (!job) { reply.error = {kJobIdentity}; return; }
        if (job->control.running())
        {
            if (const auto failure = runtime_->device(job->execution->device)->admissionError(); failure.type)
            {
                reply.error = failure;
                return;
            }
        }
        const auto result = job->control.setCadence(input.cadence);
        if (result)
        {
            job->last_run_error = {};
        }
        reply.error = result ? RenderError{} : result.error();
    }

    RenderResult<void>
    RenderServer::Impl::retainExecutionResource(JobExecution& execution, ResourceInstanceId resource) noexcept
    {
        auto& resources = execution.resource_ids;
        if (std::ranges::find(resources, resource) != resources.end()) return {};
        if (auto retained = runtime_->resources_.retainExecution(resource); !retained) return retained;
        resources.push_back(resource);
        return {};
    }

    void RenderServer::Impl::destroyExecution(std::unique_ptr<JobExecution> execution) noexcept
    {
        if (!execution) return;
        auto resources = std::move(execution->resource_ids);
        for (const auto& link : execution->storage_bindings)
            if (!link.publication) runtime_->resources_.unbindGraphProjection(link, execution->binding_identity);
        execution.reset();
        for (const auto id : resources) runtime_->resources_.releaseExecution(id);
    }

    void RenderServer::Impl::retireStoppedExecution(RenderJob& job) noexcept
    {
        if (!job.control.running() && canChangeExecution(job))
        {
            destroyExecution(std::move(job.execution));
            job.binding = {};
        }
    }

    void RenderServer::Impl::stopJob(RenderJob& job) noexcept
    {
        const auto id = job.control.id();
        if (job.execution)
        {
            if (auto& stop = job.execution->programs.stop) stop->request_stop();
            targetState(*runtime_->targets_.find(job.execution->target)).releaseWriter(id);
        }
        if (std::erase(running_jobs_, id) != 0) ++job_structure_revision_;
        job.control.stop();
        if (auto* current = runtime_->jobs_.find(id)) retireStoppedExecution(*current);
    }

    void RenderServer::Impl::handleStopJob(const RenderJobId& id, AckResponse& reply) noexcept
    {
        auto* job = runtime_->jobs_.find(id);
        if (!job) { reply.error = {kJobIdentity}; return; }
        stopJob(*job);
        reply = {};
    }

    void RenderServer::Impl::removeJob(const RenderJobId& id, AckResponse& reply) noexcept
    {
        auto* job = runtime_->jobs_.find(id);
        if (!job) { reply.error = {kJobIdentity}; return; }
        if (!job->control.canRemove() || !canChangeExecution(*job))
        {
            reply.error = {kJobBusy};
            return;
        }
        for (auto previous : job->relations.predecessors)
        {
            std::erase(runtime_->jobs_.find(previous)->relations.successors, id);
        }
        for (auto next : job->relations.successors)
        {
            std::erase(runtime_->jobs_.find(next)->relations.predecessors, id);
        }
        auto& view = *runtime_->scenes_.find(job->content.scene)->views.find(job->content.view);
        std::erase(view.jobs, id);
        destroyExecution(std::move(job->execution));
        runtime_->jobs_.erase(id);
        ++job_structure_revision_;
        reply = {};
    }

    bool RenderServer::Impl::canChangeExecution(const RenderJob& job) const noexcept
    {
        if (job.control.hasRun() || job.control.hasRequest()) return false;
        if (!job.execution) return true;
        const auto& execution = *job.execution;
        const bool borrowed = execution.pending_uses || execution.programs.stop;
        return !borrowed && !executionTargetsResizing(execution);
    }

    bool RenderServer::Impl::executionTargetsResizing(const JobExecution& execution) const noexcept
    {
        if (targetState(*runtime_->targets_.find(execution.target)).resizing) return true;
        return std::ranges::any_of(execution.target_images, [&](const auto& input)
        {
            return targetState(*runtime_->targets_.find(input.target)).resizing;
        });
    }

    RenderResult<void> RenderServer::Impl::validateJobBinding(
        const JobStartRequest& request, const RenderJob& job, const compiled::RenderGraph& graph
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
        if (!state.writer.isNull() && state.writer != job.control.id())
            return cxx::unexpected(RenderError{kTargetInUse});
        if (state.resizing || !canChangeExecution(job))
            return cxx::unexpected(RenderError{kJobBusy});
        if (frameUsesTarget(target_id)) return cxx::unexpected(RenderError{kJobBusy});
        if (!targetAvailable(*target)) return cxx::unexpected(RenderError{kRuntimeState});
        const bool writes_target = graph.executable.output() != kGraphInvalidIndex;
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
        if (!job->control.running())
        {
            if (!job->control.canRemove() || !canChangeExecution(*job))
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

    AckResponse RenderServer::Impl::buildJobExecution(JobStartRequest request) noexcept
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
        const auto* selected = effect->graphForDevice(device);
        if (!selected) return AckResponse{RenderError{kJobState}};
        if (auto valid = validateJobBinding(request, job, *selected); !valid)
            return AckResponse{valid.error()};
        const auto qualify_view = [&]() noexcept -> RenderResult<detail::ViewParameterStorage*>
        {
            if (binding.view_parameters.isNull()) return nullptr;
            ResourceResponse owner;
            queryResource({device, ViewRequest{job.content.scene, job.content.view}, kViewParameterResourceName}, owner);
            if (owner.error.type) return cxx::unexpected(owner.error);
            if (owner.resource_id != binding.view_parameters)
                return cxx::unexpected(RenderError{kStorageMismatch});
            if (runtime_->resources_.hasExclusiveCommand(owner.resource_id))
                return cxx::unexpected(RenderError{kDeviceBusy});
            return runtime_->resources_.borrow<detail::ViewParameterStorage>(owner.resource_id);
        };
        auto relations = jobRelations();
        std::ranges::find(relations, id, &detail::JobRelationEntry::id)->device = device;
        if (auto valid = detail::validateJobDevices(relations); !valid) return AckResponse{valid.error()};
        const auto generation = job.control.reserveGeneration(rebind);
        if (!generation) return AckResponse{generation.error()};
        std::ranges::sort(binding.target_images, {}, &TargetImageInput::binding);
        std::ranges::sort(binding.resources, {}, &JobResourceInput::binding);
        const auto self = std::ranges::find(binding.target_images, target_id, &TargetImageInput::target);
        if (self != binding.target_images.end())
            return AckResponse{RenderError{kCommandContract}};
        const auto qualified_view = qualify_view();
        if (!qualified_view) return AckResponse{qualified_view.error()};
        auto* view_parameters = *qualified_view;
        auto& target = *runtime_->targets_.find(target_id);
        if (!selected->executable.compatible(targetDescription(target)))
            return AckResponse{RenderError{kDeviceArgument}};
        auto resources = bindEffectResources(*entry, selected->storage_imports, binding.resources);
        if (!resources) return AckResponse{resources.error()};
        const JobExecution::Installation installation{effect_id, target_id, {id, *generation}};
        auto candidate = std::make_unique<JobExecution>(*entry, *selected, installation, targetOutput(target));
        cxx::scope_exit rollback([&]() noexcept { destroyExecution(std::move(candidate)); });
        candidate->view_parameters = view_parameters;
        if (auto assembled = assembleExecution(*candidate, binding, *effect, *resources); !assembled)
            return AckResponse{assembled.error()};

        if (view_parameters)
        {
            if (auto retained = retainExecutionResource(*candidate, binding.view_parameters); !retained)
                return AckResponse{retained.error()};
        }
        if (auto layout = buildJobInputLayout(job, *candidate); !layout) return AckResponse{layout.error()};

        auto running = running_jobs_;
        if (!rebind) running.push_back(id);
        auto ordered = detail::orderRunningJobs(jobRelations(candidate.get()), running);
        if (!ordered) return AckResponse{ordered.error()};
        auto* previous = job.execution.get();
        job.control.publishGeneration(*generation);
        candidate->instance.bindPreparationOwner(id, job.control.executionGeneration());
        targetState(target).writer = id;
        if (previous && previous->target != target_id)
        {
            targetState(*runtime_->targets_.find(previous->target)).releaseWriter(id);
        }
        auto retired = std::exchange(job.execution, std::move(candidate));
        rollback.release();
        job.content.effect = effect_id;
        job.binding = std::move(binding);
        job.last_run_error = {};
        running_jobs_ = std::move(*ordered);
        ++job_structure_revision_;
        destroyExecution(std::move(retired));
        return AckResponse{};
    }

    RenderResult<void> RenderServer::Impl::buildJobInputLayout(const RenderJob& job, JobExecution& candidate) noexcept
    {
        std::vector<FrameChannel> channels;
        auto& registry = runtime_->resources_;
        const ViewRequest scope{job.content.scene, job.content.view};
        struct FrameResource final
        {
            ResourceInstanceId id;
            ResourceTypeId type;
            const ResourceRegistration* factory;
        };
        std::vector<FrameResource> frame_resources;
        for (const auto id : candidate.retainedResources())
        {
            const auto type = registry.declarationOf(id);
            const auto* factory = registry.factory(type);
            if (factory->frame_call.channels.empty()) continue;
            ResourceResponse local;
            queryResource({candidate.device, scope, factory->descriptor->type}, local);
            // Named graph imports may read a foreign scope. FrameData addresses the Job's own scope.
            if (local.resource_id == id) frame_resources.push_back({id, type, factory});
        }
        std::ranges::sort(frame_resources, {}, [](const FrameResource& resource) noexcept
        {
            return std::pair{resource.factory->descriptor->domain, resource.factory->descriptor->type.name()};
        });
        std::size_t extra_columns = 0;
        for (const auto& resource : frame_resources)
        {
            const auto count = resource.factory->frame_call.channels.size();
            if (count > UINT32_MAX - extra_columns)
                return cxx::unexpected(RenderError{kRuntimeCapacity});
            extra_columns += count;
        }
        channels.reserve(extra_columns);
        auto& columns = candidate.input_columns;
        columns.reserve(extra_columns);
        for (const auto& resource : frame_resources)
        {
            const auto& call = resource.factory->frame_call;
            const auto first = columns.size();
            for (const auto& channel : call.channels)
            {
                columns.push_back(static_cast<std::uint32_t>(channels.size()));
                channels.push_back(channel);
            }
            candidate.inputs.emplace_back(
                resource.id,
                resource.type,
                JobExecution::Input::ColumnRange{
                    static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(call.channels.size())
                }
            );
        }
        auto* previous = job.execution.get();
        const auto same_input = [](const auto& a, const auto& b) noexcept
        {
            return a.identity == b.identity && a.call_slot == b.call_slot && a.columns == b.columns;
        };
        const bool can_reuse_layout = previous && previous->target == candidate.target &&
            std::ranges::equal(previous->input_layout.channels(), channels) &&
            std::ranges::equal(previous->inputs, candidate.inputs, same_input);
        if (can_reuse_layout)
        {
            candidate.input_layout = previous->input_layout;
        }
        else
        {
            if (next_input_layout_revision_ == UINT64_MAX) return cxx::unexpected(RenderError{kRuntimeCapacity});
            auto layout = FrameLayout::create(channels, next_input_layout_revision_++);
            if (!layout) return cxx::unexpected(layout.error());
            layout->key_.server = instance_id_;
            candidate.input_layout = std::move(*layout);
        }
        return {};
    }


    void RenderServer::Impl::updateJobInputs(const UpdateJobInputs::Request& input, AckResponse& reply) noexcept
    {
        auto* job = runtime_->jobs_.find(input.job);
        if (!job) { reply.error = {kJobIdentity}; return; }
        if (!job->control.running()) { reply.error = {kJobStopped}; return; }
        if (const auto failure = runtime_->device(job->execution->device)->admissionError(); failure.type)
        {
            reply.error = failure;
            return;
        }
        if (!canChangeExecution(*job)) { reply.error = {kJobBusy}; return; }
        if (auto valid = input.inputs.viewport.validate(); !valid) { reply.error = valid.error(); return; }
        auto& execution = *job->execution;
        if (auto applied = execution.instance.setConditions(input.inputs.conditions); !applied)
        {
            reply.error = applied.error();
            return;
        }
        job->binding.inputs = input.inputs;
        execution.instance.setViewport(input.inputs.viewport);
        execution.view_revision = 0;
        job->last_run_error = {};
        reply = {};
    }

    void RenderServer::Impl::invalidateHistory(const InvalidateHistory::Request& input, AckResponse& reply) noexcept
    {
        auto* job = runtime_->jobs_.find(input.job);
        if (!job) { reply.error = {kJobIdentity}; return; }
        if (!job->control.running()) { reply.error = {kJobStopped}; return; }
        if (const auto failure = runtime_->device(job->execution->device)->admissionError(); failure.type)
        {
            reply.error = failure;
            return;
        }
        if (!canChangeExecution(*job)) { reply.error = {kJobBusy}; return; }
        const auto result = job->execution->instance.invalidateHistory(input.previous);
        reply.error = result ? RenderError{} : result.error();
    }

    bool RenderServer::Impl::jobTargetReady(const RenderJob& job) const noexcept
    {
        const auto& target = *runtime_->targets_.find(job.execution->target);
        const auto& state = targetState(target);
        return state.enabled && state.writer == job.control.id() && targetAvailable(target) &&
            !executionTargetsResizing(*job.execution);
    }

    auto RenderServer::Impl::requestJob(RequestRenderJob::Request&& input) noexcept
    {
        return exec::create<stdexec::set_value_t(JobSubmitted)>(
            [this, id = input.job, data = input.data, pending = detail::JobControl::Request{}]
            (auto& context) mutable noexcept
            {
                auto* job = runtime_->jobs_.find(id);
                if (!job || !job->control.running())
                {
                    const auto error = job ? kJobStopped : kJobIdentity;
                    stdexec::set_value(std::move(context.receiver), JobSubmitted{RenderError{error}, id});
                    return;
                }
                const auto failure = runtime_->device(job->execution->device)->admissionError();
                if (failure.type)
                {
                    stdexec::set_value(std::move(context.receiver), JobSubmitted{failure, id});
                    return;
                }
                if (!jobTargetReady(*job))
                {
                    stdexec::set_value(std::move(context.receiver), JobSubmitted{RenderError{kJobStopped}, id});
                    return;
                }
                pending.receiver = &context;
                pending.complete = [](void* state, RenderResult<detail::JobReceipt> result) noexcept
                {
                    auto& completed = *static_cast<std::remove_reference_t<decltype(context)>*>(state);
                    const JobSubmitted reply = result
                        ? JobSubmitted{{}, result->job_id, result->frame_index, result->slot_index}
                        : JobSubmitted{result.error()};
                    stdexec::set_value(std::move(completed.receiver), reply);
                };
                if (auto registered = job->control.request(pending); !registered)
                {
                    stdexec::set_value(std::move(context.receiver), JobSubmitted{registered.error(), id});
                    return;
                }
                // The connected sender owns the request credit at a stable address. Its start runs
                // during IPC dispatch, so no later mutation can overtake this adoption.
                if (data)
                {
                    const auto adopted = adoptFrameData(*job->execution, *data);
                    job->last_run_error = adopted ? RenderError{} : adopted.error();
                    if (!adopted) job->control.rejectUnadmitted(adopted.error());
                }
            }
        );
    }

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

} // namespace lux::render

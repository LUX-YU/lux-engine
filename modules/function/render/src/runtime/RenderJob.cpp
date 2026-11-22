#include <algorithm>
#include <exec/create.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/graph/compile/EffectCompileRequest.hpp>
#include <lux/engine/render/effect/EffectGraphBuild.hpp>
#include <lux/engine/render/graph/compile/GraphCompiler.hpp>
#include <lux/engine/render/resource/view/ViewParameterStorage.hpp>
#include <lux/engine/render/resources/mesh/Resource.hpp>
#include <lux/engine/render/resources/material/Resource.hpp>
#include <lux/engine/render/resources/texture/Resource.hpp>
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
            const auto* execution = candidate && candidate->storage_start.job == id ? candidate : job.execution.get();
            auto device = execution ? execution->device : DeviceSessionId{};
            if (const auto* start = job.control.pendingStart(); start && device.isNull())
            {
                if (const auto* output = runtime_->targets_.find(start->target)) device = targetState(*output).device;
            }
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
                if (candidate && candidate->target == image.target) writer = candidate->storage_start.job;
                else if (candidate && writer == candidate->storage_start.job) writer = {};
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
        const auto effect = execution->effect;
        const auto device = execution->device;
        const auto variant = execution->variant;
        auto resources = std::move(execution->resource_ids);
        for (const auto& link : execution->storage_bindings)
            if (!link.publication) runtime_->resources_.unbindGraphProjection(link, execution->storage_start);
        execution.reset();
        for (const auto id : resources) runtime_->resources_.releaseExecution(id);
        runtime_->device(device)->releaseVariant(variant);
        runtime_->effects_.find(effect)->releaseExecution();
    }

    void RenderServer::Impl::retireStoppedExecution(RenderJob& job) noexcept
    {
        if (!job.control.running() && !job.hasExecutionBorrows())
        {
            destroyExecution(std::move(job.execution));
        }
    }

    void RenderServer::Impl::stopJob(RenderJob& job) noexcept
    {
        const auto id = job.control.id();
        if (const auto* starting = job.control.pendingStart())
        {
            if (auto* target = runtime_->targets_.find(starting->target))
            {
                auto& state = targetState(*target);
                if (state.reservation == detail::TargetReservation{id, starting->serial}) state.reservation = {};
            }
        }
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
        if (!job->control.canRemove() || job->hasExecutionBorrows())
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
        if (job.hasExecutionBorrows() || job.control.hasRequest())
        {
            return false;
        }
        if (!job.execution)
        {
            return true;
        }
        auto& execution = *job.execution;
        const bool output_resizing = targetState(*runtime_->targets_.find(execution.target)).resizing;
        const bool input_resizing = std::ranges::any_of(execution.target_images, [&](const auto& input)
        {
            return targetState(*runtime_->targets_.find(input.target)).resizing;
        });
        return !output_resizing && !input_resizing;
    }

    RenderResult<void> RenderServer::Impl::validateJobBinding(
        DeviceSessionId device,
        const RenderJob& job,
        const RenderEffect& effect,
        TargetId target_id,
        const JobBinding& binding,
        detail::TargetReservation reservation
    ) const noexcept
    {
        const auto* target = runtime_->targets_.find(target_id);
        if (!target) return cxx::unexpected(RenderError{kRuntimeIdentity});
        const auto* owner = runtime_->device(device);
        if (!owner) return cxx::unexpected(RenderError{kRuntimeIdentity});
        if (const auto failure = owner->admissionError(); failure.type) return cxx::unexpected(failure);
        const bool wrong_device = targetState(*target).device != device ||
            (job.execution && job.execution->device != device);
        if (wrong_device) return cxx::unexpected(RenderError{kJobState});
        const auto& state = targetState(*target);
        if (!state.writer.isNull() && state.writer != job.control.id())
            return cxx::unexpected(RenderError{kTargetInUse});
        if (state.resizing || state.reservation != reservation || !canChangeExecution(job))
            return cxx::unexpected(RenderError{kJobBusy});
        if (frameUsesTarget(target_id)) return cxx::unexpected(RenderError{kJobBusy});
        if (!targetAvailable(*target)) return cxx::unexpected(RenderError{kRuntimeState});
        const bool writes_target = std::ranges::any_of(effect.source->outputs(), &EffectOutput::target);
        if (std::holds_alternative<detail::SwapchainTarget>(*target) && !writes_target)
            return cxx::unexpected(RenderError{kRuntimeUnsupported});
        if (auto valid = binding.inputs.viewport.validate(); !valid) return valid;
        return {};
    }

    exec::task<AckResponse> RenderServer::Impl::startJob(StartRenderJob::Request&& request) noexcept
    {
        const auto* job = runtime_->jobs_.find(request.job);
        if (!job) co_return AckResponse{RenderError{kJobIdentity}};
        co_return co_await buildJobExecution({
            request.device, request.job, job->content.effect, request.target, std::move(request.binding),
            EJobStartMode::START
        });
    }

    exec::task<AckResponse> RenderServer::Impl::rebindJob(RebindRenderJobEffect::Request&& request) noexcept
    {
        auto* job = runtime_->jobs_.find(request.job);
        if (!job || !runtime_->effects_.find(request.effect)) co_return AckResponse{RenderError{kJobIdentity}};
        if (!job->control.running())
        {
            if (!job->control.canRemove() || job->hasExecutionBorrows())
                co_return AckResponse{RenderError{kJobBusy}};
            if (!request.device.isNull() || !request.target.isNull() || request.binding)
                co_return AckResponse{RenderError{kJobState}};
            job->content.effect = request.effect;
            co_return AckResponse{};
        }
        const auto target = request.target.isNull() ? job->execution->target : request.target;
        auto binding = request.binding ? std::move(*request.binding) : job->binding;
        co_return co_await buildJobExecution({
            request.device, request.job, request.effect, target, std::move(binding), EJobStartMode::REBIND
        });
    }

    RenderResult<EffectImportContracts>
    RenderServer::Impl::qualifyEffectImports(const detail::DeviceEntry& entry, const JobBinding& binding) const noexcept
    {
        const auto inputs = std::span{binding.resources};
        if (inputs.size() >= kGraphInvalidIndex) return cxx::unexpected(RenderError{kRuntimeCapacity});
        auto& registry = runtime_->resources_;
        EffectImportContracts imports;
        imports.storages.reserve(inputs.size());
        for (std::uint32_t i = 0; i < inputs.size(); ++i)
        {
            const auto& input = inputs[i];
            if (input.publication && input.use != EJobResourceUse::GPU_BINDING)
                return cxx::unexpected(RenderError{kStorageContract, {i}});
            const bool is_duplicate = i != 0 && inputs[i - 1].binding == input.binding;
            if (input.binding.empty() || is_duplicate)
                return cxx::unexpected(RenderError{kCommandContract, {i}});
            const auto type = registry.declarationOf(input.resource);
            const auto* factory = registry.factory(type);
            if (!factory) return cxx::unexpected(RenderError{kStorageIdentity, {i}});
            if (registry.deviceOf(input.resource) != &entry.session())
                return cxx::unexpected(RenderError{kStorageMismatch, {i}});
            if (registry.hasExclusiveCommand(input.resource))
                return cxx::unexpected(RenderError{kDeviceBusy, {i}});
            const auto* ops = factory->storage()->bindings();
            const auto same_binding = std::ranges::find_if(inputs.first(i), [&](const JobResourceInput& earlier) noexcept
            {
                return earlier.resource == input.resource && earlier.projection == input.projection &&
                    earlier.use == input.use && earlier.publication == input.publication;
            });
            const auto canonical = static_cast<std::uint32_t>(same_binding - inputs.begin());
            const auto name = ResourceName{factory->descriptor->type.name()};
            const bool raster_textures = input.use == EJobResourceUse::RASTER_TEXTURES;
            const bool raster_meshes = input.use == EJobResourceUse::RASTER_MESHES;
            const bool raster_materials = input.use == EJobResourceUse::RASTER_MATERIALS;
            if (raster_textures || raster_meshes || raster_materials)
            {
                const auto expected = raster_textures ? TextureResource::descriptor.type :
                    raster_meshes ? MeshResource::descriptor.type : MaterialResource::descriptor.type;
                if (input.projection != 0 || factory->descriptor->type != expected)
                    return cxx::unexpected(RenderError{kStorageMismatch, {i}});
                const auto& enabled = entry.session().enabledFeatures();
                const bool unsupported = (raster_textures && !enabled.texture_descriptors) ||
                    (raster_materials && !enabled.buffer_descriptors);
                if (unsupported)
                    return cxx::unexpected(RenderError{kDeviceUnsupported, {i}});
                using Owner = EffectStorageImport::ERasterOwner;
                const auto kind = raster_textures ? Owner::TEXTURES :
                    raster_meshes ? Owner::MESHES : Owner::MATERIALS;
                imports.storages.push_back({input.binding, name, kind, canonical, 0});
                continue;
            }
            if (input.use == EJobResourceUse::CPU_SNAPSHOT)
            {
                const auto* snapshot = ops ? ops->cpu_snapshot : nullptr;
                const bool invalid_snapshot = !snapshot || !snapshot->create || !snapshot->capture ||
                    !snapshot->type.isValid() || snapshot->type.name().empty() || input.projection != 0;
                if (invalid_snapshot) return cxx::unexpected(RenderError{kStorageContract, {i}});
                EffectStorageImport::SnapshotContract contract{
                    snapshot->type.hash(), std::string{snapshot->type.name()}
                };
                imports.storages.push_back({input.binding, name, std::move(contract), canonical, input.projection});
                continue;
            }
            const bool invalid_binding = input.use != EJobResourceUse::GPU_BINDING || !ops ||
                !ops->resolveGraphImport || !ops->describeGraphImport;
            if (invalid_binding) return cxx::unexpected(RenderError{kStorageContract, {i}});
            auto contract = canonical == i || input.publication ?
                ops->describeGraphImport(registry.resourceInstance(input.resource), input.projection) :
                RenderResult<detail::StorageGraphImport>{
                    std::get<detail::StorageGraphImport>(imports.storages[canonical].contract)
                };
            if (!contract) return cxx::unexpected(contract.error());
            if (input.publication)
            {
                if (!contract->final_buffer_output) return cxx::unexpected(RenderError{kStorageContract, {i}});
                auto selected = registry.selectProduct(input.resource, input.projection, *input.publication);
                if (!selected) return cxx::unexpected(selected.error());
                const auto bytes = selected->size;
                contract->description = GraphBufferDesc{bytes};
                contract->initial_ranges = {GraphBufferRange{0, bytes}};
                contract->final_buffer_output = false;
            }
            // The Storage retains this boundary state through uploads and any replacement.
            const bool invalid_writer = contract->final_buffer_output &&
                (contract->read_use || !std::holds_alternative<GraphBufferDesc>(contract->description));
            const bool read_layout = contract->initial_use == EGraphUse::SAMPLED ||
                contract->initial_use == EGraphUse::TRANSFER_SOURCE;
            const bool invalid_concurrent_read = contract->concurrent_image_read &&
                (!std::holds_alternative<GraphImageDesc>(contract->description) ||
                    contract->read_use != contract->initial_use || !read_layout);
            if (invalid_writer || invalid_concurrent_read || contract->initial_use == EGraphUse::UNSPECIFIED)
                return cxx::unexpected(RenderError{kStorageContract, {i}});
            imports.storages.push_back({
                input.binding, name, std::move(*contract), kGraphInvalidIndex, input.projection
            });
        }
        return imports;
    }

    exec::task<detail::EffectVariantResult> RenderServer::Impl::acquireEffectVariant(
        EffectId id, detail::DeviceEntry& entry, EffectOutputDesc target,
        const EffectImportContracts& imports
    ) noexcept
    {
        if (auto* variant = entry.findVariant(id, target, imports))
        {
            if (variant->readers == UINT32_MAX) co_return cxx::unexpected(RenderError{kRuntimeCapacity});
            const auto identity = variant->id;
            ++variant->readers;
            if (variant->plan) co_return identity;
            auto result = co_await exec::create<stdexec::set_value_t(detail::EffectVariantResult)>(
                [variant](auto& context) noexcept
                {
                    variant->waiters.push_back({
                        &context,
                        [](void* receiver, detail::EffectVariantResult result) noexcept
                        {
                            auto& operation = *static_cast<std::remove_reference_t<decltype(context)>*>(receiver);
                            stdexec::set_value(std::move(operation.receiver), std::move(result));
                        }
                    });
                }
            );
            if (!result) entry.releaseVariant(identity);
            co_return result;
        }

        auto& variant = entry.beginVariant(id, target, imports);
        const auto identity = variant.id;
        const auto& effect = *runtime_->effects_.find(id);
        detail::EffectCompileRequest request{id, {*effect.source, effect.node_configurations}, {}, target, imports};
        request.inputs.reserve(effect.node_states.size());
        for (const auto& state : effect.node_states) request.inputs.push_back({state.node, state.type, state.bytes});
        auto compiled = co_await detail::GraphCompiler::compileEffect(
            entry, std::move(request), {effect_catalog_, cpu_scheduler_, config_.diagnostics, stop_requested_}
        );
        auto waiters = std::exchange(variant.waiters, {});
        const auto deliver = [&](const detail::EffectVariantResult& result) noexcept
        {
            // Keep the compiling reader through all completion callbacks, including reentrant Starts.
            for (const auto& waiter : waiters) waiter.complete(waiter.receiver, result);
        };
        if (!compiled)
        {
            variant.failure = compiled.error();
            deliver(cxx::unexpected(variant.failure));
            entry.releaseVariant(identity);
            co_return cxx::unexpected(compiled.error());
        }
        variant.plan = std::move(*compiled);
        compiled_graphs_.fetch_add(1, std::memory_order_relaxed);
        deliver(identity);
        co_return identity;
    }

    exec::task<AckResponse> RenderServer::Impl::buildJobExecution(JobStartRequest request) noexcept
    {
        auto& [device, id, effect_id, target_id, binding, mode] = request;
        const bool rebind = mode == EJobStartMode::REBIND;
        auto& job = *runtime_->jobs_.find(id);
        auto* effect = runtime_->effects_.find(effect_id);
        if (!effect) co_return AckResponse{RenderError{kJobIdentity}};
        if (auto valid = validateJobBinding(device, job, *effect, target_id, binding, {}); !valid)
            co_return AckResponse{valid.error()};
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
        if (auto qualified = qualify_view(); !qualified) co_return AckResponse{qualified.error()};
        auto relations = jobRelations();
        std::ranges::find(relations, id, &detail::JobRelationEntry::id)->device = device;
        if (auto valid = detail::validateJobDevices(relations); !valid) co_return AckResponse{valid.error()};
        const auto begun = job.control.beginStart(target_id, rebind);
        if (!begun) co_return AckResponse{begun.error()};
        const detail::TargetReservation reservation{id, *begun};
        targetState(*runtime_->targets_.find(target_id)).reservation = reservation;
        cxx::scope_exit finish_start([&]() noexcept
        {
            if (auto* target = runtime_->targets_.find(target_id))
            {
                auto& state = targetState(*target);
                if (state.reservation == reservation) state.reservation = {};
            }
            if (auto* current = runtime_->jobs_.find(id)) current->control.finishStart(reservation.start_serial);
        });
        if (auto retained = effect->retainExecution(); !retained) co_return AckResponse{retained.error()};
        cxx::scope_exit release_effect([effect]() noexcept { effect->releaseExecution(); });
        auto& entry = *runtime_->device(device);
        if (entry.pending_starts == UINT32_MAX) co_return AckResponse{RenderError{kRuntimeCapacity}};
        ++entry.pending_starts;
        cxx::scope_exit release_device([&entry]() noexcept { --entry.pending_starts; });
        const auto target_desc = targetDescription(*runtime_->targets_.find(target_id));
        const EffectOutputDesc output_contract{
            target_desc.format, target_desc.samples, target_desc.kind, target_desc.sampled
        };
        std::ranges::sort(binding.target_images, {}, &TargetImageInput::binding);
        std::ranges::sort(binding.resources, {}, &JobResourceInput::binding);
        auto imports = qualifyEffectImports(entry, binding);
        if (!imports) co_return AckResponse{imports.error()};
        const auto variant = co_await acquireEffectVariant(effect_id, entry, output_contract, *imports);
        cxx::scope_exit release_variant([&]() noexcept { if (variant) entry.releaseVariant(*variant); });
        if (stop_requested_.load()) co_return AckResponse{RenderError{kCommandCancelled}};
        if (!job.control.canPublishStart(reservation.start_serial))
            co_return AckResponse{RenderError{kJobStopped}};
        if (!variant) co_return AckResponse{variant.error()};
        if (auto valid = validateJobBinding(device, job, *effect, target_id, binding, reservation); !valid)
            co_return AckResponse{valid.error()};
        if (!imports->storages.empty())
        {
            auto current = qualifyEffectImports(entry, binding);
            if (!current) co_return AckResponse{current.error()};
            if (*current != *imports) co_return AckResponse{RenderError{kDeviceArgument}};
        }
        const auto self = std::ranges::find(binding.target_images, target_id, &TargetImageInput::target);
        if (self != binding.target_images.end())
            co_return AckResponse{RenderError{kCommandContract}};
        const auto qualified_view = qualify_view();
        if (!qualified_view) co_return AckResponse{qualified_view.error()};
        auto* view_parameters = *qualified_view;
        auto& target = *runtime_->targets_.find(target_id);
        const auto& selected = entry.readyVariant(*variant);
        if (!selected.plan->executable.compatible(targetDescription(target)))
            co_return AckResponse{RenderError{kDeviceArgument}};
        auto candidate = std::make_unique<JobExecution>(entry, selected, target_id, targetOutput(target));
        release_variant.release();
        release_effect.release();
        cxx::scope_exit rollback([&]() noexcept { destroyExecution(std::move(candidate)); });
        candidate->storage_start = reservation;
        candidate->view_parameters = view_parameters;
        if (auto assembled = assembleExecution(*candidate, binding, *effect); !assembled)
            co_return AckResponse{assembled.error()};

        if (view_parameters)
        {
            if (auto retained = retainExecutionResource(*candidate, binding.view_parameters); !retained)
                co_return AckResponse{retained.error()};
        }
        if (auto layout = buildJobInputLayout(job, *candidate); !layout) co_return AckResponse{layout.error()};

        auto running = running_jobs_;
        if (!rebind) running.push_back(id);
        auto ordered = detail::orderRunningJobs(jobRelations(candidate.get()), running);
        if (!ordered) co_return AckResponse{ordered.error()};
        auto* previous = job.execution.get();
        if (auto published = job.control.publishStart(reservation.start_serial); !published)
            co_return AckResponse{published.error()};
        candidate->instance.bindPreparationOwner(id, job.control.executionGeneration());
        targetState(target).writer = id;
        if (previous && previous->target != target_id)
        {
            targetState(*runtime_->targets_.find(previous->target)).releaseWriter(id);
        }
        destroyExecution(std::exchange(job.execution, std::move(candidate)));
        rollback.release();
        job.content.effect = effect_id;
        job.binding = std::move(binding);
        job.last_run_error = {};
        running_jobs_ = std::move(*ordered);
        ++job_structure_revision_;
        co_return AckResponse{};
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
        if (job->control.pendingStart() || !canChangeExecution(*job)) { reply.error = {kJobBusy}; return; }
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
        return state.enabled && !state.resizing && state.writer == job.control.id() && targetAvailable(target);
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

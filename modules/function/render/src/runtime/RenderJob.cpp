#include <algorithm>
#include <exec/create.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/NativeResult.hpp>
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

    std::vector<detail::JobRelationEntry> RenderServer::Impl::jobRelations() noexcept
    {
        std::vector<detail::JobRelationEntry> entries;
        entries.reserve(runtime_->jobs_.size());
        for (auto& job : runtime_->jobs_)
        {
            auto device = job.execution ? job.execution->device : DeviceSessionId{};
            if (const auto* start = job.control.pendingStart(); start && device.isNull())
            {
                if (const auto* output = runtime_->targets_.find(start->target())) device = targetState(*output).device;
            }
            entries.push_back({job.control.id(), device, &job.relations});
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
            if (auto* target = runtime_->targets_.find(starting->target()))
            {
                auto& state = targetState(*target);
                if (state.reservation == starting) state.reservation = nullptr;
            }
        }
        if (job.execution)
        {
            if (auto& active = job.execution->programs.active) active->stop.request_stop();
            targetState(*runtime_->targets_.find(job.execution->target)).releaseWriter(id);
        }
        if (std::erase(running_jobs_, id) != 0) ++job_structure_revision_;
        job.control.stop();
        retireStoppedExecution(job);
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
        const void* reservation
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
        for (const auto& input : binding.target_images)
        {
            if (input.binding.empty()) return cxx::unexpected(RenderError{kCommandContract});
            const auto* source = runtime_->targets_.find(input.target);
            if (!source || !targetDescription(*source).sampled)
                return cxx::unexpected(RenderError{kRuntimeIdentity});
            if (!std::holds_alternative<detail::OffscreenTarget>(*source))
                return cxx::unexpected(RenderError{kDeviceUnsupported});
            if (targetState(*source).device != device) return cxx::unexpected(RenderError{kJobState});
            if (targetState(*source).resizing) return cxx::unexpected(RenderError{kDeviceBusy});
        }
        if (targetDependencyCycle(target_id, binding.target_images, job.control.id()))
            return cxx::unexpected(RenderError{kCommandContract});
        return {};
    }

    exec::task<AckResponse> RenderServer::Impl::startJob(StartRenderJob::Request&& request) noexcept
    {
        const auto* job = runtime_->jobs_.find(request.job);
        if (!job) co_return AckResponse{RenderError{kJobIdentity}};
        co_return co_await buildJobExecution(
            request.device, request.job, job->content.effect, request.target, std::move(request.binding), false
        );
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
        co_return co_await buildJobExecution(request.device, request.job, request.effect, target, std::move(binding), true);
    }

    RenderResult<std::vector<EffectTargetImport>>
    RenderServer::Impl::qualifyEffectTargetImports(std::span<const TargetImageInput> inputs) const noexcept
    {
        if (inputs.size() >= kGraphInvalidIndex) return cxx::unexpected(RenderError{kRuntimeCapacity});
        std::vector<EffectTargetImport> imports;
        imports.reserve(inputs.size());
        for (std::uint32_t i = 0; i < inputs.size(); ++i)
        {
            const auto& input = inputs[i];
            if (i != 0 && inputs[i - 1].binding == input.binding)
                return cxx::unexpected(RenderError{kCommandContract, {i}});
            const auto* owner = runtime_->targets_.find(input.target);
            if (!owner) return cxx::unexpected(RenderError{kRuntimeIdentity, {i}});
            const auto& desc = targetDescription(*owner);
            // Distinct offscreen Target owners allocate distinct images. The same ID under
            // different names is one logical graph resource with one physical binding.
            const auto same_owner = std::ranges::find(inputs.first(i), input.target, &TargetImageInput::target);
            const auto canonical = same_owner == inputs.first(i).end() ?
                i : static_cast<std::uint32_t>(same_owner - inputs.begin());
            imports.push_back({input.binding, desc.format, desc.samples, canonical});
        }
        return imports;
    }

    RenderResult<EffectImportContracts>
    RenderServer::Impl::qualifyEffectImports(const detail::DeviceEntry& entry, const JobBinding& binding) const noexcept
    {
        auto targets = qualifyEffectTargetImports(binding.target_images);
        if (!targets) return cxx::unexpected(targets.error());
        const auto inputs = std::span{binding.resources};
        if (inputs.size() >= kGraphInvalidIndex) return cxx::unexpected(RenderError{kRuntimeCapacity});
        auto& registry = runtime_->resources_;
        EffectImportContracts imports{std::move(*targets), {}};
        imports.storages.reserve(inputs.size());
        for (std::uint32_t i = 0; i < inputs.size(); ++i)
        {
            const auto& input = inputs[i];
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
            const auto* ops = static_cast<const detail::StorageBindingOps*>(factory->storage->binding_ops);
            const auto same_binding = std::ranges::find_if(inputs.first(i), [&](const JobResourceInput& earlier) noexcept
            {
                return earlier.resource == input.resource && earlier.projection == input.projection &&
                    earlier.use == input.use;
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
            auto contract = canonical == i ?
                ops->describeGraphImport(registry.resourceInstance(input.resource), input.projection) :
                RenderResult<detail::StorageGraphImport>{
                    std::get<detail::StorageGraphImport>(imports.storages[canonical].contract)
                };
            if (!contract) return cxx::unexpected(contract.error());
            // The Storage retains this boundary state through uploads and any replacement.
            if (contract->initial_use == EGraphUse::UNSPECIFIED)
                return cxx::unexpected(RenderError{kStorageContract, {i}});
            imports.storages.push_back({input.binding, name, std::move(*contract), canonical, input.projection});
        }
        return imports;
    }

    exec::task<RenderEffect::VariantResult> RenderServer::Impl::compileEffectVariant(
        RenderEffect& effect, detail::DeviceEntry& entry, const EffectOutputDesc& target,
        const EffectImportContracts& imports
    ) noexcept
    {
        const auto& session = entry.session();
        const auto& features = session.enabledFeatures();
        const auto& limits = session.description();
        const auto& dispatch = limits.shaderStage.compute.dispatchMaxDim;
        const EffectBuildDevice device{
            features.shader_target, session.usesDeviceAddresses(), features.indirect_first_instance,
            {dispatch[0], dispatch[1], dispatch[2]}, limits.memory.storageBufferMaxRange,
            limits.dimensions.textureLayerMaxNum
        };
        const auto owner = co_await stdexec::read_env(stdexec::get_scheduler);
        auto built = co_await (
            stdexec::schedule(cpu_scheduler_) |
            stdexec::then([&]() noexcept
            {
                return lowerEffectGraph(
                    *effect.source, effect.node_configurations, effect_catalog_, target, device, imports,
                    config_.diagnostics
                );
            }) |
            stdexec::upon_stopped([]() noexcept -> RenderResult<EffectGraphBuild>
            {
                return cxx::unexpected(RenderError{kCpuStopped});
            }) |
            stdexec::continues_on(owner) |
            stdexec::upon_error(detail::NativeDeliveryFailure<EffectGraphBuild>{})
        );
        if (!built) co_return cxx::unexpected(built.error());
        if (stop_requested_.load()) co_return cxx::unexpected(RenderError{kCommandCancelled});
        if (const auto failure = entry.admissionError(); failure.type) co_return cxx::unexpected(failure);

        auto plan = co_await detail::GraphCompiler::compile(
            entry.session(), cpu_scheduler_, built->source, built->analysis, &built->output,
            detail::ETransientAllocation::ALIAS_MEMORY
        );
        if (!plan) co_return cxx::unexpected(plan.error());
        if (stop_requested_.load()) co_return cxx::unexpected(RenderError{kCommandCancelled});
        if (const auto failure = entry.admissionError(); failure.type) co_return cxx::unexpected(failure);

        for (auto& slot : built->target_imports)
        {
            if (slot.index == kGraphInvalidIndex) continue;
            const auto& resource = plan->executable.resources()[slot.index];
            if (!resource.memory_used) slot = {};
            else if (!resource.sampled_target_input)
                co_return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {slot.index}});
        }
        for (auto& slot : built->storage_imports)
        {
            if (slot.index != kGraphInvalidIndex && !plan->executable.resources()[slot.index].memory_used) slot = {};
        }
        auto preparation = linkPreparation(
            built->preparation, built->source, plan->assembly, plan->executable, plan->programs, effect
        );
        if (!preparation) co_return cxx::unexpected(preparation.error());
        std::vector<detail::LinkedUniformBlock> uniforms;
        std::vector<RenderEffect::UniformPool> pending_pools;
        for (const auto& block : built->uniforms)
        {
            if (!plan->executable.resources()[block.resource.index].memory_used) continue;
            if (!entry.uploader()) co_return cxx::unexpected(RenderError{kDeviceUnsupported});
            const auto state = std::ranges::find(effect.node_states, block.node, &RenderEffect::NodeState::node);
            if (state == effect.node_states.end() || state->type != block.source.state_type)
                co_return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {block.node.value, block.resource.index}});
            auto& pools = effect.uniform_pools;
            const auto existing = std::ranges::find_if(pools, [&](const auto& pool) noexcept
            {
                return pool.device == entry.id && pool.node == block.node && pool.name == block.name;
            });
            auto versions = existing == pools.end() ? nullptr : existing->versions;
            if (versions && !versions->matches(block.source))
                co_return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {block.node.value, block.resource.index}});
            if (!versions)
            {
                versions = std::make_shared<detail::UniformBlockVersions>(
                    entry.session(), state->value.get(), block.source, entry.frameProgress().frameCount()
                );
                pending_pools.push_back({entry.id, block.node, block.name, versions});
            }

            auto& linked = uniforms.emplace_back(std::move(versions), block.resource, std::vector<std::uint32_t>{});
            for (const auto& access : plan->executable.importedAccesses())
            {
                if (access.resource != block.resource) continue;
                linked.batches.push_back(access.batch);
                linked.batches.push_back(access.release_batch);
            }
            std::ranges::sort(linked.batches);
            const auto duplicates = std::ranges::unique(linked.batches);
            linked.batches.erase(duplicates.begin(), duplicates.end());
        }
        for (auto& pool : pending_pools) effect.uniform_pools.push_back(std::move(pool));
        const auto samplers = built->source.samplers();
        co_return std::make_shared<const JobGraphPlan>(JobGraphPlan{
            std::move(plan->executable), std::move(plan->programs), {samplers.begin(), samplers.end()},
            std::move(built->target_imports), std::move(built->storage_imports), std::move(*preparation),
            std::move(uniforms)
        });
    }

    exec::task<RenderEffect::VariantResult>
    RenderServer::Impl::acquireEffectVariant(
        RenderEffect& effect, detail::DeviceEntry& entry, EffectOutputDesc target,
        const EffectImportContracts& imports
    ) noexcept
    {
        // The Effect owns one immutable recipe and each DeviceSession has a fixed shader and queue contract.
        // Extents and physical target IDs are instance data. The import contract retains the alias pattern.
        auto& variants = effect.variants;
        std::erase_if(variants, [](const auto& variant) noexcept
        {
            return !variant.compiling && variant.plan.expired();
        });
        const auto found = std::ranges::find_if(variants, [&](const auto& variant) noexcept
        {
            return variant.device == entry.id && variant.target == target && variant.imports == imports;
        });
        if (found != variants.end())
        {
            if (auto plan = found->plan.lock()) co_return plan;
            auto pending = found->compiling;
            co_return co_await exec::create<stdexec::set_value_t(RenderEffect::VariantResult)>(
                [pending = std::move(pending)](auto& context) noexcept
                {
                    if (pending->result)
                    {
                        stdexec::set_value(std::move(context.receiver), *pending->result);
                        return;
                    }
                    pending->waiters.push_back({
                        &context,
                        [](void* receiver, RenderEffect::VariantResult result) noexcept
                        {
                            auto& operation = *static_cast<std::remove_reference_t<decltype(context)>*>(receiver);
                            stdexec::set_value(std::move(operation.receiver), std::move(result));
                        }
                    });
                }
            );
        }

        auto pending = std::make_shared<RenderEffect::VariantCompilation>();
        variants.push_back({entry.id, target, imports, pending, {}});
        RenderEffect::VariantResult result = cxx::unexpected(RenderError{kCommandCancelled});
        cxx::scope_exit settle([&]() noexcept
        {
            const auto building = std::ranges::find(variants, pending, &RenderEffect::Variant::compiling);
            if (building == variants.end()) std::terminate();
            pending->result.emplace(result);
            if (result)
            {
                building->plan = *result;
                building->compiling.reset();
                compiled_graphs_.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                variants.erase(building);
            }

            // Publish the terminal result before resuming any StartRenderJob. A completion
            // can re-enter the owner and the compiling Job may itself have been stopped.
            auto waiters = std::move(pending->waiters);
            for (const auto& waiter : waiters) waiter.complete(waiter.receiver, *pending->result);
        });
        result = co_await compileEffectVariant(effect, entry, target, imports);
        co_return std::as_const(result);
    }

    exec::task<AckResponse> RenderServer::Impl::buildJobExecution(
        DeviceSessionId device, RenderJobId id, EffectId effect_id, TargetId target_id, JobBinding binding, bool rebind
    ) noexcept
    {
        auto& job = *runtime_->jobs_.find(id);
        auto* effect = runtime_->effects_.find(effect_id);
        if (!effect) co_return AckResponse{RenderError{kJobIdentity}};
        if (auto valid = validateJobBinding(device, job, *effect, target_id, binding, nullptr); !valid)
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
        detail::JobControl::Start operation;
        if (auto begun = job.control.beginStart(target_id, rebind, operation); !begun)
            co_return AckResponse{begun.error()};
        targetState(*runtime_->targets_.find(target_id)).reservation = &operation;
        cxx::scope_exit release_target([&]() noexcept
        {
            if (auto* target = runtime_->targets_.find(target_id))
            {
                auto& state = targetState(*target);
                if (state.reservation == &operation) state.reservation = nullptr;
            }
        });
        auto effect_use = effect->borrowExecution();
        if (!effect_use) co_return AckResponse{effect_use.error()};
        auto& entry = *runtime_->device(device);
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
        auto graph_plan = co_await acquireEffectVariant(*effect, entry, output_contract, *imports);
        if (stop_requested_.load()) co_return AckResponse{RenderError{kCommandCancelled}};
        if (operation.cancelled()) co_return AckResponse{RenderError{kJobStopped}};
        if (!graph_plan) co_return AckResponse{graph_plan.error()};
        if (auto valid = validateJobBinding(device, job, *effect, target_id, binding, &operation); !valid)
            co_return AckResponse{valid.error()};
        if (!imports->targets.empty() || !imports->storages.empty())
        {
            auto current = qualifyEffectImports(entry, binding);
            if (!current) co_return AckResponse{current.error()};
            if (*current != *imports) co_return AckResponse{RenderError{kDeviceArgument}};
        }
        const auto qualified_view = qualify_view();
        if (!qualified_view) co_return AckResponse{qualified_view.error()};
        auto* view_parameters = *qualified_view;
        auto& target = *runtime_->targets_.find(target_id);
        if (!(*graph_plan)->executable.compatible(targetDescription(target)))
            co_return AckResponse{RenderError{kDeviceArgument}};
        auto candidate = std::make_unique<JobExecution>(
            std::move(*effect_use),
            runtime_->resources_,
            device,
            entry.session(),
            entry.frameProgress().frameCount(),
            std::move(*graph_plan),
            target_id,
            targetOutput(target)
        );
        candidate->view_parameters = view_parameters;
        if (auto assembled = assembleExecution(*candidate, binding); !assembled)
            co_return AckResponse{assembled.error()};

        std::vector<FrameChannel> channels;
        auto& registry = runtime_->resources_;
        const ViewRequest scope{job.content.scene, job.content.view};
        if (view_parameters)
        {
            if (auto retained = candidate->retainStorage(binding.view_parameters); !retained)
                co_return AckResponse{retained.error()};
        }
        struct FrameResource final
        {
            ResourceInstanceId id;
            const ResourceRegistration* factory;
        };
        std::vector<FrameResource> frame_resources;
        for (const auto id : candidate->retainedResources())
        {
            const auto* factory = registry.factory(registry.declarationOf(id));
            if (factory->frame_call.channels.empty()) continue;
            ResourceResponse local;
            queryResource({device, scope, factory->descriptor->type}, local);
            // Named graph imports may read a foreign scope. FrameData addresses the Job's own scope.
            if (local.resource_id == id) frame_resources.push_back({id, factory});
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
                co_return AckResponse{RenderError{kRuntimeCapacity}};
            extra_columns += count;
        }
        channels.reserve(extra_columns);
        auto& columns = candidate->input_columns;
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
            candidate->inputs.emplace_back(
                resource.id, registry.resourceInstance(resource.id), &call, std::span{columns}.subspan(first)
            );
        }
        auto* previous = job.execution.get();
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
            targetState(*runtime_->targets_.find(previous->target)).releaseWriter(id);
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

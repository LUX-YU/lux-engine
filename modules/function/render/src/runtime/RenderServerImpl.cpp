#include <exec/variant_sender.hpp>
#include <exception>
#include <lux/engine/render/execution/ResizeResources.hpp>
#include <lux/engine/render/extensions/device/DescriptorTable.hpp>
#include <lux/engine/render/extensions/storage/StorageBinding.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>
#include <map>
#include <ranges>
#include <set>

namespace lux::render
{
    RenderServer::Impl::Impl(RenderServerConfig value, ipc::IpcResponder input, execution::CpuScheduler cpu_scheduler)
        : config(std::move(value)), cpu_scheduler(std::move(cpu_scheduler)), responder(std::move(input)),
          render_loop_([](void*) noexcept { ipc::IpcCore::instance().notifyWork(); })
    {
        const auto query = bindFrameworkCommand<QueryCommandIds, &Impl::queryCommandIds>();
        if (query != QueryCommandIds::id) std::terminate(); // Fixed bootstrap identity is a protocol invariant.
        bindFrameworkCommand<CreateScene, &Impl::createScene>();
        bindFrameworkCommand<RemoveScene, &Impl::removeScene>();
        bindFrameworkCommand<CreateView, &Impl::createView>();
        bindFrameworkCommand<UpdateAttachmentInputs, &Impl::updateAttachmentInputs>();
        bindFrameworkCommand<InvalidateHistory, &Impl::invalidateHistory>();
        bindFrameworkCommand<RemoveView, &Impl::removeView>();
        bindFrameworkCommand<CreateTarget, &Impl::createTarget>();
        bindFrameworkCommand<ResizeTarget, &Impl::resizeTarget>();
        bindFrameworkCommand<RemoveTarget, &Impl::removeTarget>();
        bindFrameworkCommand<SetTargetActive, &Impl::setTargetActive>();
        bindCreateEffectCommand();
        bindFrameworkCommand<RemoveEffect, &Impl::removeEffect>();
        bindFrameworkCommand<Attach, &Impl::attachEffect>();
        bindFrameworkCommand<Detach, &Impl::detachEffect>();
        bindFrameworkCommand<Stop, &Impl::stopCommand>();
        bindFrameworkCommand<QueryRenderStats, &Impl::queryRenderStats>();
        bindFrameworkCommand<QueryExecutionProgress, &Impl::queryExecutionProgress>();
        bindFrameworkCommand<ListCapabilities, &Impl::listCapabilities>();
        bindFrameworkCommand<ValidateCapabilityConfiguration, &Impl::validateCapabilityConfiguration>();
        bindFrameworkCommand<ListCommands, &Impl::listCommands>();
        bindFrameworkCommand<ListCommandBindings, &Impl::listCommandBindings>();
        bindFrameworkCommand<SelectCommandBinding, &Impl::selectCommandBinding>();
        bindSubmitFrameCommand();
        frame_inputs_.resize(static_cast<std::size_t>(config.max_frames_ahead) + 1);
        submit_frame_command_ = *command_registry_.findCommand(commandDescriptor<SubmitFrame>());
        stop_command_ = *command_registry_.findCommand(commandDescriptor<Stop>());
        select_binding_command_ = *command_registry_.findCommand(commandDescriptor<SelectCommandBinding>());
    }

    void NRI_CALL RenderServer::Impl::diagnostic(
        nri::Message severity,
        const char* file,
        std::uint32_t line,
        const char* message,
        void* user
    ) noexcept
    {
        auto& state = *static_cast<Impl*>(user);
        if (severity == nri::Message::ERROR)
        {
            ++state.validation_errors;
        }
        if (severity == nri::Message::WARNING)
        {
            ++state.validation_warnings;
        }
        const auto& sink = state.config.diagnostics;
        if (sink.report)
        {
            const auto level = severity == nri::Message::ERROR     ? ERenderDiagnosticSeverity::ERROR_MESSAGE
                               : severity == nri::Message::WARNING ? ERenderDiagnosticSeverity::WARNING
                                                                   : ERenderDiagnosticSeverity::INFORMATION;
            sink.report(
                sink.context,
                {level,
                 RenderError{kDeviceDiagnostic, {static_cast<std::uint64_t>(level), line}},
                 message ? std::string_view(message) : std::string_view{},
                 file ? std::string_view(file) : std::string_view{},
                 line}
            );
        }
    }

    RenderResult<void> RenderServer::Impl::initialize(DeviceOptions device_options) noexcept
    {
        auto& s = *this;
        const nri::CallbackInterface diagnostic{Impl::diagnostic, [](void*) noexcept {}, &s};
        auto device = detail::DeviceSession::create(device_options, diagnostic, s.config.validation, 3);
        if (!device)
        {
            return cxx::unexpected(device.error());
        }
        s.device = std::move(*device);
        if (s.config.uploads)
        {
            auto uploads = detail::ResourceUploader::create(*s.device, *s.config.uploads, s.cpu_scheduler);
            if (!uploads)
            {
                return cxx::unexpected(uploads.error());
            }
            s.uploads = std::move(*uploads);
        }
        auto frames = detail::FrameExecution::create(*s.device, s.uploads.get(), s.config.frames_in_flight);
        if (!frames)
        {
            return cxx::unexpected(frames.error());
        }
        s.frames = std::move(*frames);
        const auto& enabled = s.device->enabledFeatures();
        s.resources.emplace(
            (s.device->descriptors().capacity(nri::DescriptorType::TEXTURE) ? 1u : 0u) |
            (enabled.texture_compression_bc ? 2u : 0u) |
            (s.device->descriptors().capacity(nri::DescriptorType::STORAGE_STRUCTURED_BUFFER) ? 4u : 0u)
        );
        s.global_storages.emplace(*s.resources, true);
        s.paged_instances = device_options.buffer_device_address;
        return {};
    }

    auto RenderServer::Impl::compileGraphEffect(
        GraphEffectRequest&& input,
        RenderError initial_error,
        std::vector<RenderEffect::Implementation> implementations,
        std::vector<RenderEffect::StorageDependency> storage_dependencies
    ) noexcept
    {
        std::vector<const FeatureEntry*> registrations;
        if (initial_error.type == 0 && input.definition.features.empty())
        {
            initial_error = {kFeatureContract};
        }
        registrations.reserve(input.definition.features.size());
        for (const auto& selection : input.definition.features)
        {
            if (initial_error.type != 0)
            {
                break;
            }
            const auto* found = features.find(selection.feature);
            if (!found)
            {
                initial_error = {kRuntimeIdentity};
                break;
            }
            registrations.push_back(found);
        }

        std::vector<StorageNameView> storage_types;
        resources->forEachType(
            [&](StorageTypeId id, const StorageDescriptor& type, std::uint32_t) noexcept
            {
                const auto* factory = resources->factory(id);
                if (factory && factory->binding_ops &&
                    static_cast<const detail::StorageBindingOps*>(factory->binding_ops)->resolve)
                {
                    storage_types.push_back(type.type);
                }
            }
        );
        std::ranges::sort(storage_types, {}, [](StorageNameView type) { return type.name(); });
        // Workers receive only immutable feature entries, the request and actual device facts.
        // The final continuation is the sole place that adopts a candidate into the Effect table.
        auto compiled =
            stdexec::schedule(cpu_scheduler) |
            stdexec::then(
                [native = &device->device(),
                 nri_core = &device->nriCore(),
                 enabled = device->enabledFeatures(),
                 queues =
                     std::array{
                         &device->queue(detail::EQueueRole::GRAPHICS).properties(),
                         &device->queue(detail::EQueueRole::COMPUTE).properties(),
                         &device->queue(detail::EQueueRole::TRANSFER).properties()
                     },
                 storage_types = std::move(storage_types),
                 request = std::move(input),
                 entries = std::move(registrations),
                 limit = config.max_config_bytes,
                 storage_dependencies = std::move(storage_dependencies)]() mutable noexcept
                {
                    return compileEffect(
                        *native,
                        *nri_core,
                        enabled,
                        queues,
                        storage_types,
                        request,
                        entries,
                        limit,
                        std::move(storage_dependencies)
                    );
                }
            ) |
            stdexec::continues_on(render_loop_.scheduler()) |
            stdexec::let_value(
                [server = this, implementations = std::move(implementations)](
                    RenderResult<std::pair<RenderEffect, RenderGraph>>& candidate
                ) mutable noexcept
                {
                    return [](Impl* server, RenderResult<std::pair<RenderEffect, RenderGraph>> candidate,
                              std::vector<RenderEffect::Implementation> implementations
                    ) noexcept -> exec::task<EffectCreated>
                    {
                        if (!candidate) co_return EffectCreated{candidate.error()};
                        auto [compiled_effect, graph] = std::move(*candidate);
                        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
                        if (stop.stop_requested()) co_return EffectCreated{RenderError{kCommandCancelled}};
                        auto programs = co_await compiled_effect.plan->preparePrograms(
                            *server->device, graph, server->cpu_scheduler
                        );
                        if (!programs) co_return EffectCreated{programs.error()};
                        if (stop.stop_requested()) co_return EffectCreated{RenderError{kCommandCancelled}};
                        compiled_effect.implementations = std::move(implementations);
                        auto bindings = server->resolveFrameBindings(compiled_effect, graph);
                        if (!bindings) co_return EffectCreated{bindings.error()};
                        const auto effect = server->effects.emplace(std::move(compiled_effect));
                        ++server->compiled_graphs;
                        co_return EffectCreated{{}, effect};
                    }(server, std::move(candidate), std::move(implementations));
                }
            );
        auto invalid = stdexec::just(EffectCreated{initial_error});
        using VSource = exec::variant_sender<decltype(invalid), decltype(compiled)>;
        return initial_error.type != error::ErrorId{} ? VSource{std::move(invalid)} : VSource{std::move(compiled)};
    }

    auto RenderServer::Impl::createCapabilityEffect(CreateEffect::Request&& input) noexcept
    {
        GraphEffectRequest graph{input.output};
        std::vector<RenderEffect::StorageDependency> storage_dependencies;
        auto built = buildCapabilityGraph(input, graph, storage_dependencies);
        std::vector<RenderEffect::Implementation> implementations;
        for (const auto& contribution : input.composition.contributions)
        {
            implementations.push_back({contribution.implementation_id, contribution.key, {}});
        }
        return compileGraphEffect(
            std::move(graph),
            built ? RenderError{} : built.error(),
            std::move(implementations),
            std::move(storage_dependencies)
        );
    }

    void RenderServer::Impl::bindCreateEffectCommand() noexcept
    {
        bindFrameworkCommand<CreateEffect, &Impl::createCapabilityEffect>();
    }

    void RenderServer::Impl::removeEffect(
        const RemoveEffect::Request& _request,
        RemoveEffect::Response& output_response
    ) noexcept
    {
        const auto* input = &_request;
        auto* effect = effects.find(input->effect);
        if (!effect)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        // Effect deletion must not silently destroy other Views' associations. The creator
        // removes it only after every borrowing Attachment has explicitly left.
        if (!effect->attachments.empty())
        {
            output_response.error = RenderError{kRuntimeState};
            return;
        }
        effects.erase(input->effect);
        output_response = {};
    }

    RenderResult<EffectAttachment> RenderServer::Impl::prepareAttachment(
        RenderEffect& effect,
        detail::TargetOutput output,
        StorageSet& scene,
        StorageSet& view,
        StorageSet& attachment,
        const AttachmentRequest& input
    ) noexcept
    {
        const auto target_images = std::span(input.target_images);
        const auto& plan = *effect.plan;
        auto dependencies = storageContext();
        std::vector<std::pair<StorageSet*, StorageId>> created_storages;
        cxx::scope_exit rollback(
            [&]() noexcept
            {
                for (const auto& [set, id] : created_storages | std::views::reverse)
                {
                    (void)set->remove(id);
                }
            }
        );
        const std::array scope_sets{&*global_storages, &scene, &view, &attachment};
        std::vector<StorageId> dependency_storages;
        dependency_storages.reserve(effect.storage_dependencies.size());
        for (const auto& dependency : effect.storage_dependencies)
        {
            const auto& requirement = dependency.requirement;
            auto instance = ensureStorage(
                *scope_sets[static_cast<std::size_t>(dependency.scope)],
                resources->typeId(requirement.type.view()),
                requirement.key,
                requirement.config,
                created_storages
            );
            if (!instance)
            {
                return cxx::unexpected(instance.error());
            }
            dependency_storages.push_back(*instance);
        }
        EffectAttachment candidate;
        candidate.instance =
            std::make_unique<detail::RenderGraphInstance>(*device, frames->frameCount(), plan, output);
        candidate.target_images.reserve(target_images.size());
        std::vector<GraphResourceId> image_resources;
        for (const auto& input : target_images)
        {
            const auto* source = targets.find(input.target);
            if (!source || !source->description.sampled || source->surface())
            {
                return cxx::unexpected(RenderError{kRuntimeIdentity});
            }
            const auto resource =
                std::ranges::find(plan.resources(), input.resource_key, &detail::CompiledResource::key);
            if (resource == plan.resources().end())
            {
                return cxx::unexpected(RenderError{kRuntimeIdentity});
            }
            const auto index = static_cast<std::uint32_t>(resource - plan.resources().begin());
            const GraphResourceId id{index};
            const bool duplicate = std::ranges::find(image_resources, id) != image_resources.end();
            const auto* image = std::get_if<GraphImageDesc>(&resource->description);
            const bool invalid_resource = duplicate || !image || !resource->memory_used ||
                                          resource->origin != EGraphOrigin::IMPORTED ||
                                          !resource->sampled_target_input;
            if (invalid_resource)
            {
                return cxx::unexpected(RenderError{kCommandContract});
            }
            if (source->resizing)
            {
                return cxx::unexpected(RenderError{kDeviceBusy});
            }
            candidate.target_images.push_back({input.target, id});
            const auto images = source->output();
            const bool incompatible = !images.description.sampled || images.description.samples != image->samples ||
                                      images.description.format != image->format || image->layers != 1 ||
                                      image->mips != 1;
            if (incompatible)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            image_resources.push_back(id);
            for (std::uint32_t slot = 0; slot < frames->frameCount(); ++slot)
            {
                candidate.instance->bindImage(id, slot, *images.frames[slot].image);
            }
        }
        candidate.storage_inputs.reserve(plan.resources().size());
        std::vector<StorageId> resolved(plan.resources().size());
        for (std::size_t dependency_index = 0; dependency_index < effect.storage_dependencies.size();
             ++dependency_index)
        {
            const auto& dependency = effect.storage_dependencies[dependency_index];
            const auto* factory = resources->factory(dependency.requirement.type.view());
            const auto id = dependency_storages[dependency_index];
            for (const auto& argument : dependency.bindings)
            {
                const auto index = argument.resource.index;
                const auto type = argument.type();
                if (type != factory->descriptor->binding_type)
                {
                    return cxx::unexpected(RenderError{kStorageContract});
                }
                const auto* value = argument.data();
                const auto* ops = static_cast<const detail::StorageBindingOps*>(factory->binding_ops);
                resolved[index] = id;
                if (auto bound =
                        candidate
                            .bindStorage(index, *ops, resources->storageInstance(id), value, frames->frameCount());
                    !bound)
                {
                    return cxx::unexpected(bound.error());
                }
            }
        }
        for (const auto& binding : effect.bindings)
        {
            auto created = binding.create(
                *device,
                *frames,
                *resources,
                *candidate.instance,
                device->programs(),
                resolved,
                binding.configuration.data()
            );
            if (!created)
            {
                return cxx::unexpected(created.error());
            }
            created->definition = &binding;
            created->resolved_inputs.resize(binding.inputs.size());
            for (std::uint32_t slot = 0; slot < frames->frameCount(); ++slot)
            {
                for (const auto& update : created->initial_resources)
                {
                    candidate.instance->setResource(update.resource.index, slot, update.input);
                }
            }
            created->initial_resources = {};
            candidate.feature_bindings.push_back(std::move(*created));
        }
        for (std::uint32_t input = 0; input < candidate.instance->samplerInputCount(); ++input)
        {
            if (!candidate.instance->usesSampler(input))
            {
                continue;
            }
            auto sampler = device->descriptors().sampler(effect.samplers[input]);
            if (!sampler)
            {
                return cxx::unexpected(sampler.error());
            }
            if (auto bound =
                    candidate.instance->bindSampler(input, **sampler, effect.samplers[input].comparison.has_value());
                !bound)
            {
                return cxx::unexpected(bound.error());
            }
        }
        if (auto ready = candidate.instance->initialize(); !ready)
        {
            return cxx::unexpected(ready.error());
        }
        if (auto values =
                candidate.instance
                    ->setInputs(input.parameters, input.conditions, input.parameter_type, effect.parameter_projections);
            !values)
        {
            return cxx::unexpected(values.error());
        }
        candidate.instance->setViewport(input.viewport);
        rollback.release();
        return candidate;
    }

    bool RenderServer::Impl::canChangeAttachments(std::span<const AttachmentId> ids) const noexcept
    {
        return std::ranges::all_of(
            ids,
            [&](auto id)
            {
                const auto& attachment = *attachments.find(id);
                auto* instance = attachment.instance.get();
                const bool target_resizing = targets.find(attachment.target)->resizing ||
                                             std::ranges::any_of(
                                                 attachment.target_images,
                                                 [&](const auto& input) { return targets.find(input.target)->resizing; }
                                             );
                return !target_resizing && frames->canChangeResources({&instance, 1});
            }
        );
    }

    void RenderServer::Impl::detachAttachment(AttachmentId id) noexcept
    {
        auto& attachment = *attachments.find(id);

        auto& view = *scenes.find(attachment.scene)->views.find(attachment.view);
        auto& effect = *effects.find(attachment.effect);
        auto& target = *targets.find(attachment.target);
        const auto last_view = view.attachments.back();
        view.attachments[attachment.view_position] = last_view;
        attachments.find(last_view)->view_position = attachment.view_position;
        view.attachments.pop_back();

        const auto last_effect = effect.attachments.back();
        effect.attachments[attachment.effect_position] = last_effect;
        attachments.find(last_effect)->effect_position = attachment.effect_position;
        effect.attachments.pop_back();
        for (const auto& input : attachment.target_images)
        {
            std::erase(targets.find(input.target)->consumers, id);
        }
        target.attachment = {};
        active_targets.erase(attachment.target);
        active_frame_inputs_dirty = true;
        attachments.erase(id);
    }

    void RenderServer::Impl::attachEffect(const Attach::Request& _request, Attach::Response& output_response) noexcept
    {

        const auto& scene_id = _request.scene;
        const auto& view_id = _request.view;
        const auto& target_id = _request.target;
        const auto& effect_id = _request.effect;
        auto* scene = scenes.find(scene_id);
        auto* view = scene ? scene->views.find(view_id) : nullptr;
        auto* target = targets.find(target_id);
        auto* effect = effects.find(effect_id);
        const bool invalid = !view || !target || !effect;
        if (invalid)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        if (target->resizing)
        {
            output_response.error = RenderError{kDeviceBusy};
            return;
        }
        if (target->surface() && !target->surface()->available())
        {
            output_response.error = RenderError{kRuntimeState};
            return;
        }
        if (target->surface() && effect->plan->output() == kGraphInvalidIndex)
        {
            output_response.error = RenderError{kRuntimeUnsupported};
            return;
        }
        const bool target_mismatch = !target->attachment.isNull() && target->attachment != _request.replaces;
        if (target_mismatch)
        {
            output_response.error = RenderError{kRuntimeState};
            return;
        }
        if (!_request.replaces.isNull())
        {
            const auto* previous = attachments.find(_request.replaces);
            if (!previous)
            {
                output_response.error = RenderError{kRuntimeIdentity};
                return;
            }
            const bool different_view = previous->scene != scene_id || previous->view != view_id;
            if (different_view || !canChangeAttachments({&_request.replaces, 1}))
            {
                output_response.error = RenderError{different_view ? kRuntimeIdentity : kDeviceBusy};
                return;
            }
        }
        if (auto valid = _request.viewport.validate(); !valid)
        {
            output_response.error = valid.error();
            return;
        }
        if (targetDependencyCycle(target_id, _request.target_images, _request.replaces))
        {
            output_response.error = RenderError{kCommandContract};
            return;
        }
        auto local_storages = std::make_unique<StorageSet>(*resources);
        auto prepared =
            prepareAttachment(*effect, target->output(), scene->storages, view->storages, *local_storages, _request);
        if (!prepared)
        {
            output_response.error = prepared.error();
            return;
        }
        output_response = {};
        prepared->effect = effect_id;
        prepared->scene = scene_id;
        prepared->view = view_id;
        prepared->target = target_id;
        prepared->storages = std::move(local_storages);
        if (!_request.replaces.isNull())
        {
            detachAttachment(_request.replaces);
        }
        const auto id = attachments.emplace(std::move(*prepared));
        auto& attachment = *attachments.find(id);
        attachment.view_position = view->attachments.size();
        attachment.effect_position = effect->attachments.size();
        view->attachments.push_back(id);
        effect->attachments.push_back(id);
        target->attachment = id;
        for (const auto& input : attachment.target_images)
        {
            auto& consumers = targets.find(input.target)->consumers;
            if (std::ranges::find(consumers, id) == consumers.end())
            {
                consumers.push_back(id);
            }
        }
        output_response.attachment = id;
        if (target->enabled)
        {
            active_targets.emplace(target_id);
        }
        active_frame_inputs_dirty = true;
        storages_created.store(resources->creations());
        return;
    }

    bool RenderServer::Impl::targetDependencyCycle(
        TargetId destination,
        std::span<const TargetImageInput> inputs,
        AttachmentId replaced
    ) const noexcept
    {
        std::vector<TargetId> pending, visited;
        for (const auto& input : inputs)
        {
            pending.push_back(input.target);
        }
        while (!pending.empty())
        {
            const auto id = pending.back();
            pending.pop_back();
            if (id == destination)
            {
                return true;
            }
            if (std::ranges::find(visited, id) != visited.end())
            {
                continue;
            }
            visited.push_back(id);
            const auto* target = targets.find(id);
            const auto* attachment =
                target && target->attachment != replaced ? attachments.find(target->attachment) : nullptr;
            if (attachment)
            {
                for (const auto& input : attachment->target_images)
                {
                    pending.push_back(input.target);
                }
            }
        }
        return false;
    }

    void RenderServer::Impl::createScene(
        const CreateScene::Request& _request,
        CreateScene::Response& output_response
    ) noexcept
    {
        output_response = SceneCreated{{}, scenes.emplace(*resources)};
    }

    void RenderServer::Impl::removeScene(
        const RemoveScene::Request& _request,
        RemoveScene::Response& output_response
    ) noexcept
    {
        const auto* input = &_request;
        const auto scene_id = input->scene;
        auto* scene = scenes.find(scene_id);
        if (!scene)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        for (auto& view : scene->views)
        {
            if (!canChangeAttachments(view.attachments))
            {
                output_response.error = RenderError{kDeviceBusy};
                return;
            }
        }
        for (auto& view : scene->views)
        {
            while (!view.attachments.empty())
            {
                detachAttachment(view.attachments.back());
            }
        }
        scenes.erase(scene_id);
        output_response = {};
    }

    void
    RenderServer::Impl::createView(const CreateView::Request& _request, CreateView::Response& output_response) noexcept
    {
        const auto* input = &_request;
        auto* scene = scenes.find(input->scene);
        if (!scene)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        output_response = ViewCreated{{}, scene->views.emplace(*resources)};
    }

    void RenderServer::Impl::updateAttachmentInputs(
        const UpdateAttachmentInputs::Request& input,
        UpdateAttachmentInputs::Response& output_response
    ) noexcept
    {
        auto* attachment = attachments.find(input.attachment);
        if (!attachment)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        if (auto valid = input.viewport.validate(); !valid)
        {
            output_response.error = valid.error();
            return;
        }
        // Native validates the complete input size before changing any parameter bytes.
        const auto& effect = *effects.find(attachment->effect);
        if (auto applied =
                attachment->instance
                    ->setInputs(input.parameters, input.conditions, input.parameter_type, effect.parameter_projections);
            !applied)
        {
            output_response.error = applied.error();
            return;
        }
        attachment->instance->setViewport(input.viewport);
        output_response = {};
    }

    void RenderServer::Impl::invalidateHistory(
        const InvalidateHistory::Request& input,
        InvalidateHistory::Response& output
    ) noexcept
    {
        auto* attachment = attachments.find(input.attachment);
        if (!attachment)
        {
            output.error = RenderError{kRuntimeIdentity};
            return;
        }
        if (auto invalidated = attachment->instance->invalidateHistory(input.previous); !invalidated)
        {
            output.error = invalidated.error();
            return;
        }
        output = {};
    }

    void
    RenderServer::Impl::removeView(const RemoveView::Request& _request, RemoveView::Response& output_response) noexcept
    {
        const auto* input = &_request;
        auto* scene = scenes.find(input->scene);
        auto* view = scene ? scene->views.find(input->view) : nullptr;
        if (!view)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        if (!canChangeAttachments(view->attachments))
        {
            output_response.error = RenderError{kDeviceBusy};
            return;
        }
        while (!view->attachments.empty())
        {
            detachAttachment(view->attachments.back());
        }
        scene->views.erase(input->view);
        output_response = {};
    }

    void RenderServer::Impl::createTarget(const CreateTarget::Request& input, CreateTarget::Response& response) noexcept
    {
        if (auto valid = input.description.validate(); !valid)
        {
            response.error = valid.error();
            return;
        }
        const bool is_surface = input.description.kind == ETargetKind::SURFACE;
        if (is_surface != input.surface.has_value())
        {
            response.error = RenderError{kCommandContract};
            return;
        }
        TargetId id;
        if (is_surface)
        {
            auto physical =
                detail::SurfaceTarget::create(*device, input.description, *input.surface, frames->frameCount());
            if (!physical)
            {
                response.error = physical.error();
                return;
            }
            id = targets.emplace(RenderTarget::VPhysicalTarget{std::move(*physical)});
        }
        else
        {
            auto physical = detail::OffscreenTarget::create(*device, input.description, frames->frameCount());
            if (!physical)
            {
                response.error = physical.error();
                return;
            }
            id = targets.emplace(RenderTarget::VPhysicalTarget{std::move(*physical)});
        }
        response = TargetCreated{{}, id, 1, targets.find(id)->description, true};
    }

    RenderResult<RenderServer::Impl::ResizeCandidates> RenderServer::Impl::prepareTargetResize(
        TargetId target,
        detail::TargetOutput output,
        std::span<const AttachmentId> affected
    ) noexcept
    {
        ResizeCandidates candidates;
        candidates.reserve(affected.size());
        for (auto id : affected)
        {
            auto& attachment = *attachments.find(id);
            std::vector<detail::RenderGraphInstance::ImageReplacement> inputs;
            for (const auto& image : attachment.target_images)
            {
                if (image.target == target)
                {
                    inputs.push_back({image.resource, output.frames});
                }
            }
            auto changed = attachment.instance->prepareResize(
                attachment.target == target ? output : targets.find(attachment.target)->output(),
                inputs
            );
            if (!changed)
            {
                return cxx::unexpected(changed.error());
            }
            candidates.emplace_back(id, std::move(*changed));
        }
        return candidates;
    }

    exec::task<TargetCreated> RenderServer::Impl::resizeTarget(ResizeTarget::Request&& request) noexcept
    {
        const auto input = std::move(request);
        const auto response = [this, id = input.target](RenderError error = {}) noexcept
        {
            const auto* current = targets.find(id);
            if (!current)
            {
                return TargetCreated{error};
            }
            const bool available = !current->surface() || current->surface()->available();
            return TargetCreated{error, id, current->epoch, current->description, available};
        };
        if (auto valid = input.description.validate(); !valid)
        {
            co_return response(valid.error());
        }
        auto* target = targets.find(input.target);
        if (!target)
        {
            co_return response(RenderError{kRuntimeIdentity});
        }
        const bool different_kind = input.description.kind != target->description.kind;
        const auto maximum = device->description().dimensions.attachmentMaxDim;
        const bool oversized = input.description.width > maximum || input.description.height > maximum;
        const bool removes_sampling = !input.description.sampled && !target->consumers.empty();
        if (different_kind || oversized || removes_sampling)
        {
            co_return response(RenderError{kCommandContract});
        }
        if (target->resizing) co_return response(RenderError{kDeviceBusy});
        const auto* surface_output = target->surface();
        const bool unchanged = input.description == target->description;
        const bool healthy = !surface_output || !surface_output->needsRecreation();
        if (unchanged && healthy) co_return response();
        auto affected = target->consumers;
        if (!target->attachment.isNull())
        {
            affected.push_back(target->attachment);
            const auto& attachment = *attachments.find(target->attachment);
            if (!effects.find(attachment.effect)->plan->compatible(input.description))
            {
                co_return response(RenderError{kDeviceArgument});
            }
        }
        if (!canChangeAttachments(affected))
        {
            co_return response(RenderError{kDeviceBusy});
        }
        if (target->epoch == UINT64_MAX)
        {
            co_return response(RenderError{kRuntimeCapacity});
        }
        std::vector<detail::RenderGraphInstance*> instances;
        std::vector<TargetId> paused;
        for (auto id : affected)
        {
            const auto& attachment = *attachments.find(id);
            instances.push_back(attachment.instance.get());
            paused.push_back(attachment.target);
            active_targets.erase(attachment.target);
        }
        target->resizing = true;
        active_frame_inputs_dirty = true;
        cxx::scope_exit finish(
            [&]() noexcept
            {
                targets.find(input.target)->resizing = false;
                for (auto id : paused)
                {
                    const auto& output = *targets.find(id);
                    const bool available = !output.surface() || output.surface()->available();
                    if (output.enabled && !output.attachment.isNull() && available)
                    {
                        active_targets.emplace(id);
                    }
                }
                active_frame_inputs_dirty = true;
            }
        );
        const auto accepted = frames->acceptedUses(instances);
        RenderTarget::VPhysicalTarget physical;
        ResizeCandidates candidates;
        const bool surface = target->surface() != nullptr;
        if (!surface)
        {
            auto created = detail::OffscreenTarget::create(*device, input.description, frames->frameCount());
            if (!created)
            {
                co_return response(created.error());
            }
            physical = std::move(*created);
            auto prepared = prepareTargetResize(
                input.target,
                std::visit([](const auto& value) { return value->output(); }, physical),
                affected
            );
            if (!prepared)
            {
                co_return response(prepared.error());
            }
            candidates = std::move(*prepared);
        }
        // Register all physical-queue frontiers before suspension; no device idle or per-pass wait.
        RenderError wait_error;
        const auto wait = [&](std::size_t q)
        { return device->physicalQueue(q).whenSubmissionCompletes(accepted.serials[q]); };
        const auto recordWaitFailure = [&](const RenderResult<void>& completed)
        {
            if (!completed && wait_error.type == 0)
            {
                wait_error = completed.error();
            }
        };
        switch (device->queueCount())
        {
        case 1:
            recordWaitFailure(co_await wait(0));
            break;
        case 2:
        {
            auto [first, second] = co_await stdexec::when_all(wait(0), wait(1));
            recordWaitFailure(first);
            recordWaitFailure(second);
            break;
        }
        case 3:
        {
            auto [first, second, third] = co_await stdexec::when_all(wait(0), wait(1), wait(2));
            recordWaitFailure(first);
            recordWaitFailure(second);
            recordWaitFailure(third);
            break;
        }
        }
        if (wait_error.type != 0)
        {
            co_return response(wait_error);
        }
        target = targets.find(input.target);
        if (surface)
        {
            const auto window = target->surface()->surface();
            if (!target->attachment.isNull())
            {
                attachments.find(target->attachment)->instance->releaseOutputReferences();
            }
            auto closed = co_await target->surface()->close(accepted);
            if (!closed)
            {
                co_return response(closed.error());
            }
            target = targets.find(input.target);
            auto created = detail::SurfaceTarget::create(*device, input.description, window, frames->frameCount());
            if (!created)
            {
                co_return response(created.error());
            }
            physical = std::move(*created);
            auto prepared = prepareTargetResize(
                input.target,
                std::visit([](const auto& value) { return value->output(); }, physical),
                affected
            );
            if (!prepared)
            {
                co_return response(prepared.error());
            }
            candidates = std::move(*prepared);
        }
        // Every fallible GPU creation is finished. Publish all producer/consumer references without suspension.
        for (auto& [id, candidate] : candidates)
        {
            if (auto ready = attachments.find(id)->instance->prepareResizeAdoption(candidate); !ready)
            {
                co_return response(ready.error());
            }
        }
        for (auto& [id, candidate] : candidates)
        {
            attachments.find(id)->instance->commitResize(std::move(candidate));
        }
        target = targets.find(input.target);
        target->physical = std::move(physical);
        target->description = target->output().description;
        ++target->epoch;
        // The reply carries the adopted extent. Resume only with matching inputs in a later complete frame.
        target->enabled = false;
        co_return response();
    }

    exec::task<AckResponse> RenderServer::Impl::removeTarget(RemoveTarget::Request&& input) noexcept
    {
        auto* target = targets.find(input);
        if (!target)
        {
            co_return AckResponse{RenderError{kRuntimeIdentity}};
        }
        if (target->resizing)
        {
            co_return AckResponse{RenderError{kDeviceBusy}};
        }
        if (!target->consumers.empty())
        {
            co_return AckResponse{RenderError{kRuntimeState}};
        }
        detail::SubmissionFrontiers accepted;
        // A detached target no longer has its instance in the active binding table. Keep the
        // conservative snapshot for that path; attached owners can identify their exact uses.
        for (std::size_t q = 0; q < device->queueCount(); ++q)
        {
            accepted.serials[q] = device->physicalQueue(q).submitted();
        }
        if (!target->attachment.isNull())
        {
            if (!canChangeAttachments({&target->attachment, 1}))
            {
                co_return AckResponse{RenderError{kDeviceBusy}};
            }
            auto* instance = attachments.find(target->attachment)->instance.get();
            accepted = frames->acceptedUses({&instance, 1});
            detachAttachment(target->attachment);
        }
        auto physical = std::move(target->physical);
        targets.erase(input);
        if (auto* surface = std::get_if<std::unique_ptr<detail::SurfaceTarget>>(&physical))
        {
            auto closed = co_await (*surface)->close(accepted);
            if (!closed)
            {
                // The close completion follows actual SwapChain destruction, including DeviceLost.
                // Removal succeeded; report device failure separately instead of retaining a stale TargetId.
                failExecution(closed.error());
            }
        }
        co_return AckResponse{};
    }

    void RenderServer::Impl::setTargetActive(
        const SetTargetActive::Request& input,
        SetTargetActive::Response& response
    ) noexcept
    {
        auto* target = targets.find(input.target);
        if (!target)
        {
            response.error = RenderError{kRuntimeIdentity};
            return;
        }
        if (target->resizing || (!target->attachment.isNull() && !canChangeAttachments({&target->attachment, 1})))
        {
            response.error = RenderError{kDeviceBusy};
            return;
        }
        if (input.active && input.epoch != target->epoch)
        {
            response.error = RenderError{kRuntimeState};
            return;
        }
        if (input.active && target->surface() && !target->surface()->available())
        {
            response.error = RenderError{kRuntimeState};
            return;
        }
        target->enabled = input.active;
        if (input.active && !target->attachment.isNull())
        {
            active_targets.emplace(input.target);
        }
        else
        {
            active_targets.erase(input.target);
        }
        active_frame_inputs_dirty = true;
        response = {};
    }

    void RenderServer::Impl::detachEffect(const Detach::Request& _request, Detach::Response& output_response) noexcept
    {
        const auto* attachment = attachments.find(_request);
        if (!attachment)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        if (!canChangeAttachments({&_request, 1}))
        {
            output_response.error = RenderError{kDeviceBusy};
            return;
        }
        detachAttachment(_request);
        output_response = {};
    }

    void RenderServer::Impl::queryCommandIds(
        const QueryCommandIds::Request& names, QueryCommandIds::Response& response
    ) noexcept
    {
        command_registry_.queryCommandIds(names, response);
    }

    void RenderServer::Impl::listCommands(
        const ListCommands::Request& _request,
        ListCommands::Response& output_response
    ) noexcept
    {
        output_response = command_registry_.listCommands();
    }

    void RenderServer::Impl::listCommandBindings(
        const ListCommandBindings::Request& input,
        ListCommandBindings::Response& response
    ) noexcept
    {
        command_registry_.listCommandBindings(input, response);
    }

    void RenderServer::Impl::selectCommandBinding(
        const SelectCommandBinding::Request& input,
        SelectCommandBinding::Response& response
    ) noexcept
    {
        command_registry_.selectHandler(input, response, *resources);
    }

    RenderResult<void> RenderServer::Impl::progressGpuCompletions() noexcept
    {
        if (auto observed = device->pollQueues(); !observed)
        {
            return observed;
        }
        device->dispatchQueueCompletions();
        return {};
    }

    void RenderServer::Impl::progressUploads() noexcept
    {
        if (uploads)
        {
            uploads->submitReadyTransfers();
        }
    }

    void RenderServer::Impl::rebuildActiveFrameInputs() noexcept
    {
        // Semantic conflict order is stable. Fairness rotates ready physical queues, not
        // the order of writers of one imported resource.
        const auto keys = active_targets.keys();
        std::vector<TargetId> active(keys.begin(), keys.end());
        std::ranges::sort(active, {}, &TargetId::index);
        std::map<std::uint32_t, std::size_t> positions;
        std::vector<std::vector<std::size_t>> successors(active.size());
        std::vector<std::size_t> predecessors(active.size());
        std::vector<bool> unavailable(active.size());
        for (std::size_t i = 0; i < active.size(); ++i)
        {
            positions.emplace(active[i].index, i);
        }
        for (std::size_t i = 0; i < active.size(); ++i)
        {
            const auto& attachment = *attachments.find(targets.find(active[i])->attachment);
            for (const auto& input : attachment.target_images)
            {
                const auto producer = positions.find(input.target.index);
                if (producer == positions.end())
                {
                    unavailable[i] = true; // No implicit previous-frame/history sampling.
                    continue;
                }
                successors[producer->second].push_back(i);
                ++predecessors[i];
            }
        }
        std::vector<std::size_t> ready;
        std::vector<TargetId> ordered;
        ready.reserve(active.size());
        ordered.reserve(active.size());
        for (std::size_t i = 0; i < active.size(); ++i)
        {
            if (predecessors[i] == 0)
            {
                ready.push_back(i);
            }
        }
        for (std::size_t cursor = 0; cursor < ready.size(); ++cursor)
        {
            const auto i = ready[cursor];
            if (!unavailable[i])
            {
                ordered.push_back(active[i]);
            }
            for (auto consumer : successors[i])
            {
                unavailable[consumer] = unavailable[consumer] || unavailable[i];
                if (--predecessors[consumer] == 0)
                {
                    ready.push_back(consumer);
                }
            }
        }
        active = std::move(ordered);
        frame_instances.clear();
        frame_surfaces.clear();
        frame_storages.clear();
        frame_features.clear();
        for (auto id : active)
        {
            auto& target = *targets.find(id);
            if (auto* surface = target.surface())
            {
                frame_surfaces.push_back(surface);
            }
            auto& attachment = *attachments.find(target.attachment);
            frame_instances.push_back(attachment.instance.get());
            const auto& effect = *effects.find(attachment.effect);
            auto& scene = *scenes.find(attachment.scene);
            auto& view = *scene.views.find(attachment.view);
            const std::array scopes{&*global_storages, &scene.storages, &view.storages, attachment.storages.get()};
            for (const auto& dependency : effect.storage_dependencies)
            {
                const auto* factory = resources->factory(dependency.requirement.type.view());
                const auto* ops = static_cast<const detail::StorageBindingOps*>(factory->binding_ops);
                if (!ops || !ops->prepareFrame)
                {
                    continue;
                }
                const auto storage = scopes[static_cast<std::size_t>(dependency.scope)]->find(
                    resources->typeId(dependency.requirement.type.view()),
                    dependency.requirement.key
                );
                if (!storage.isNull())
                {
                    auto* native = resources->storageInstance(storage);
                    frame_storages.push_back(
                        {native, ops, storage, ops->preparationRevision ? ops->preparationRevision(native) : nullptr}
                    );
                }
            }
            for (auto& feature : attachment.feature_bindings)
            {
                frame_features.emplace_back(attachment.instance.get(), &feature);
            }
        }
        std::ranges::sort(frame_storages, {}, &detail::StorageFrameUse::storage);
        const auto duplicate = std::ranges::unique(frame_storages, {}, &detail::StorageFrameUse::storage);
        frame_storages.erase(duplicate.begin(), duplicate.end());
        active_frame_inputs_dirty = false;
    }

    RenderResult<bool> RenderServer::Impl::acquireFrameOutputs(std::uint32_t slot) noexcept
    {
        if (auto presented = presentFrameOutputs(); !presented)
        {
            return cxx::unexpected(presented.error());
        }
        for (auto* surface : frame_surfaces)
        {
            const auto acquired = surface->acquire(slot);
            if (!acquired || !*acquired)
            {
                return acquired;
            }
        }
        return true;
    }

    RenderResult<std::uint32_t> RenderServer::Impl::presentFrameOutputs() noexcept
    {
        if (active_frame_inputs_dirty)
        {
            rebuildActiveFrameInputs();
        }
        std::uint32_t count = 0;
        RenderError failure;
        for (auto* surface : frame_surfaces)
        {
            const auto presented = surface->present();
            if (presented)
            {
                count += *presented;
            }
            else if (failure.type == 0)
            {
                failure = presented.error();
            }
        }
        presentations += count;
        if (failure.type != 0)
        {
            return cxx::unexpected(failure);
        }
        return count;
    }

    RenderResult<void> RenderServer::Impl::submitCurrentFrame(std::uint32_t slot) noexcept
    {
        const auto accepted = frames->render(frame_instances, frame_storages, frame_features);
        const auto presented = presentFrameOutputs();
        if (!accepted)
        {
            return cxx::unexpected(accepted.error());
        }
        submissions += *accepted;
        if (!presented)
        {
            if (!frames->inputConsumed(slot))
            {
                frames->cancelSubmissions(presented.error());
            }
            return cxx::unexpected(presented.error());
        }
        return {};
    }

    void RenderServer::Impl::queryExecutionProgress(
        const QueryExecutionProgress::Request& _request,
        QueryExecutionProgress::Response& output_response
    ) noexcept
    {
        const auto* attachment = attachments.find(_request);
        if (!attachment)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        output_response = {{}, attachment->instance->progress()};
    }

    void RenderServer::Impl::queryRenderStats(
        const QueryRenderStats::Request& _request,
        QueryRenderStats::Response& output_response
    ) noexcept
    {
        output_response =
            RenderStatsSnapshot{{}, ticks.load(), commands.load(), compiled_graphs.load(), submissions.load()};
    }

    void RenderServer::Impl::stopCommand(const Stop::Request& _request, Stop::Response& output_response) noexcept
    {
        failExecution(RenderError{kCommandCancelled});
        output_response = {};
    }

    void RenderServer::Impl::failExecution(RenderError error) noexcept
    {
        if (execution_error.type != 0)
        {
            return;
        }
        execution_error = error;
        for (std::size_t i = 0; i < frame_count_; ++i)
        {
            auto& frame = frame_inputs_[(first_frame_ + i) % frame_inputs_.size()];
            if (frame.error.type == 0)
            {
                frame.error = error;
            }
        }
        frames->cancelSubmissions(error);
        if (config.execution_failed)
        {
            config.execution_failed(config.failure_context, error);
        }
    }

    RenderResult<HandlerCall> RenderServer::Impl::resolveCommandCall(
        CommandId command,
        const CommandRecord& entry,
        const ipc::IpcMessage& request,
        void*& storage
    ) noexcept
    {
        if (entry.storage)
        {
            const auto id = entry.storage(request);
            const HandlerBinding* selected = nullptr;
            for (const auto& binding : entry.bindings)
            {
                if (!binding.storage_type.isValid())
                {
                    continue;
                }
                auto borrowed = resources->borrow(id, binding.storage_type);
                if (!borrowed)
                {
                    continue;
                }
                if (selected)
                {
                    return cxx::unexpected(RenderError{kCommandContract});
                }
                storage = *borrowed;
                selected = &binding;
            }
            if (!selected)
            {
                return cxx::unexpected(RenderError{kStorageIdentity});
            }
            return selected->call;
        }
        if (!entry.implementation)
        {
            const auto* binding = entry.selectedBinding();
            if (!binding || !binding->call.object)
            {
                return cxx::unexpected(RenderError{kRuntimeUnsupported});
            }
            return binding->call;
        }
        const auto* implementation = capability_registry_.find(entry.implementation(request));
        if (!implementation)
        {
            return cxx::unexpected(RenderError{kRuntimeIdentity});
        }
        const auto method = std::ranges::find_if(
            implementation->command_bindings,
            [&](const auto& value) noexcept { return value.location.command == command; }
        );
        if (method == implementation->command_bindings.end())
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        const auto* binding = command_registry_.binding(method->location);
        if (!binding || !binding->call.object)
        {
            return cxx::unexpected(RenderError{kRuntimeUnsupported});
        }
        if (binding->storage_type.isValid())
        {
            if (method->storage_dependency >= implementation->storage_dependencies.size())
            {
                return cxx::unexpected(RenderError{kCommandContract});
            }
            const auto& dependency = implementation->storage_dependencies[method->storage_dependency];
            VRenderScope scope = GlobalRenderScope{};
            if (entry.scene)
            {
                scope = entry.scene(request);
            }
            if (scope.index() != static_cast<std::size_t>(dependency.scope))
            {
                return cxx::unexpected(RenderError{kStorageMismatch});
            }
            auto owner = findScope(scope);
            if (!owner)
            {
                return cxx::unexpected(owner.error());
            }
            ConfigValue configuration;
            if (dependency.configuration)
            {
                if (!implementation->make_default_configuration)
                {
                    return cxx::unexpected(RenderError{kStorageContract});
                }
                auto defaults = implementation->make_default_configuration();
                if (!defaults)
                {
                    return cxx::unexpected(defaults.error());
                }
                auto projected = dependency.configuration(defaults->data());
                if (!projected)
                {
                    return cxx::unexpected(projected.error());
                }
                configuration = std::move(*projected);
            }
            auto context = storageContext();
            const auto previous_size = (*owner)->size();
            auto id = (*owner)->getOrCreate(
                context, resources->typeId(dependency.storage_type), dependency.storage_key, configuration
            );
            if (!id)
            {
                return cxx::unexpected(id.error());
            }
            if ((*owner)->size() != previous_size)
            {
                active_frame_inputs_dirty = true;
                storages_created.store(resources->creations());
            }
            auto qualified = resources->borrow(*id, binding->storage_type);
            if (!qualified)
            {
                return cxx::unexpected(qualified.error());
            }
            storage = *qualified;
        }
        return binding->call;
    }

    void RenderServer::Impl::dispatchRequest(ipc::ReceivedRequest received) noexcept
    {
        const ResponseRoute route{received.requesterId(), RequestId{received.correlation()}};
        const auto id = commandIdFromTag(received.tag());
        const auto* entry = command_registry_.findCommand(id);
        const bool is_wrong_frame_source = id == submit_frame_command_ && route.requester_id != frame_source_;
        const bool requires_frame_boundary =
            (entry && entry->description->requires_frame_boundary) || id == select_binding_command_;
        if (requires_frame_boundary && frame_count_ != 0)
        {
            // Retain the already received head message, not another command queue. Later IPC
            // messages cannot overtake it; GPU/completion progress continues until older frames settle.
            deferred_request_.emplace(std::move(received));
            return;
        }
        if (!entry || route.request_id.isNull())
        {
            response_outbox_.emplaceResponse<AckResponse>(route, AckResponse{RenderError{kCommandContract}});
        }
        else if (received.message().typeHash() != entry->description->request.hash())
        {
            entry->make_error(response_outbox_.prepare(route), RenderError{kCommandContract});
        }
        else if (is_wrong_frame_source)
        {
            entry->make_error(response_outbox_.prepare(route), RenderError{kFrameSource});
        }
        else if (execution_error.type != 0 && !entry->description->failure_safe && id != submit_frame_command_)
        {
            entry->make_error(response_outbox_.prepare(route), execution_error);
        }
        else
        {
            void* storage = nullptr;
            auto call = resolveCommandCall(id, *entry, received.message(), storage);
            if (!call)
            {
                entry->make_error(response_outbox_.prepare(route), call.error());
            }
            else
            {
                structural_command_running_ = requires_frame_boundary;
                CommandDispatchContext context{
                    received.message(),
                    route,
                    response_outbox_,
                    command_scope_,
                    render_loop_,
                    requires_frame_boundary ? &structural_command_running_ : nullptr,
                    storage
                };
                // Async Handler construction consumes this exact call now; completion never
                // re-reads the capability directory or a selected Handler.
                call->invoke(call->object, context);
            }
        }
        ++commands;
    }

    void RenderServer::Impl::shutdown(RenderError cause) noexcept
    {
        stop_requested.store(true);
        if (frame_count_ != 0 && frame_inputs_[first_frame_].error.type == 0)
        {
            // A device/queue failure must reach the active frame; shutdown is not proof of cancellation.
            frame_inputs_[first_frame_].error = cause;
        }
        if (deferred_request_)
        {
            const ResponseRoute route{deferred_request_->requesterId(), RequestId{deferred_request_->correlation()}};
            const auto* entry = command_registry_.findCommand(commandIdFromTag(deferred_request_->tag()));
            entry->make_error(response_outbox_.prepare(route), cause);
            deferred_request_.reset();
        }
        command_scope_.close();
        command_scope_.request_stop();
        if (frames)
        {
            frames->cancelSubmissions(RenderError{kUploadStopped});
        }
        std::atomic<bool> joined{false};

        struct Joined final
        {
            using receiver_concept = stdexec::receiver_t;
            std::atomic<bool>* done;

            auto get_env() const noexcept
            {
                return stdexec::env{stdexec::prop{stdexec::get_scheduler, stdexec::inline_scheduler{}}};
            }

            void set_value() && noexcept
            {
                done->store(true, std::memory_order_release);
            }
        };

        auto join = stdexec::connect(command_scope_.join(), Joined{&joined});
        stdexec::start(join);
        publishResponses(32);
        // Keep RenderThread and GPU completion paths advancing until every accepted Sender settles.
        while (!joined.load(std::memory_order_acquire) || !render_loop_.empty())
        {
            // Adopt published CPU results while the Scope is draining.
            render_loop_.executeReady(128);
            publishResponses(32);
            if (device)
            {
                auto collected = device->pollQueues();
                device->dispatchQueueCompletions();
                if (frames)
                {
                    frames->dispatchWrites();
                    frames->cancelSubmissions(RenderError{kUploadStopped});
                }
                // Stopped upload continuations must run after GPU/write-admission callbacks.
                render_loop_.executeReady(128);
                progressUploads();
                if (!collected && !device->deviceLost())
                {
                    std::terminate();
                }
                progressFrame();
            }
            // Settle frame terminals before testing Scope completion again.
            render_loop_.executeReady(128);
            publishResponses(32);
            if (!joined.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
        }
        while (render_loop_.activePublishers() != 0)
        {
            std::this_thread::yield();
        }
        while (!response_outbox_.empty())
        {
            const auto observed = ipc::IpcCore::instance().workEpoch();
            if (publishResponses(SIZE_MAX) == 0)
            {
                ipc::IpcCore::instance().waitUntil(
                    observed,
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(1)
                );
            }
        }
        responder.reset();
        ipc::IpcCore::instance().notifyWork(); // Wake bootstrap/reply waiters after CLOSED is published.
        while (!active_targets.empty())
        {
            auto& target = *targets.find(active_targets.keys().back());
            detachAttachment(target.attachment);
        }
        // End plan owners while physical cleanup is still admitted. The Session closes only
        // after Scene/Global Storage and every other producer have transferred their resources.
        effects.clear();
        targets.clear();
        scenes.clear();
        command_registry_.clear(); // All operations ended; the borrowed resource directory is still alive.
        if (global_storages)
        {
            global_storages->clear();
        }
        global_storages.reset();
        resources.reset();
        // Frame callback nodes and Staging outlive all accepted GPU submissions.
        if (auto settled = device->drain(); !settled && !device->deviceLost())
        {
            std::terminate();
        }
        frames.reset();
        uploads.reset();
        device.reset();
        running.store(false);
    }

    std::size_t RenderServer::Impl::publishResponses(std::size_t limit) noexcept
    {
        const auto result = response_outbox_.flushTo(responder, limit);
        if (result.published != 0 && config.responses_available)
        {
            config.responses_available(config.response_context);
        }
        return result.settled;
    }

    RenderResult<void> RenderServer::Impl::run() noexcept
    {
        auto& s = *this;
        if (s.started.exchange(true))
        {
            return cxx::unexpected(RenderError{kRuntimeState});
        }
        if (s.stop_requested.load())
        {
            s.shutdown();
            return {};
        }
        s.command_registry_.activateHandlers(*s.resources);
        s.running.store(true);
        RenderResult<void> result;
        auto& notifications = ipc::IpcCore::instance();
        while (!s.stop_requested.load())
        {
            // Snapshot precedes every readiness check. A publication in the check/wait gap
            // changes the epoch, so WaitOnAddress/futex cannot sleep through that publication.
            const auto observed = notifications.workEpoch();
            const auto submitted_before = s.submissions.load();
            // Safe point 1: adopt already-published CPU results before accepting more commands.
            bool progressed = s.render_loop_.executeReady(128) != 0;
            // A structural request is admitted between complete frames. Its asynchronous terminal
            // opens the boundary again; this does not block GPU or stdexec completion progress.
            if (s.deferred_request_ && s.frame_count_ == 0 && !s.structural_command_running_)
            {
                auto request = std::move(*s.deferred_request_);
                s.deferred_request_.reset();
                s.dispatchRequest(std::move(request));
                progressed = true;
            }
            for (std::uint32_t i = 0; i < 32; ++i)
            {
                const bool can_receive =
                    !s.stop_requested.load() && !s.deferred_request_ && !s.structural_command_running_;
                if (!can_receive)
                {
                    break;
                }
                ipc::ReceivedRequest request;
                if (s.responder.tryReceive(request) == ipc::EIpcReceive::RECEIVED)
                {
                    s.dispatchRequest(std::move(request));
                    progressed = true;
                }
            }
            if (s.stop_requested.load())
            {
                break;
            }
            progressed |= s.publishResponses(32) != 0;
            if (auto observed_gpu = s.progressGpuCompletions(); !observed_gpu)
            {
                s.failExecution(observed_gpu.error());
            }
            s.frames->dispatchWrites();
            // Safe point 2: GPU callbacks/write admission can schedule continuations required
            // by this frame. Count their progress before deciding whether the loop may wait.
            progressed |= s.render_loop_.executeReady(128) != 0;
            s.progressUploads();
            // Staging availability resumes a Task through the same scheduler. Let that continuation
            // consume its admitted range before frame preparation closes background staging again;
            // otherwise continuously submitted frames can wake and immediately requeue it forever.
            progressed |= s.render_loop_.executeReady(128) != 0;
            s.progressFrame();
            // A submitted frame may have returned its CPU input before a present-fence slot
            // becomes reusable. Continue presentation even when no new input frame is queued.
            if (s.frame_count_ == 0)
            {
                if (auto presented = s.presentFrameOutputs(); presented)
                {
                    progressed |= *presented != 0;
                }
                else
                {
                    s.failExecution(presented.error());
                }
            }
            // Safe point 3: publish terminal frame/command results produced by this iteration.
            progressed |= s.render_loop_.executeReady(128) != 0;
            progressed |= s.publishResponses(32) != 0;
            progressed |= s.submissions.load() != submitted_before;
            ++s.ticks;
            if (!progressed && !s.stop_requested.load())
            {
                // A publisher may be counted before its node is visible. Its eventual wakeup
                // changes this epoch; spinning on RunLoop::size() would delay other producers.
                if (notifications.workEpoch() != observed)
                {
                    continue;
                }
                if (s.device->pendingGpuWork() && s.execution_error.type == 0)
                {
                    if (auto advanced = s.frames->waitForProgress(s.config.gpu_wait_ns); !advanced)
                    {
                        s.failExecution(advanced.error());
                    }
                }
                else
                {
                    // Receive only inspected a bounded prefix of Links. Preserve its finite revisit.
                    notifications.waitUntil(observed, std::chrono::steady_clock::now() + std::chrono::milliseconds(1));
                }
            }
        }
        if (s.execution_error.type != 0)
        {
            result = cxx::unexpected(s.execution_error);
        }
        s.shutdown(result ? RenderError{kCommandCancelled} : result.error());
        return result;
    }

    RenderServerStats RenderServer::Impl::stats() const noexcept
    {
        const auto& s = *this;
        return {
            s.ticks.load(),
            s.commands.load(),
            s.compiled_graphs.load(),
            s.submissions.load(),
            s.validation_errors.load(),
            s.validation_warnings.load(),
            s.storages_created.load(),
            s.presentations.load()
        };
    }

} // namespace lux::render

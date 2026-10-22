#include <exec/variant_sender.hpp>
#include <lux/engine/render/device/DescriptorTable.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>
#include <ranges>

namespace lux::render
{
    RenderServer::Impl::Impl(RenderServerConfig value, ipc::IpcResponder input, execution::CpuScheduler cpu_scheduler)
        : config(std::move(value)), cpu_scheduler(std::move(cpu_scheduler)), responder(std::move(input)),
          render_loop_([](void*) noexcept { ipc::IpcCore::instance().notifyWork(); })
    {
        bindFrameworkCommand<CreateScene, &Impl::createScene>();
        bindFrameworkCommand<RemoveScene, &Impl::removeScene>();
        bindFrameworkCommand<CreateView, &Impl::createView>();
        bindFrameworkCommand<UpdateAttachmentInputs, &Impl::updateAttachmentInputs>();
        bindFrameworkCommand<InvalidateHistory, &Impl::invalidateHistory>();
        bindFrameworkCommand<RemoveView, &Impl::removeView>();
        bindFrameworkCommand<CreateTarget, &Impl::createTarget>();
        bindFrameworkCommand<ResizeTarget, &Impl::resizeTarget>();
        bindCreateEffectCommand();
        bindFrameworkCommand<RemoveEffect, &Impl::removeEffect>();
        bindFrameworkCommand<Attach, &Impl::attachEffect>();
        bindFrameworkCommand<Detach, &Impl::detachEffect>();
        bindFrameworkCommand<Stop, &Impl::stopCommand>();
        bindFrameworkCommand<QueryRenderStats, &Impl::queryRenderStats>();
        bindFrameworkCommand<QueryExecutionProgress, &Impl::queryExecutionProgress>();
        bindFrameworkCommand<ListFeatures, &Impl::listFeatures>();
        bindFrameworkCommand<ListStorages, &Impl::listStorages>();
        bindFrameworkCommand<FindStorage, &Impl::findStorage>();
        bindFrameworkCommand<PrepareStorage, &Impl::prepareStorage>();
        bindFrameworkCommand<RemoveStorage, &Impl::removeStorage>();
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
            (s.device->descriptors().imageCapacity() ? 1u : 0u) | (enabled.texture_compression_bc ? 2u : 0u)
        );
        s.global_storages.emplace(*s.resources, true);
        s.paged_instances = device_options.buffer_device_address;
        return {};
    }

    auto RenderServer::Impl::createEffect(CreateEffect::Request&& input) noexcept
    {
        RenderError initial_error;
        std::vector<const FeatureEntry*> registrations;
        if (input.definition.features.empty())
        {
            initial_error = {kFeatureContract};
        }
        registrations.reserve(input.definition.features.size());
        for (const auto& selection : input.definition.features)
        {
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
        const auto sampled_image_capacity = device->descriptors().imageCapacity();
        const auto sampler_capacity = device->descriptors().samplerCapacity();
        // Workers receive only immutable feature entries, the request and actual device facts.
        // The final continuation is the sole place that adopts a candidate into the Effect table.
        auto compiled =
            stdexec::schedule(cpu_scheduler) |
            stdexec::then(
                [native = &device->device(),
                 nri_core = &device->nriCore(),
                 enabled = device->enabledFeatures(),
                 formats = device->formatSupport(),
                 vertex_limits = std::array{device->vertexStrideLimit(), device->vertexOffsetLimit()},
                 queues =
                     std::array{
                         &device->queue(detail::EQueueRole::GRAPHICS).properties(),
                         &device->queue(detail::EQueueRole::COMPUTE).properties(),
                         &device->queue(detail::EQueueRole::TRANSFER).properties()
                     },
                 sampled_image_capacity,
                 sampler_capacity,
                 storage_types = std::move(storage_types),
                 request = std::move(input),
                 entries = std::move(registrations),
                 limit = config.max_config_bytes]() noexcept
                {
                    return compileEffect(
                        *native,
                        *nri_core,
                        enabled,
                        formats,
                        vertex_limits,
                        queues,
                        sampled_image_capacity,
                        sampler_capacity,
                        storage_types,
                        request,
                        entries,
                        limit
                    );
                }
            ) |
            stdexec::continues_on(render_loop_.scheduler()) |
            stdexec::let_value(
                [server = this](RenderResult<RenderEffect>& candidate) noexcept
                {
                    return stdexec::read_env(stdexec::get_stop_token) |
                           stdexec::then(
                               [server, &candidate](auto stop) noexcept -> CreateEffect::Response
                               {
                                   if (!candidate)
                                   {
                                       return {candidate.error()};
                                   }
                                   if (stop.stop_requested())
                                   {
                                       return {RenderError{kCommandCancelled}};
                                   }
                                   const auto effect = server->effects.emplace(std::move(*candidate));
                                   ++server->compiled_graphs;
                                   return EffectCreated{{}, effect};
                               }
                           );
                }
            );
        auto invalid = stdexec::just(CreateEffect::Response{initial_error});
        using VSource = exec::variant_sender<decltype(invalid), decltype(compiled)>;
        return initial_error.type != error::ErrorId{} ? VSource{std::move(invalid)} : VSource{std::move(compiled)};
    }

    void RenderServer::Impl::bindCreateEffectCommand() noexcept
    {
        bindFrameworkCommand<CreateEffect, &Impl::createEffect>();
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
        if (attachmentsSubmitting(effect->attachments))
        {
            output_response.error = RenderError{kDeviceBusy};
            return;
        }
        while (!effect->attachments.empty())
        {
            detachAttachment(effect->attachments.back());
        }
        device->deferRelease([plan = std::move(effect->plan)]() mutable noexcept { plan.reset(); });
        effects.erase(input->effect);
        output_response = {};
    }

    RenderResult<EffectAttachment> RenderServer::Impl::prepareAttachment(
        const RenderEffect& effect,
        detail::OffscreenTarget& output,
        StorageSet& scene,
        StorageSet& view,
        StorageSet& attachment
    ) noexcept
    {
        const auto& plan = *effect.plan;
        auto dependencies = storageContext();
        std::vector<const StorageArgument*> resolved_arguments(plan.resources().size());
        std::vector<bool> required(plan.resources().size());
        for (std::size_t i = 0; i < required.size(); ++i)
        {
            required[i] = plan.resources()[i].used;
        }
        // Contributions are in the port topological order. Backward closure retains only the
        // preparation inputs of live outputs; it does not invent GPU accesses or another graph.
        for (const auto& binding : effect.bindings | std::views::reverse)
        {
            if (std::ranges::any_of(binding.outputs, [&](auto id) { return required[id.index]; }))
            {
                for (auto input : binding.inputs)
                {
                    required[input.index] = true;
                }
            }
        }
        for (const auto& argument : effect.arguments)
        {
            const auto index = argument.resource.index;
            if (index >= plan.resources().size())
            {
                return cxx::unexpected(RenderError{kStorageContract});
            }
            if (!required[index])
            {
                continue;
            }
            const auto* previous = resolved_arguments[index];
            if (previous && *previous != argument)
            {
                return cxx::unexpected(RenderError{kStorageMismatch});
            }
            resolved_arguments[index] = &argument;
        }
        std::vector<std::pair<StorageSet*, StorageId>> created_storages;
        cxx::scope_exit rollback(
            [&]() noexcept
            {
                for (const auto& [set, id] : created_storages)
                {
                    (void)set->remove(id);
                }
            }
        );
        EffectAttachment candidate;
        std::uint32_t parameter_bytes = 0;
        for (const auto& binding : effect.bindings)
        {
            if (std::ranges::any_of(binding.outputs, [&](auto id) { return required[id.index]; }))
            {
                parameter_bytes = std::max(parameter_bytes, binding.parameter_bytes);
            }
        }
        candidate.instance = std::make_unique<detail::RenderGraphInstance>(
            *device,
            frames->frameCount(),
            plan,
            &output,
            parameter_bytes
        );
        candidate.storage_inputs.reserve(plan.resources().size());
        std::vector<StorageId> resolved(plan.resources().size());
        const auto prepareGroup = [&](std::span<const GraphStorageBinding> bindings,
                                      StorageSet* set) -> RenderResult<void>
        {
            for (const auto& binding : bindings)
            {
                const auto i = binding.resource.index;
                const auto& resource = plan.resources()[i];
                if (!required[i])
                {
                    continue;
                }
                if (!set)
                {
                    return cxx::unexpected(RenderError{kStorageIdentity});
                }
                const auto* factory = dependencies.resources.factory(binding.storage.type.view());
                const auto key = binding.storage.key.empty() ? resource.key : binding.storage.key;
                const auto* argument = resolved_arguments[i];
                const auto type = argument ? argument->type() : cxx::TypeToken{};
                if (type != factory->descriptor->binding_type)
                {
                    return cxx::unexpected(RenderError{kStorageContract});
                }
                const auto* value = argument ? argument->data() : nullptr;
                const auto* ops = static_cast<const detail::StorageBindingOps*>(factory->binding_ops);
                const auto storage_type = dependencies.resources.typeId(binding.storage.type.view());
                const auto previous = set->find(storage_type, key);
                auto instance = set->getOrCreate(dependencies, storage_type, key, binding.storage.config);
                if (!instance)
                {
                    return cxx::unexpected(instance.error());
                }
                const auto id = *instance;
                if (previous.isNull() && set != &attachment)
                {
                    created_storages.emplace_back(set, id);
                }
                resolved[i] = id;
                if (auto bound =
                        candidate.bindStorage(i, id, *ops, resources->storageInstance(id), value, frames->frameCount());
                    !bound)
                {
                    return cxx::unexpected(bound.error());
                }
            }
            return {};
        };
        const auto& groups = effect.storage_requirements;
        if (auto r = prepareGroup(groups.global, &*global_storages); !r)
        {
            return cxx::unexpected(r.error());
        }
        if (auto r = prepareGroup(groups.scene, &scene); !r)
        {
            return cxx::unexpected(r.error());
        }
        if (auto r = prepareGroup(groups.view, &view); !r)
        {
            return cxx::unexpected(r.error());
        }
        if (auto r = prepareGroup(groups.attachment, &attachment); !r)
        {
            return cxx::unexpected(r.error());
        }
        for (const auto& binding : effect.bindings)
        {
            if (std::ranges::none_of(binding.outputs, [&](auto id) { return required[id.index]; }))
            {
                continue;
            }
            auto created = binding.create(*device, *frames, *resources, resolved, binding.configuration.data());
            if (!created)
            {
                return cxx::unexpected(created.error());
            }
            created->inputs = binding.inputs;
            created->resolved_inputs.resize(binding.inputs.size());
            for (std::uint32_t slot = 0; slot < frames->frameCount(); ++slot)
            {
                for (const auto& update : created->initial_resources)
                {
                    candidate.instance->setResource(update.resource.index, slot, update.input);
                }
            }
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
        rollback.release();
        return candidate;
    }

    RenderResult<StorageSet*> RenderServer::Impl::findStorageSet(const VStorageLocation& address) noexcept
    {
        return std::visit(
            [&](const auto& location) -> RenderResult<StorageSet*>
            {
                using T = std::decay_t<decltype(location)>;
                if constexpr (std::same_as<T, GlobalStorageLocation>)
                {
                    return &*global_storages;
                }
                else if constexpr (std::same_as<T, SceneId>)
                {
                    auto* scene = scenes.find(location);
                    if (!scene)
                    {
                        return cxx::unexpected(RenderError{kRuntimeIdentity});
                    }
                    return &scene->storages;
                }
                else if constexpr (std::same_as<T, ViewRequest>)
                {
                    auto* scene = scenes.find(location.scene);
                    auto* view = scene ? scene->views.find(location.view) : nullptr;
                    if (!view)
                    {
                        return cxx::unexpected(RenderError{kRuntimeIdentity});
                    }
                    return &view->storages;
                }
                else
                {
                    auto* attachment = attachments.find(location);
                    if (!attachment)
                    {
                        return cxx::unexpected(RenderError{kRuntimeIdentity});
                    }
                    return attachment->storages.get();
                }
            },
            address
        );
    }

    void RenderServer::Impl::findStorage(
        const FindStorage::Request& _request,
        FindStorage::Response& output_response
    ) noexcept
    {
        const auto& input = _request;
        auto storage_set = findStorageSet(input.location);
        if (!storage_set)
        {
            output_response.error = storage_set.error();
            return;
        }
        if (!resources->factory(input.type))
        {
            output_response.error = RenderError{kStorageFactory};
            return;
        }
        const auto id = (*storage_set)->find(input.type, input.key);
        if (id.isNull())
        {
            output_response.error = RenderError{kStorageIdentity};
            return;
        }
        output_response = {{}, id};
    }

    void RenderServer::Impl::prepareStorage(
        const PrepareStorage::Request& _request,
        PrepareStorage::Response& output_response
    ) noexcept
    {
        const auto& input = _request;
        auto storage_set = findStorageSet(input.location.location);
        if (!storage_set)
        {
            output_response.error = storage_set.error();
            return;
        }
        auto dependencies = storageContext();
        auto id = (*storage_set)->getOrCreate(dependencies, input.location.type, input.location.key, input.config);
        if (!id)
        {
            output_response.error = id.error();
            return;
        }
        output_response = {{}, *id};
    }

    void RenderServer::Impl::removeStorage(
        const RemoveStorage::Request& _request,
        RemoveStorage::Response& output_response
    ) noexcept
    {
        const auto id = _request;
        auto* storage_set = resources->storageSet(id);
        if (!storage_set)
        {
            output_response.error = RenderError{kStorageIdentity};
            return;
        }
        if (frames->submitting())
        {
            output_response.error = RenderError{kDeviceBusy};
            return;
        }
        if (auto result = storage_set->remove(id); !result)
        {
            output_response.error = result.error();
            return;
        }
        output_response = {};
    }

    bool RenderServer::Impl::attachmentsSubmitting(std::span<const AttachmentId> ids) const noexcept
    {
        return std::ranges::any_of(ids, [&](auto id) { return frames->submitting(*attachments.find(id)->instance); });
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
        target.attachment = {};
        active_targets.erase(attachment.target);
        frame_storages_dirty = true;
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
        if (!target->attachment.isNull())
        {
            output_response.error = RenderError{kRuntimeState};
            return;
        }
        if (auto valid = _request.viewport.validate(); !valid)
        {
            output_response.error = valid.error();
            return;
        }
        auto local_storages = std::make_unique<StorageSet>(*resources);
        auto prepared = prepareAttachment(*effect, *target->physical, scene->storages, view->storages, *local_storages);
        if (!prepared)
        {
            output_response.error = prepared.error();
            return;
        }
        if (auto values = prepared->instance->setInputs(_request.parameters, _request.conditions); !values)
        {
            output_response.error = values.error();
            return;
        }
        prepared->instance->setViewport(_request.viewport);
        output_response = {};
        for (const auto& input : prepared->storage_inputs)
        {
            output_response.storages.push_back({effect->plan->resources()[input.resource].key, input.storage_id});
        }
        prepared->effect = effect_id;
        prepared->scene = scene_id;
        prepared->view = view_id;
        prepared->target = target_id;
        prepared->storages = std::move(local_storages);
        const auto id = attachments.emplace(std::move(*prepared));
        auto& attachment = *attachments.find(id);
        attachment.view_position = view->attachments.size();
        attachment.effect_position = effect->attachments.size();
        view->attachments.push_back(id);
        effect->attachments.push_back(id);
        target->attachment = id;
        output_response.attachment = id;
        active_targets.emplace(target_id);
        frame_storages_dirty = true;
        storages_created.store(resources->creations());
        return;
    }

    RenderResult<void> RenderServer::Impl::resizeTargetResources(
        RenderTarget& target,
        const TargetDesc& description
    ) noexcept
    {
        auto physical = detail::OffscreenTarget::create(*device, description, frames->frameCount());
        if (!physical)
        {
            return cxx::unexpected(physical.error());
        }
        auto* group = target.attachment.isNull() ? nullptr : attachments.find(target.attachment);
        if (group)
        {
            // One Target writer, proven on attach. Resize builds one candidate without recompilation.
            const auto& effect = *effects.find(group->effect);
            auto candidate = prepareAttachment(
                effect,
                **physical,
                scenes.find(group->scene)->storages,
                scenes.find(group->scene)->views.find(group->view)->storages,
                *group->storages
            );
            if (!candidate)
            {
                return cxx::unexpected(candidate.error());
            }
            if (auto values = candidate->instance
                                  ->setInputs(group->instance->instanceParameters(), group->instance->conditions());
                !values)
            {
                return cxx::unexpected(values.error());
            }
            candidate->instance->setViewport(group->instance->viewport());
            group->swapResources(*candidate);
            frame_storages_dirty = true;
        }
        target.physical = std::move(*physical);
        target.description = description;
        ++target.epoch;
        storages_created.store(resources->creations());
        return {};
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
            if (attachmentsSubmitting(view.attachments))
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

    void RenderServer::Impl::createView(
        const CreateView::Request& _request,
        CreateView::Response& output_response
    ) noexcept
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
        if (auto applied = attachment->instance->setInputs(input.parameters, input.conditions); !applied)
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

    void RenderServer::Impl::removeView(
        const RemoveView::Request& _request,
        RemoveView::Response& output_response
    ) noexcept
    {
        const auto* input = &_request;
        auto* scene = scenes.find(input->scene);
        auto* view = scene ? scene->views.find(input->view) : nullptr;
        if (!view)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        if (attachmentsSubmitting(view->attachments))
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

    void RenderServer::Impl::createTarget(
        const CreateTarget::Request& _request,
        CreateTarget::Response& output_response
    ) noexcept
    {
        const auto* input = &_request;
        if (auto valid = input->description.validate(); !valid)
        {
            output_response.error = valid.error();
            return;
        }
        if (input->description.kind != ETargetKind::OFFSCREEN)
        {
            output_response.error = RenderError{kRuntimeUnsupported};
            return;
        }
        auto physical = detail::OffscreenTarget::create(*device, input->description, frames->frameCount());
        if (!physical)
        {
            output_response.error = physical.error();
            return;
        }
        output_response = TargetCreated{{}, targets.emplace(std::move(*physical)), 1};
    }

    void RenderServer::Impl::resizeTarget(
        const ResizeTarget::Request& _request,
        ResizeTarget::Response& output_response
    ) noexcept
    {
        const auto* input = &_request;
        if (auto valid = input->description.validate(); !valid)
        {
            output_response.error = valid.error();
            return;
        }
        if (input->description.kind != ETargetKind::OFFSCREEN)
        {
            output_response.error = RenderError{kRuntimeUnsupported};
            return;
        }
        auto* target = targets.find(input->target);
        if (!target)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        if (!target->attachment.isNull() && attachmentsSubmitting({&target->attachment, 1}))
        {
            output_response.error = RenderError{kDeviceBusy};
            return;
        }
        if (target->epoch == UINT64_MAX)
        {
            output_response.error = RenderError{kRuntimeCapacity};
            return;
        }
        if (auto changed = resizeTargetResources(*target, input->description); !changed)
        {
            output_response.error = changed.error();
            return;
        }
        output_response = TargetCreated{{}, input->target, target->epoch};
    }

    void RenderServer::Impl::detachEffect(const Detach::Request& _request, Detach::Response& output_response) noexcept
    {
        const auto* attachment = attachments.find(_request);
        if (!attachment)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        if (frames->submitting(*attachment->instance))
        {
            output_response.error = RenderError{kDeviceBusy};
            return;
        }
        detachAttachment(_request);
        output_response = {};
    }

    void RenderServer::Impl::listFeatures(
        const ListFeatures::Request& _request,
        ListFeatures::Response& output_response
    ) noexcept
    {
        FeatureList result;
        result.features.reserve(features.size());
        for (const auto& entry : features)
        {
            result.features.push_back(
                {entry.id,
                 std::string(entry.descriptor.type.name()),
                 std::string(entry.descriptor.config.name()),
                 std::string(entry.descriptor.description)}
            );
        }
        output_response = std::move(result);
    }

    void RenderServer::Impl::listStorages(
        const ListStorages::Request& _request,
        ListStorages::Response& output_response
    ) noexcept
    {
        StorageList result;
        resources->forEachType(
            [&](StorageTypeId id, const StorageDescriptor& type, std::uint32_t capabilities) noexcept
            {
                result.storages.push_back(
                    {id, std::string(type.type.name()), capabilities, std::string(type.description)}
                );
            }
        );
        output_response = std::move(result);
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

    RenderResult<void> RenderServer::Impl::progressCompletions() noexcept
    {
        if (auto observed = device->pollQueues(); !observed)
        {
            return observed;
        }
        device->dispatchQueueCompletions();
        frames->dispatchWrites();
        render_loop_.executeReady(128);
        return {};
    }

    void RenderServer::Impl::progressUploads() noexcept
    {
        if (uploads)
        {
            uploads->submitReadyTransfers();
        }
    }

    RenderResult<void> RenderServer::Impl::submitCurrentFrame() noexcept
    {
        if (frame_storages_dirty)
        {
            // Semantic conflict order is stable. Fairness rotates ready physical queues, not
            // the order of writers of one imported resource.
            const auto keys = active_targets.keys();
            std::vector<TargetId> active(keys.begin(), keys.end());
            std::ranges::sort(active, {}, &TargetId::index);
            frame_instances.clear();
            frame_storages.clear();
            frame_features.clear();
            for (auto id : active)
            {
                auto& attachment = *attachments.find(targets.find(id)->attachment);
                frame_instances.push_back(attachment.instance.get());
                for (const auto& input : attachment.storage_inputs)
                {
                    if (input.ops->prepareFrame)
                    {
                        frame_storages.push_back(
                            {input.native,
                             input.ops,
                             input.storage_id,
                             input.ops->preparationRevision ? input.ops->preparationRevision(input.native) : nullptr}
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
            frame_storages_dirty = false;
        }
        auto accepted = frames->render(frame_instances, frame_storages, frame_features);
        if (!accepted)
        {
            return cxx::unexpected(accepted.error());
        }
        submissions += *accepted;
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
        stop_requested.store(true);
        output_response = {};
    }

    void RenderServer::Impl::dispatchRequest(ipc::ReceivedRequest received) noexcept
    {
        const ResponseRoute route{received.requesterId(), RequestId{received.correlation()}};
        const auto id = commandIdFromTag(received.tag());
        const auto* entry = command_registry_.findCommand(id);
        const bool has_active_frame =
            frame_count_ != 0 && frame_inputs_[first_frame_].stage != FrameRecord::EStage::QUEUED;
        const bool is_wrong_frame_source = id == submit_frame_command_ && route.requester_id != frame_source_;
        const bool requires_frame_boundary =
            (entry && entry->description->frame_scoped) || (id == select_binding_command_ && has_active_frame);
        if (!entry || route.request_id.isNull())
        {
            response_outbox_.emplaceResponse<AckResponse>(route, AckResponse{RenderError{kCommandContract}});
        }
        else if (received.message().typeHash() != entry->description->request.hash())
        {
            entry->make_error(response_outbox_.prepare(route), RenderError{kCommandContract});
        }
        else if (!entry->selectedBinding())
        {
            entry->make_error(response_outbox_.prepare(route), RenderError{kRuntimeUnsupported});
        }
        else if (is_wrong_frame_source)
        {
            entry->make_error(response_outbox_.prepare(route), RenderError{kFrameSource});
        }
        else if (requires_frame_boundary)
        {
            entry->make_error(response_outbox_.prepare(route), RenderError{kFrameBoundary});
        }
        else
        {
            CommandDispatchContext context{received.message(), route, response_outbox_, command_scope_, render_loop_};
            const auto active = entry->selectedBinding()->call;
            active.invoke(active.object, context);
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
        response_outbox_.flushTo(responder, 32);
        // Keep RenderThread and GPU completion paths advancing until every accepted Sender settles.
        while (!joined.load(std::memory_order_acquire) || !render_loop_.empty())
        {
            render_loop_.executeReady(128);
            response_outbox_.flushTo(responder, 32);
            if (device)
            {
                auto collected = device->pollQueues();
                device->dispatchQueueCompletions();
                if (frames)
                {
                    frames->dispatchWrites();
                    frames->cancelSubmissions(RenderError{kUploadStopped});
                }
                if (uploads)
                {
                    uploads->submitReadyTransfers();
                }
                if (!collected && !device->deviceLost())
                {
                    std::terminate();
                }
                progressFrame();
            }
            if (!joined.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
        }
        while (render_loop_.activePublishers() != 0)
        {
            std::this_thread::yield();
        }
        response_outbox_.flushTo(responder, SIZE_MAX);
        response_outbox_.clear(); // Connection closes after safe operations; caller owns any unknown outcomes.
        responder.reset();
        while (!active_targets.empty())
        {
            auto& target = *targets.find(active_targets.keys().back());
            detachAttachment(target.attachment);
        }
        // End plan owners while physical cleanup is still admitted. The Session closes only
        // after Scene/Global Storage and every other producer have transferred their resources.
        for (auto& effect : effects)
        {
            device->deferRelease([plan = std::move(effect.plan)]() mutable noexcept { plan.reset(); });
        }
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
        s.command_registry_.activateSelectedHandlers(*s.resources);
        s.running.store(true);
        RenderResult<void> result;
        auto& notifications = ipc::IpcCore::instance();
        while (!s.stop_requested.load())
        {
            // Snapshot precedes every readiness check. A publication in the check/wait gap
            // changes the epoch, so WaitOnAddress/futex cannot sleep through that publication.
            const auto observed = notifications.workEpoch();
            const auto submitted_before = s.submissions.load();
            bool progressed = s.render_loop_.executeReady(128) != 0;
            // Service batching is independent of the sealed frame boundary.
            for (std::uint32_t i = 0; i < 32 && !s.stop_requested.load(); ++i)
            {
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
            progressed |= s.response_outbox_.flushTo(s.responder, 32) != 0;
            if (auto observed_gpu = s.progressCompletions(); !observed_gpu)
            {
                result = cxx::unexpected(observed_gpu.error());
                break;
            }
            s.progressUploads();
            s.progressFrame();
            progressed |= s.render_loop_.executeReady(128) != 0;
            progressed |= s.response_outbox_.flushTo(s.responder, 32) != 0;
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
                if (s.device->pendingGpuWork())
                {
                    if (auto advanced = s.frames->waitForProgress(s.config.gpu_wait_ns); !advanced)
                    {
                        result = cxx::unexpected(advanced.error());
                        break;
                    }
                }
                else
                {
                    // Receive only inspected a bounded prefix of Links. Preserve its finite revisit.
                    notifications.waitUntil(observed, std::chrono::steady_clock::now() + std::chrono::milliseconds(1));
                }
            }
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
            s.storages_created.load()
        };
    }

} // namespace lux::render

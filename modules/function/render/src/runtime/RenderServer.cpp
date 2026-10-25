#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/extensions/FeatureRegistration.hpp>
#include <lux/engine/render/features/BuiltinFeatures.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>

namespace lux::render
{
    namespace
    {
        bool sameFactory(const StorageFactory& left, const StorageFactory& right) noexcept
        {
            return left.implementation_type == right.implementation_type && left.config_type == right.config_type &&
                   left.ops_type == right.ops_type && left.create == right.create && left.ops == right.ops &&
                   left.binding_ops == right.binding_ops && left.compatible_config == right.compatible_config;
        }

    } // namespace

    RenderServer::RenderServer(std::unique_ptr<Impl> state) noexcept : impl_(std::move(state)) {}

    RenderServer::~RenderServer() noexcept
    {
        if (impl_->running.load())
        {
            std::terminate(); // the application must stop and join its rendering thread
        }
        if (impl_->device)
        {
            impl_->shutdown();
        }
    }

    RenderResult<std::unique_ptr<RenderServer>> RenderServer::create(
        RenderServerConfig config,
        execution::ExecutionRuntime& execution,
        DeviceOptions device_options
    ) noexcept
    {
        const bool invalid = config.frames_in_flight == 0 || config.frames_in_flight > 3 ||
                             config.max_frames_ahead == UINT32_MAX || config.max_config_bytes == 0 ||
                             config.gpu_wait_ns > 1'000'000;
        if (invalid)
        {
            return cxx::unexpected(RenderError{kRuntimeCapacity});
        }
        auto responder = ipc::IpcCore::instance().createResponder(config.channel_name);
        if (!responder)
        {
            return cxx::unexpected(RenderError{kRuntimeState});
        }
        auto state = std::make_unique<RenderServer::Impl>(std::move(config), std::move(*responder), execution.cpu());
        if (auto initialized = state->initialize(device_options); !initialized)
        {
            return cxx::unexpected(initialized.error());
        }
        state->registerBuiltinFeatures();
        return std::unique_ptr<RenderServer>(new RenderServer(std::move(state)));
    }

    RenderResult<void> RenderServer::setFrameSource(ipc::RequesterId requester) noexcept
    {
        if (impl_->started.load() || requester.isNull())
        {
            return cxx::unexpected(RenderError{kRuntimeState});
        }
        impl_->frame_source_ = requester;
        return {};
    }

    void RenderServer::Impl::registerBuiltinFeatures() noexcept
    {
        // Built-ins have a closed, statically typed dependency set. No candidate owners,
        // external compatibility checks or rollback transaction belong on this path.
        cxx::tuple_traits::for_each_type<detail::BuiltinFeatures>(
            [&]<class Feature, std::size_t>() noexcept
            {
                using StorageTypes = decltype(uniqueTypes(static_cast<typename Feature::Storages*>(nullptr)));
                const auto required = []<class... Types>(std::tuple<Types...>*) noexcept
                {
                    return (0u | ... | Types::type::Descriptor::descriptor.required_capabilities);
                }(static_cast<StorageTypes*>(nullptr));
                if ((required & ~resources->capabilities()) != 0)
                {
                    // Keep selectable metadata so applications can distinguish an unsupported implementation
                    // from an unknown name. No unsupported factory or handler is installed.
                    if (featureCapabilities<Feature>())
                    {
                        publishFeature(
                            Feature::name.name(),
                            featureConfigType<Feature>(),
                            Feature::graph(paged_instances),
                            featureCapabilities<Feature>()
                        );
                    }
                    return;
                }
                cxx::tuple_traits::for_each_type<StorageTypes>(
                    [&]<class Type, std::size_t>() noexcept
                    {
                        using Storage = typename Type::type;
                        if (resources->typeId(Storage::Descriptor::descriptor.type).isNull())
                        {
                            resources->publishFactory(Storage::factory());
                        }
                    }
                );
                using HandlerTypes = decltype(handlerTypes(static_cast<typename Feature::Commands*>(nullptr)));
                cxx::tuple_traits::for_each_type<HandlerTypes>(
                    [&]<class Type, std::size_t>() noexcept
                    {
                        using Handler = typename Type::type;
                        if (command_registry_.handlerId(cxx::typeToken<Handler>(), {}).isNull())
                        {
                            command_registry_.publishHandler(handlerRecord<Handler>({}));
                        }
                    }
                );
                for (const auto& binding : commandBindings(static_cast<typename Feature::Commands*>(nullptr)))
                {
                    command_registry_.publishBinding(command_registry_.handlerId(binding.handlerType(), {}), binding);
                }
                publishFeature(
                    Feature::name.name(),
                    featureConfigType<Feature>(),
                    Feature::graph(paged_instances),
                    featureCapabilities<Feature>()
                );
            }
        );
    }

    FeatureId RenderServer::Impl::publishFeature(
        std::string_view name,
        cxx::TypeToken config_type,
        const FeatureGraphOps& graph,
        const CapabilitySupport* capabilities
    ) noexcept
    {
        const auto found = features_by_name.find(name);
        if (found != features_by_name.end())
        {
            return found->second;
        }
        const auto id = features.emplace(FeatureName{name}, config_type, graph, capabilities);
        auto& entry = *features.find(id);
        entry.id = id;
        features_by_name.emplace(entry.name, id);
        return id;
    }

    RenderResult<void> RenderServer::Impl::validateFeature(
        std::string_view name,
        cxx::TypeToken config_type,
        const FeatureGraphOps& graph,
        const CapabilitySupport* capabilities
    ) const noexcept
    {
        const bool is_invalid_declaration =
            name.empty() || (!graph.describe_ports && !graph.expand && graph.storage_inputs.empty());
        if (is_invalid_declaration)
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        for (auto dependency : graph.dependencies)
        {
            if (dependency.name() == name || !features_by_name.contains(dependency.name()))
            {
                return cxx::unexpected(RenderError{kFeatureMissingProvider});
            }
        }
        if (capabilities)
        {
            const bool invalid_support =
                capabilities->implementation != name || capabilities->implementation_version == 0 ||
                capabilities->capabilities.empty() || capabilities->configuration.cppType() != config_type;
            if (invalid_support)
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            for (const auto& capability : capabilities->capabilities)
            {
                const bool invalid_capability =
                    capability.name.empty() || capability.version == 0 ||
                    std::ranges::any_of(
                        capability.inputs,
                        [](const auto& input) noexcept
                        { return input.schema.empty() || input.version == 0 || !input.cppType().isValid(); }
                    );
                if (invalid_capability)
                {
                    return cxx::unexpected(RenderError{kFeatureContract});
                }
                for (const auto& previous : features)
                {
                    if (!previous.capabilities)
                    {
                        continue;
                    }
                    for (const auto& published : previous.capabilities->capabilities)
                    {
                        const bool same_contract =
                            published.name == capability.name && published.version == capability.version;
                        const bool mismatched_payload =
                            same_contract &&
                            (published.inputs != capability.inputs || published.outputs != capability.outputs ||
                             published.target_formats != capability.target_formats ||
                             published.target_samples != capability.target_samples ||
                             published.target_kinds != capability.target_kinds);
                        if (mismatched_payload)
                        {
                            return cxx::unexpected(RenderError{kFeatureResourceMismatch});
                        }
                    }
                }
            }
        }
        const auto found = features_by_name.find(name);
        if (found != features_by_name.end())
        {
            const auto& previous = *features.find(found->second);
            const bool is_incompatible = previous.config_type != config_type ||
                                         previous.graph.describe_ports != graph.describe_ports ||
                                         previous.graph.build != graph.build || previous.graph.expand != graph.expand ||
                                         previous.graph.parameters != graph.parameters ||
                                         previous.graph.decode_configuration != graph.decode_configuration ||
                                         previous.graph.effect_input_type != graph.effect_input_type ||
                                         !std::ranges::equal(previous.graph.dependencies, graph.dependencies) ||
                                         previous.capabilities != capabilities;
            if (is_incompatible)
            {
                return cxx::unexpected(RenderError{kFeatureDuplicate});
            }
        }
        return {};
    }

    RenderResult<void> RenderServer::Impl::validateRegistration(
        std::span<const StorageFactory> factories,
        std::span<const HandlerRecord> handlers,
        std::span<const CommandBinding> bindings
    ) const noexcept
    {
        for (std::size_t i = 0; i < factories.size(); ++i)
        {
            const auto& factory = factories[i];
            if (auto valid = resources->validateFactory(factory); !valid)
            {
                return valid;
            }
            for (std::size_t j = 0; j < i; ++j)
            {
                if (factories[j].descriptor->type != factory.descriptor->type)
                {
                    continue;
                }
                if (auto valid = compatibleStorage(*factories[j].descriptor, *factory.descriptor); !valid)
                {
                    return valid;
                }
                if (!sameFactory(factories[j], factory))
                {
                    return cxx::unexpected(RenderError{kStorageMismatch});
                }
            }
        }
        for (const auto& handler : handlers)
        {
            if (auto valid = command_registry_.validateHandler(handler, bindings); !valid)
            {
                return valid;
            }
        }
        return {};
    }

    void RenderServer::Impl::publishRegistration(
        std::span<const StorageFactory> factories,
        std::span<HandlerRecord> handlers,
        std::span<const CommandBinding> bindings
    ) noexcept
    {
        // Every recoverable boundary precedes this point. Publishing only allocates ordinary
        // host memory; no Handler/Storage constructor or user callback can partially commit.
        command_registry_.reserve(bindings.size(), handlers.size());
        for (const auto& factory : factories)
        {
            resources->publishFactory(factory);
        }
        for (auto& handler : handlers)
        {
            const auto type = handler.type;
            const auto id = command_registry_.publishHandler(std::move(handler));
            for (const auto& binding : bindings)
            {
                if (binding.handlerType() == type)
                {
                    command_registry_.publishBinding(id, binding);
                }
            }
        }
    }

    RenderResult<FeatureId> RenderServer::registerFeature(
        std::string_view name,
        cxx::TypeToken config_type,
        const FeatureGraphOps& graph,
        const CapabilitySupport* capabilities,
        std::span<const StorageFactory> factories,
        std::span<HandlerRecord> handlers,
        std::span<const CommandBinding> bindings
    ) noexcept
    {
        auto& state = *impl_;
        if (state.stop_requested.load())
        {
            return cxx::unexpected(RenderError{kRuntimeState});
        }
        if (auto valid = state.validateFeature(name, config_type, graph, capabilities); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        if (auto valid = state.validateRegistration(factories, handlers, bindings); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        for (const auto& input : graph.storage_inputs)
        {
            const bool invalid_address = input.schema.empty() || input.version == 0 || input.storage_key.empty() ||
                                         input.scope > ECapabilityInputScope::ATTACHMENT;
            const bool known_factory =
                state.resources->factory(input.storage_type) ||
                std::ranges::any_of(
                    factories,
                    [&](const auto& factory) noexcept { return factory.descriptor->type == input.storage_type; }
                );
            if (invalid_address || !known_factory)
            {
                return cxx::unexpected(RenderError{invalid_address ? kFeatureContract : kStorageFactory});
            }
        }
        state.publishRegistration(factories, handlers, bindings);
        const auto id = state.publishFeature(name, config_type, graph, capabilities);
        // A running Server may be extended only at its control safe point. Finish activation
        // before dispatch resumes; the registration templates themselves invoke no constructors.
        if (state.running.load())
        {
            state.command_registry_.activateSelectedHandlers(*state.resources);
        }
        return id;
    }

    RenderResult<StorageTypeId> RenderServer::registerStorage(
        StorageFactory factory,
        ConfigValue configuration
    ) noexcept
    {
        if (impl_->stop_requested.load())
        {
            return cxx::unexpected(RenderError{kRuntimeState});
        }
        return impl_->resources->installFactory(std::move(factory), std::move(configuration));
    }

    RenderResult<HandlerId> RenderServer::registerHandler(
        HandlerRecord handler,
        std::span<const CommandBinding> bindings
    ) noexcept
    {
        auto& state = *impl_;
        if (state.stop_requested.load())
        {
            return cxx::unexpected(RenderError{kRuntimeState});
        }
        if (auto valid = state.command_registry_.validateHandler(handler, bindings); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        const auto id = state.command_registry_.publishHandler(std::move(handler));
        for (const auto& binding : bindings)
        {
            state.command_registry_.publishBinding(id, binding);
        }
        if (state.running.load())
        {
            state.command_registry_.activateSelectedHandlers(*state.resources);
        }
        return id;
    }

    bool RenderServer::pagedInstances() const noexcept
    {
        return impl_->paged_instances;
    }

    RenderResult<FeatureId> RenderServer::featureId(std::string_view implementation) const noexcept
    {
        const auto found = impl_->features_by_name.find(implementation);
        if (found == impl_->features_by_name.end())
        {
            return cxx::unexpected(RenderError{kRuntimeUnsupported});
        }
        return found->second;
    }

    RenderResult<CommandId> RenderServer::findCommand(const CommandDescriptor& descriptor) const noexcept
    {
        return impl_->command_registry_.findCommand(descriptor);
    }

    RenderResult<void> RenderServer::run() noexcept
    {
        return impl_->run();
    }

    void RenderServer::stop() noexcept
    {
        impl_->stop();
    }

    RenderServerStats RenderServer::stats() const noexcept
    {
        return impl_->stats();
    }
} // namespace lux::render

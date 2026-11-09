#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/capabilities/BuiltinImplementations.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/extensions/Registration.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>
#include <optional>
#include <set>

namespace lux::render
{
    namespace
    {
        bool sameFactory(const StorageFactory& left, const StorageFactory& right) noexcept
        {
            return  left.implementation_type == right.implementation_type && 
                    left.config_type == right.config_type &&
                    left.ops_type == right.ops_type && 
                    left.create == right.create && 
                    left.ops == right.ops &&
                    left.binding_ops == right.binding_ops && 
                    left.compatible_config == right.compatible_config;
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
        auto state = std::make_unique<RenderServer::Impl>(std::move(config), std::move(*responder), execution.cpuScheduler());
        if (auto initialized = state->initialize(device_options); !initialized)
        {
            return cxx::unexpected(initialized.error());
        }
        if (auto registered = state->registerBuiltinFeatures(); !registered)
        {
            return cxx::unexpected(registered.error());
        }
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

    RenderResult<void> RenderServer::Impl::registerBuiltinFeatures() noexcept
    {
        constexpr auto implementation_count = std::tuple_size_v<detail::BuiltinImplementations>;
        if (auto prepared = capability_registry_.prepare(implementation_count); !prepared)
        {
            return prepared;
        }
        // Declarations describe the public contract even when this device cannot implement it.
        // Register them before filtering implementations so prerequisite lookup remains complete.
        RenderResult<void> declarations;
        cxx::tuple_traits::for_each_type<detail::BuiltinImplementations>(
            [&]<class Implementation, std::size_t>() noexcept
            {
                if (!declarations)
                {
                    return;
                }
                auto added = capability_registry_.add(describeCapability<typename Implementation::Capability>());
                if (!added)
                {
                    declarations = cxx::unexpected(added.error());
                }
            }
        );
        if (!declarations)
        {
            return declarations;
        }
        // Built-in declarations are trusted. Device support decides factory availability;
        // no unsupported Storage or Handler is installed, and no rollback owner is needed.
        const auto register_dependencies =
            [&]<class Declaration>() noexcept -> std::optional<std::vector<CapabilityCommandBinding>>
        {
            using StorageTypes = decltype(uniqueTypes(static_cast<typename Declaration::Storages*>(nullptr)));
            const auto required = []<class... Types>(std::tuple<Types...>*) noexcept
            {
                return (0u | ... | Types::type::Descriptor::descriptor.required_capabilities);
            }(static_cast<StorageTypes*>(nullptr));
            if ((required & ~resources->capabilities()) != 0)
            {
                return std::nullopt;
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
            using Methods = decltype(std::tuple_cat(
                std::declval<typename Declaration::Bindings>(),
                std::declval<typename Declaration::FrameBindings>()
            ));
            using HandlerTypes = decltype(handlerTypes(static_cast<Methods*>(nullptr)));
            cxx::tuple_traits::for_each_type<HandlerTypes>(
                [&]<class Type, std::size_t>() noexcept
                {
                    using Handler = typename Type::type;
                    if (command_registry_.handlerId(cxx::make_basic_type_info<Handler>(), {}).isNull())
                    {
                        command_registry_.publishHandler(handlerRecord<Handler>({}));
                    }
                }
            );
            const auto methods = commandBindings(static_cast<typename Declaration::Bindings*>(nullptr));
            std::vector<CapabilityCommandBinding> locations;
            locations.reserve(methods.size());
            for (const auto& method : methods)
            {
                locations.push_back(
                    {command_registry_.publishBinding(command_registry_.handlerId(method.handlerType(), {}), method),
                     method.storageDependency()}
                );
            }
            return locations;
        };
        cxx::tuple_traits::for_each_type<detail::BuiltinFeatures>(
            [&]<class Feature, std::size_t>() noexcept
            {
                publishFeature(
                    FeatureEntry{Feature::name, configurationType<Feature>(), Feature::graph(paged_instances)}
                );
            }
        );
        cxx::tuple_traits::for_each_type<detail::BuiltinImplementations>(
            [&]<class Implementation, std::size_t>() noexcept
            {
                auto command_bindings = register_dependencies.template operator()<Implementation>();
                if (!command_bindings)
                {
                    return;
                }
                std::vector<FeatureId> required_features;
                cxx::tuple_traits::for_each_type<typename Implementation::Features>(
                    [&]<class Feature, std::size_t>() noexcept
                    { required_features.push_back(features_by_name.find(Feature::name.name())->second); }
                );
                auto value = makeImplementation<Implementation>();
                value.features = std::move(required_features);
                value.command_bindings = std::move(*command_bindings);
                for (auto& call : value.frame_calls)
                {
                    call.handler = command_registry_.findHandler(command_registry_.handlerId(call.handler_type, {}));
                }
                capability_registry_.publish(
                    describeCapability<typename Implementation::Capability>(),
                    std::move(value)
                );
            }
        );
        return capability_registry_.validateDeclarations();
    }

    FeatureId RenderServer::Impl::publishFeature(FeatureEntry entry) noexcept
    {
        const auto found = features_by_name.find(entry.name);
        if (found != features_by_name.end())
        {
            return found->second;
        }
        const auto id = features.emplace(std::move(entry));
        auto& stored = *features.find(id);
        stored.id = id;
        features_by_name.emplace(stored.name, id);
        return id;
    }

    RenderResult<void> RenderServer::Impl::validateImplementationDescription(
        const CapabilityDescription& capability,
        const CapabilityImplementation& implementation
    ) const noexcept
    {
        const bool is_invalid_identity = implementation.implementation.empty();
        const bool is_invalid_configuration =
            !implementation.configuration.schema.empty() && implementation.configuration.version == 0;
        if (is_invalid_identity || is_invalid_configuration)
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        if (auto valid = capability_registry_.validate(capability); !valid)
        {
            return valid;
        }
        for (const auto prerequisite : capability.prerequisites)
        {
            if (capability_registry_.find(prerequisite).isNull())
            {
                return cxx::unexpected(RenderError{kCapabilityPrerequisite});
            }
        }
        // These graph-interface declarations still feed the existing cold composition compiler.
        // Different implementations may legitimately have different graph/resource layouts;
        // they are not compared as if those layouts were the public capability's data contract.
        const auto& graph = implementation.graph;
        const bool is_effect = !graph.ports.empty();
        const bool has_output = std::ranges::any_of(
            graph.ports,
            [](const auto& port) noexcept { return port.direction == ECapabilityPortDirection::OUTPUT; }
        );
        const bool is_missing_output = is_effect && (!has_output || graph.target_formats.empty() ||
                                                     graph.target_samples.empty() || graph.target_kinds.empty());
        if (is_missing_output)
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        std::set<std::string_view> port_names;
        for (const auto& port : graph.ports)
        {
            const bool is_invalid_port =
                port.key.empty() || port.semantic.empty() || port.direction > ECapabilityPortDirection::OUTPUT;
            if (is_invalid_port || !port_names.emplace(port.key).second)
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
        }
        const auto required_frames = capability.per_frame_data;
        if (required_frames.size() != implementation.frame_calls.size())
        {
            return cxx::unexpected(RenderError{kCapabilityDeclaration});
        }
        for (const auto type : required_frames)
        {
            if (std::ranges::count(implementation.frame_calls, type, &FrameDataCall::data_type) != 1)
            {
                return cxx::unexpected(RenderError{kCapabilityDeclaration});
            }
        }
        for (const auto& dependency : implementation.storage_dependencies)
        {
            const bool is_invalid_scope = dependency.scope > EStorageScope::ATTACHMENT;
            const bool is_invalid_storage = dependency.storage_type.name().empty() || dependency.storage_key.empty();
            if (is_invalid_scope || is_invalid_storage)
            {
                return cxx::unexpected(RenderError{kStorageFactory});
            }
        }
        return {};
    }

    RenderResult<void> RenderServer::Impl::validateFeature(const FeatureEntry& entry) const noexcept
    {
        const bool is_invalid_graph = entry.name.empty() || !entry.graph.describe_ports || !entry.graph.build;
        if (is_invalid_graph)
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        const auto found = features_by_name.find(entry.name);
        if (found != features_by_name.end())
        {
            const auto& previous = *features.find(found->second);
            if (previous.config_type != entry.config_type || previous.graph != entry.graph)
            {
                return cxx::unexpected(RenderError{kFeatureDuplicate});
            }
        }
        return {};
    }

    RenderResult<void> RenderServer::Impl::validateRegistration(
        std::span<const StorageFactory> factories,
        std::span<const HandlerRecord> handlers,
        std::span<const CommandBinding> bindings,
        std::span<const FrameDataCall> frame_calls
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
            if (auto valid = command_registry_.validateHandler(handler, bindings, frame_calls); !valid)
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

    RenderResult<FeatureId> RenderServer::registerFeature(FeatureEntry entry) noexcept
    {
        auto& state = *impl_;
        if (state.started.load() && state.device)
        {
            return cxx::unexpected(RenderError{kCapabilityInUse});
        }
        if (auto valid = state.validateFeature(entry); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        return state.publishFeature(std::move(entry));
    }

    RenderResult<CapabilityImplementationId> RenderServer::registerCapabilityImplementation(
        CapabilityDescription capability,
        CapabilityImplementation implementation,
        std::span<FeatureEntry> graph_features,
        std::span<const StorageFactory> factories,
        std::span<HandlerRecord> handlers,
        std::span<const CommandBinding> bindings
    ) noexcept
    {
        auto& state = *impl_;
        if (state.started.load() && state.device)
        {
            return cxx::unexpected(RenderError{kCapabilityInUse});
        }
        for (std::size_t i = 0; i < graph_features.size(); ++i)
        {
            const auto& feature = graph_features[i];
            if (auto valid = state.validateFeature(feature); !valid)
            {
                return cxx::unexpected(valid.error());
            }
            for (std::size_t previous = 0; previous < i; ++previous)
            {
                if (graph_features[previous].name == feature.name)
                {
                    return cxx::unexpected(RenderError{kFeatureDuplicate});
                }
            }
        }
        if (auto valid = state.validateRegistration(factories, handlers, bindings, implementation.frame_calls); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        if (auto valid = state.validateImplementationDescription(capability, implementation); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        const bool needs_graph = !implementation.graph.ports.empty();
        const bool is_invalid_direct_graph =
            needs_graph && !implementation.build_graph &&
            (graph_features.size() != 1 || graph_features.front().config_type != implementation.config_type);
        const bool is_invalid_features = needs_graph && graph_features.empty();
        if (is_invalid_features || is_invalid_direct_graph)
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        if (bindings.size() != capability.commands.size())
        {
            return cxx::unexpected(RenderError{kCapabilityDeclaration});
        }
        for (const auto* command : capability.commands)
        {
            const auto matches = std::ranges::count_if(
                bindings,
                [&](const auto& binding) noexcept
                { return compatibleCommands(*command, *binding.descriptor()).has_value(); }
            );
            if (matches != 1)
            {
                return cxx::unexpected(RenderError{kCapabilityDeclaration});
            }
        }
        for (const auto& binding : bindings)
        {
            const auto dependency = binding.storageDependency();
            if (dependency != UINT32_MAX && dependency >= implementation.storage_dependencies.size())
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            if (binding.storageType().isValid())
            {
                if (dependency == UINT32_MAX)
                {
                    return cxx::unexpected(RenderError{kStorageContract});
                }
                const auto type = implementation.storage_dependencies[dependency].storage_type;
                const auto* factory = state.resources->factory(type);
                if (!factory)
                {
                    const auto found = std::ranges::find_if(
                        factories,
                        [&](const auto& entry) noexcept { return entry.descriptor->type == type; }
                    );
                    factory = found == factories.end() ? nullptr : &*found;
                }
                if (!factory || factory->implementation_type != binding.storageType())
                {
                    return cxx::unexpected(RenderError{kStorageMismatch});
                }
            }
        }
        for (const auto& call : implementation.frame_calls)
        {
            const bool is_invalid_storage = call.target == EFrameDataTarget::STORAGE &&
                                            call.storage_dependency >= implementation.storage_dependencies.size();
            const bool is_invalid_draw =
                call.target == EFrameDataTarget::DRAW_LIST &&
                (call.graph_feature >= graph_features.size() || call.contribution_key.empty() || call.pass_key.empty());
            if (is_invalid_storage || is_invalid_draw)
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
        }
        for (const auto& input : implementation.storage_dependencies)
        {
            const bool has_factory =
                state.resources->factory(input.storage_type) ||
                std::ranges::any_of(
                    factories,
                    [&](const auto& factory) noexcept { return factory.descriptor->type == input.storage_type; }
                );
            if (!has_factory)
            {
                return cxx::unexpected(RenderError{kStorageFactory});
            }
        }
        implementation.command_bindings.clear();
        implementation.command_bindings.reserve(bindings.size());
        for (const auto& method : bindings)
        {
            const auto handler = state.command_registry_.handlerId(method.handlerType(), {});
            const auto location = state.command_registry_.findBinding(*method.descriptor(), handler);
            implementation.command_bindings.push_back(
                {location ? *location : CommandBindingLocation{}, method.storageDependency()}
            );
        }
        // Resolve candidate identities without publishing. Missing Features remain null until
        // publication; a previously registered complete implementation cannot match that candidate.
        implementation.features.clear();
        implementation.features.reserve(graph_features.size());
        for (const auto& feature : graph_features)
        {
            const auto found = state.features_by_name.find(feature.name);
            implementation.features.push_back(found == state.features_by_name.end() ? FeatureId{} : found->second);
        }
        for (const auto id : state.capability_registry_.implementations())
        {
            const auto& previous = *state.capability_registry_.find(id);
            const bool is_same_identity =
                previous.implementation == implementation.implementation &&
                state.capability_registry_.capabilityOf(id) == state.capability_registry_.find(capability.type);
            if (!is_same_identity)
            {
                continue;
            }
            if (previous != implementation)
            {
                return cxx::unexpected(RenderError{kCapabilityConflict});
            }
            return id;
        }
        if (auto prepared = state.capability_registry_.prepare(1); !prepared)
        {
            return cxx::unexpected(prepared.error());
        }
        // Publication has no fallible device work or user construction. Ordinary allocation OOM is fatal.
        state.publishRegistration(factories, handlers, bindings);
        for (std::size_t i = 0; i < bindings.size(); ++i)
        {
            const auto& method = bindings[i];
            const auto handler = state.command_registry_.handlerId(method.handlerType(), {});
            implementation.command_bindings[i].location =
                *state.command_registry_.findBinding(*method.descriptor(), handler);
        }
        for (auto& call : implementation.frame_calls)
        {
            call.handler =
                state.command_registry_.findHandler(state.command_registry_.handlerId(call.handler_type, {}));
        }
        implementation.features.clear();
        implementation.features.reserve(graph_features.size());
        for (auto& feature : graph_features)
        {
            implementation.features.push_back(state.publishFeature(std::move(feature)));
        }
        return state.capability_registry_.publish(capability, std::move(implementation));
    }

    const CapabilityRegistry& RenderServer::capabilities() const noexcept
    {
        return impl_->capability_registry_;
    }

    RenderResult<CapabilityId> RenderServer::registerCapability(const CapabilityDescription& description) noexcept
    {
        if (impl_->started.load() && impl_->device)
        {
            return cxx::unexpected(RenderError{kCapabilityInUse});
        }
        return impl_->capability_registry_.add(description);
    }

    RenderResult<void> RenderServer::Impl::validateCapabilityImplementation(
        const CapabilityDescription& capability,
        const CapabilityImplementation& implementation
    ) const noexcept
    {
        if (auto valid = validateImplementationDescription(capability, implementation); !valid)
        {
            return valid;
        }
        const auto commands = capability.commands;
        if (implementation.command_bindings.size() != commands.size())
        {
            return cxx::unexpected(RenderError{kCapabilityDeclaration});
        }
        for (const auto& binding : implementation.command_bindings)
        {
            const bool is_invalid_dependency = binding.storage_dependency != UINT32_MAX &&
                                               binding.storage_dependency >= implementation.storage_dependencies.size();
            if (is_invalid_dependency)
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            const auto* method = command_registry_.binding(binding.location);
            if (!method || method->handler.isNull() || !method->call.invoke)
            {
                return cxx::unexpected(RenderError{kCommandContract});
            }
            if (method->storage_type.isValid())
            {
                if (binding.storage_dependency == UINT32_MAX)
                {
                    return cxx::unexpected(RenderError{kStorageContract});
                }
                const auto& dependency = implementation.storage_dependencies[binding.storage_dependency];
                const auto* factory = resources->factory(dependency.storage_type);
                if (!factory || factory->implementation_type != method->storage_type)
                {
                    return cxx::unexpected(RenderError{kStorageMismatch});
                }
            }
        }
        for (const auto* command : commands)
        {
            const auto id = command_registry_.findCommand(*command);
            const bool has_exact_call =
                id && std::ranges::count_if(
                          implementation.command_bindings,
                          [&](const auto& binding) noexcept { return binding.location.command == *id; }
                      ) == 1;
            if (!has_exact_call)
            {
                return cxx::unexpected(RenderError{kCapabilityDeclaration});
            }
        }
        for (const auto& call : implementation.frame_calls)
        {
            const auto* registered = command_registry_.findHandler(command_registry_.handlerId(call.handler_type, {}));
            const bool invalid_handler = !registered || registered != call.handler || !call.invoke;
            const bool invalid_alignment =
                call.root_alignment == 0 || (call.root_alignment & (call.root_alignment - 1)) != 0;
            const bool invalid_root = call.root_size == 0 || invalid_alignment || !call.method_type.isValid();
            const bool invalid_storage =
                call.target == EFrameDataTarget::STORAGE &&
                (!call.resolve_storage || call.storage_dependency >= implementation.storage_dependencies.size());
            const bool invalid_draw =
                call.target == EFrameDataTarget::DRAW_LIST && (call.graph_feature >= implementation.features.size() ||
                                                               call.contribution_key.empty() || call.pass_key.empty());
            if (invalid_handler || invalid_root || invalid_storage || invalid_draw ||
                call.target > EFrameDataTarget::DRAW_LIST)
            {
                return cxx::unexpected(RenderError{kCapabilityDeclaration});
            }
        }
        // Extension authors provide resolved Features, not a synthetic root or another name graph.
        std::set<std::pair<std::uint32_t, std::uint32_t>> unique_features;
        for (const auto feature : implementation.features)
        {
            const auto* entry = features.find(feature);
            if (!entry || !unique_features.emplace(feature.index, feature.gen).second)
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
        }
        if (implementation.build_graph && implementation.features.empty())
        {
            return cxx::unexpected(RenderError{kFeatureMissingProvider});
        }
        if (!implementation.build_graph && !implementation.graph.ports.empty())
        {
            const bool is_single_feature = implementation.features.size() == 1;
            const auto* entry = is_single_feature ? features.find(implementation.features.front()) : nullptr;
            if (!entry || !entry->graph.describe_ports || entry->config_type != implementation.config_type)
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
        }
        return {};
    }

    RenderResult<CapabilityImplementationId> RenderServer::addCapabilityImplementation(
        CapabilityId capability,
        const CapabilityImplementation& declaration
    ) noexcept
    {
        auto& state = *impl_;
        if (state.started.load() && state.device)
        {
            return cxx::unexpected(RenderError{kCapabilityInUse});
        }
        const auto* description = state.capability_registry_.get(capability);
        if (!description)
        {
            return cxx::unexpected(RenderError{kRuntimeIdentity});
        }
        if (auto valid = state.validateCapabilityImplementation(*description, declaration); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        return state.capability_registry_.addImplementation(capability, declaration);
    }

    RenderResult<CapabilityImplementationId> RenderServer::replaceCapabilityImplementation(
        CapabilityImplementationId previous,
        const CapabilityImplementation& declaration
    ) noexcept
    {
        auto& state = *impl_;
        if (state.started.load() && state.device)
        {
            return cxx::unexpected(RenderError{kCapabilityInUse});
        }
        if (!state.capability_registry_.find(previous))
        {
            return cxx::unexpected(RenderError{kRuntimeIdentity});
        }
        const auto capability = state.capability_registry_.capabilityOf(previous);
        const auto& description = *state.capability_registry_.get(capability);
        if (auto valid = state.validateCapabilityImplementation(description, declaration); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        return state.capability_registry_.replaceImplementation(previous, declaration);
    }

    RenderResult<void> RenderServer::removeCapabilityImplementation(CapabilityImplementationId id) noexcept
    {
        if (impl_->started.load() && impl_->device)
        {
            return cxx::unexpected(RenderError{kCapabilityInUse});
        }
        return impl_->capability_registry_.removeImplementation(id);
    }

    RenderResult<void> RenderServer::removeCapability(CapabilityId id) noexcept
    {
        if (impl_->started.load() && impl_->device)
        {
            return cxx::unexpected(RenderError{kCapabilityInUse});
        }
        return impl_->capability_registry_.remove(id);
    }

    RenderResult<StorageTypeId>
    RenderServer::registerStorage(StorageFactory factory, ConfigValue configuration) noexcept
    {
        if (impl_->stop_requested.load())
        {
            return cxx::unexpected(RenderError{kRuntimeState});
        }
        return impl_->resources->installFactory(std::move(factory), std::move(configuration));
    }

    RenderResult<HandlerId>
    RenderServer::registerHandler(HandlerRecord handler, std::span<const CommandBinding> bindings) noexcept
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
            state.command_registry_.activateHandlers(*state.resources);
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

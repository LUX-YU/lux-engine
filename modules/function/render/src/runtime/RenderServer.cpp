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
        bool sameFactory(const ResourceRegistration& left, const ResourceRegistration& right) noexcept
        {
            return *left.storage == *right.storage && std::ranges::equal(left.commands, right.commands,
                [](const auto& a, const auto& b) noexcept { return a.hasSameImplementation(b); }) &&
                left.frame_call == right.frame_call;
        }

    } // namespace

    RenderServer::RenderServer(std::unique_ptr<Impl> state) noexcept : impl_(std::move(state)) {}

    RenderServer::~RenderServer() noexcept
    {
        if (impl_->running_.load())
        {
            std::terminate(); // the application must stop and join its rendering thread
        }
        if (impl_->device_)
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
        const bool invalid =
            config.frames_in_flight == 0 ||
            config.frames_in_flight > 3 ||
            config.max_frames_ahead == UINT32_MAX ||
            config.max_config_bytes == 0 ||
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
        if (!impl_->canAccessCatalog()) return cxx::unexpected(RenderError{kCapabilityInUse});
        if (impl_->has_started_.load() || requester.isNull())
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
        // Built-in and external authors must satisfy the same complete Resource/Command contract.
        // Qualify before publishing; metadata registration does not construct a Resource or Handler.
        RenderResult<void> qualified;
        cxx::tuple_traits::for_each_type<detail::BuiltinImplementations>(
            [&]<class Implementation, std::size_t>() noexcept
            {
                if (!qualified) return;
                const auto factories = storageFactories(
                    uniqueTypes(static_cast<typename Implementation::Storages*>(nullptr))
                );
                const auto handlers = handlerRecords(
                    handlerTypes(static_cast<typename Implementation::Bindings*>(nullptr))
                );
                const auto methods = commandBindings(static_cast<typename Implementation::Bindings*>(nullptr));
                qualified = validateRegistration(factories, handlers, methods);
            }
        );
        if (!qualified) return qualified;

        // Register independently of the first device. Resource creation checks its actual DeviceSession.
        const auto register_dependencies =
            [&]<class Declaration>() noexcept -> std::vector<CapabilityCommandBinding>
        {
            using StorageTypes = decltype(uniqueTypes(static_cast<typename Declaration::Storages*>(nullptr)));
            cxx::tuple_traits::for_each_type<StorageTypes>(
                [&]<class Type, std::size_t>() noexcept
                {
                    using Storage = typename Type::type;
                    if (resources_->typeId(Storage::Descriptor::descriptor.type).isNull())
                    {
                        const auto factory = Storage::factory();
                        const auto type = resources_->publishFactory(factory);
                        for (const auto& command : factory.commands)
                        {
                            const auto location = command_registry_.publishResourceBinding(command);
                            if (!resources_->bindCommand(type, location)) std::terminate();
                        }
                    }
                }
            );
            using HandlerTypes = decltype(handlerTypes(static_cast<typename Declaration::Bindings*>(nullptr)));
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
                    {command_registry_.publishBinding(command_registry_.handlerId(method.handlerType(), {}), method)}
                );
            }
            return locations;
        };
        cxx::tuple_traits::for_each_type<detail::BuiltinFeatures>(
            [&]<class Feature, std::size_t>() noexcept
            {
                publishFeature(
                    FeatureEntry{
                        Feature::name, configurationType<Feature>(),
                        runtime_detail::featureGraphOps<Feature>(paged_instances_)
                    }
                );
            }
        );
        cxx::tuple_traits::for_each_type<detail::BuiltinImplementations>(
            [&]<class Implementation, std::size_t>() noexcept
            {
                auto command_bindings = register_dependencies.template operator()<Implementation>();
                std::vector<FeatureId> required_features;
                cxx::tuple_traits::for_each_type<typename Implementation::Features>(
                    [&]<class Feature, std::size_t>() noexcept
                    { required_features.push_back(features_by_name_.find(Feature::name.name())->second); }
                );
                auto value = makeImplementation<Implementation>();
                value.features = std::move(required_features);
                value.command_bindings = std::move(command_bindings);

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
        const auto found = features_by_name_.find(entry.name);
        if (found != features_by_name_.end())
        {
            return found->second;
        }
        const auto id = features_.emplace(std::move(entry));
        auto& stored = *features_.find(id);
        stored.id = id;
        features_by_name_.emplace(stored.name, id);
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
        return {};
    }

    RenderResult<void> RenderServer::Impl::validateFeature(const FeatureEntry& entry) const noexcept
    {
        const bool is_invalid_graph = entry.name.empty() || !entry.graph.describe_ports || !entry.graph.build;
        if (is_invalid_graph)
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        const auto found = features_by_name_.find(entry.name);
        if (found != features_by_name_.end())
        {
            const auto& previous = *features_.find(found->second);
            if (previous.config_type != entry.config_type || previous.graph != entry.graph)
            {
                return cxx::unexpected(RenderError{kFeatureDuplicate});
            }
        }
        return {};
    }

    RenderResult<void> RenderServer::Impl::validateRegistration(
        std::span<const ResourceRegistration> factories,
        std::span<const HandlerRecord> handlers,
        std::span<const CommandBinding> bindings
    ) const noexcept
    {
        for (std::size_t i = 0; i < factories.size(); ++i)
        {
            const auto& factory = factories[i];
            if (auto valid = resources_->validateFactory(factory); !valid)
            {
                return valid;
            }
            if (auto valid = command_registry_.validateResourceBindings(factory.commands); !valid) return valid;
            const bool needs_creation_policy = factory.storage->config_type.isValid() &&
                resources_->typeId(factory.descriptor->type).isNull();
            if (needs_creation_policy) return cxx::unexpected(RenderError{kStorageContract});
            for (std::size_t j = 0; j < i; ++j)
            {
                for (const auto& left : factories[j].commands)
                {
                    for (const auto& right : factory.commands)
                    {
                        if (left.descriptor()->name != right.descriptor()->name) continue;
                        if (auto valid = compatibleCommands(*left.descriptor(), *right.descriptor()); !valid) return valid;
                    }
                }
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
        if (auto valid = command_registry_.validateStaticBindings(bindings, true); !valid) return valid;
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
        std::span<const ResourceRegistration> factories,
        std::span<HandlerRecord> handlers,
        std::span<const CommandBinding> bindings
    ) noexcept
    {
        // Every recoverable boundary precedes this point. Publishing only allocates ordinary
        // host memory; no Handler/Storage constructor or user callback can partially commit.
        command_registry_.reserve(bindings.size(), handlers.size());
        for (const auto& factory : factories)
        {
            const auto type = resources_->publishFactory(factory);
            for (const auto& command : factory.commands)
            {
                const auto location = command_registry_.publishResourceBinding(command);
                if (!resources_->bindCommand(type, location)) std::terminate();
            }
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
        for (const auto& binding : bindings)
        {
            if (!binding.handlerType().isValid()) command_registry_.publishBinding({}, binding);
        }
    }

    RenderResult<FeatureId> RenderServer::registerFeature(FeatureEntry entry) noexcept
    {
        if (!impl_->canAccessCatalog()) return cxx::unexpected(RenderError{kCapabilityInUse});
        auto& state = *impl_;
        if (state.has_started_.load() && state.device_)
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
        std::span<const ResourceRegistration> factories,
        std::span<HandlerRecord> handlers,
        std::span<const CommandBinding> bindings
    ) noexcept
    {
        if (!impl_->canAccessCatalog()) return cxx::unexpected(RenderError{kCapabilityInUse});
        auto& state = *impl_;
        if (state.has_started_.load() && state.device_)
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
        if (auto valid = state.validateRegistration(factories, handlers, bindings); !valid)
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
        for (const auto* command : capability.commands)
        {
            const auto matches = std::ranges::count_if(bindings, [&](const auto& binding) noexcept
                { return compatibleCommands(*command, *binding.descriptor()).has_value(); });
            if (matches != 1)
                return cxx::unexpected(RenderError{kCapabilityDeclaration});
        }
        for (const auto& binding : bindings)
            if (binding.isResourceCommand()) return cxx::unexpected(RenderError{kCommandContract});
        for (const auto& call : implementation.draw_inputs)
        {
            const bool is_invalid_draw = call.graph_feature >= graph_features.size() ||
                call.contribution_key.empty() || call.pass_key.empty();
            if (is_invalid_draw) return cxx::unexpected(RenderError{kFeatureContract});
        }
        for (const auto* resource : capability.resources)
        {
            const auto* factory = state.resources_->factory(resource->type);
            if (!factory)
            {
                const auto found = std::ranges::find_if(factories, [&](const auto& candidate) noexcept
                    { return candidate.descriptor->type == resource->type; });
                if (found != factories.end()) factory = &*found;
            }
            if (!factory) return cxx::unexpected(RenderError{kStorageFactory});
            if (auto valid = compatibleStorage(*resource, *factory->descriptor); !valid)
                return cxx::unexpected(valid.error());
        }
        implementation.command_bindings.clear();
        implementation.command_bindings.reserve(bindings.size());
        for (const auto& method : bindings)
        {
            const auto handler = state.command_registry_.handlerId(method.handlerType(), {});
            const auto location = state.command_registry_.findBinding(method, handler);
            implementation.command_bindings.push_back(
                {location ? *location : CommandBindingLocation{}}
            );
        }
        // Resolve candidate identities without publishing. Missing Features remain null until
        // publication; a previously registered complete implementation cannot match that candidate.
        implementation.features.clear();
        implementation.features.reserve(graph_features.size());
        for (const auto& feature : graph_features)
        {
            const auto found = state.features_by_name_.find(feature.name);
            implementation.features.push_back(found == state.features_by_name_.end() ? FeatureId{} : found->second);
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
                *state.command_registry_.findBinding(method, handler);

        }

        implementation.features.clear();
        implementation.features.reserve(graph_features.size());
        for (auto& feature : graph_features)
        {
            implementation.features.push_back(state.publishFeature(std::move(feature)));
        }
        return state.capability_registry_.publish(capability, std::move(implementation));
    }

    RenderResult<CapabilityId> RenderServer::registerCapability(const CapabilityDescription& description) noexcept
    {
        if (!impl_->canAccessCatalog()) return cxx::unexpected(RenderError{kCapabilityInUse});
        if (impl_->has_started_.load() && impl_->device_)
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
        for (const auto& binding : implementation.command_bindings)
        {
            const auto* method = command_registry_.binding(binding.location);
            const bool invalid_call = !method || !method->call.invoke;
            const bool is_resource_route = method && method->storage_type.isValid();
            if (invalid_call || is_resource_route) return cxx::unexpected(RenderError{kCommandContract});
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

        for (const auto& input : implementation.draw_inputs)
        {
            const bool is_invalid_draw = input.graph_feature >= implementation.features.size() ||
                input.contribution_key.empty() || input.pass_key.empty();
            if (is_invalid_draw) return cxx::unexpected(RenderError{kFeatureContract});
        }
        // Extension authors provide resolved Features, not a synthetic root or another name graph.
        std::set<std::pair<std::uint32_t, std::uint32_t>> unique_features;
        for (const auto feature : implementation.features)
        {
            const auto* entry = features_.find(feature);
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
            const auto* entry = is_single_feature ? features_.find(implementation.features.front()) : nullptr;
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
        if (!impl_->canAccessCatalog()) return cxx::unexpected(RenderError{kCapabilityInUse});
        auto& state = *impl_;
        if (state.has_started_.load() && state.device_)
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
        if (!impl_->canAccessCatalog()) return cxx::unexpected(RenderError{kCapabilityInUse});
        auto& state = *impl_;
        if (state.has_started_.load() && state.device_)
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
        if (!impl_->canAccessCatalog()) return cxx::unexpected(RenderError{kCapabilityInUse});
        if (impl_->has_started_.load() && impl_->device_)
        {
            return cxx::unexpected(RenderError{kCapabilityInUse});
        }
        return impl_->capability_registry_.removeImplementation(id);
    }

    RenderResult<void> RenderServer::removeCapability(CapabilityId id) noexcept
    {
        if (!impl_->canAccessCatalog()) return cxx::unexpected(RenderError{kCapabilityInUse});
        if (impl_->has_started_.load() && impl_->device_)
        {
            return cxx::unexpected(RenderError{kCapabilityInUse});
        }
        return impl_->capability_registry_.remove(id);
    }

    RenderResult<ResourceTypeId>
    RenderServer::registerResource(ResourceRegistration factory, ConfigValue configuration) noexcept
    {
        if (!impl_->canAccessCatalog()) return cxx::unexpected(RenderError{kCapabilityInUse});
        if (impl_->stop_requested_.load())
        {
            return cxx::unexpected(RenderError{kRuntimeState});
        }
        if (impl_->has_started_.load()) return cxx::unexpected(RenderError{kCapabilityInUse});
        if (auto valid = impl_->resources_->validateFactory(factory, configuration); !valid)
            return cxx::unexpected(valid.error());
        if (auto valid = impl_->command_registry_.validateResourceBindings(factory.commands); !valid)
            return cxx::unexpected(valid.error());
        const auto type = impl_->resources_->publishFactory(factory, std::move(configuration));
        for (const auto& command : factory.commands)
        {
            const auto location = impl_->command_registry_.publishResourceBinding(command);
            if (!impl_->resources_->bindCommand(type, location)) std::terminate();
        }
        return type;
    }

    RenderResult<HandlerId>
    RenderServer::registerHandler(HandlerRecord handler, std::span<const CommandBinding> bindings) noexcept
    {
        if (!impl_->canAccessCatalog()) return cxx::unexpected(RenderError{kCapabilityInUse});
        auto& state = *impl_;
        if (state.stop_requested_.load())
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
        if (state.running_.load())
        {
            state.command_registry_.activateHandlers(*state.resources_);
        }
        return id;
    }

    RenderResult<void> RenderServer::registerCommands(std::span<const CommandBinding> bindings) noexcept
    {
        if (!impl_->canAccessCatalog()) return cxx::unexpected(RenderError{kCapabilityInUse});
        if (impl_->stop_requested_.load()) return cxx::unexpected(RenderError{kRuntimeState});
        if (auto valid = impl_->command_registry_.validateStaticBindings(bindings); !valid) return valid;
        impl_->command_registry_.reserve(bindings.size(), 0);
        impl_->command_registry_.publishStaticBindings(bindings);
        return {};
    }

    bool RenderServer::pagedInstances() const noexcept
    {
        return impl_->paged_instances_;
    }

    RenderResult<FeatureId> RenderServer::featureId(std::string_view implementation) const noexcept
    {
        if (!impl_->canAccessCatalog()) return cxx::unexpected(RenderError{kCapabilityInUse});
        const auto found = impl_->features_by_name_.find(implementation);
        if (found == impl_->features_by_name_.end())
        {
            return cxx::unexpected(RenderError{kRuntimeUnsupported});
        }
        return found->second;
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

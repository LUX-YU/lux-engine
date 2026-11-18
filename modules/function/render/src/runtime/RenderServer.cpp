#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/capabilities/BuiltinImplementations.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/extensions/Registration.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>
#include <optional>
#include <set>

namespace lux::render
{
    RenderServer::RenderServer(std::unique_ptr<Impl> state) noexcept : impl_(std::move(state)) {}

    RenderServer::~RenderServer() noexcept
    {
        if (impl_->state_.load() == Impl::EState::RUNNING)
        {
            std::terminate(); // the application must stop and join its rendering thread
        }
        if (impl_->canAccessCatalog())
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

        auto server = std::unique_ptr<RenderServer>(new RenderServer(std::move(state)));
        if (auto registered = server->registerBuiltins(); !registered)
        {
            return cxx::unexpected(registered.error());
        }

        return server;
    }

    RenderResult<void> RenderServer::registerBuiltins() noexcept
    {
        // Deduplicate the module closure once; every definition uses the public registration entry.
        using Storages = decltype([]<class... Implementations>(std::tuple<Implementations...>*)
        {
            return std::tuple_cat(uniqueTypes(static_cast<typename Implementations::Storages*>(nullptr))...);
        }(static_cast<detail::BuiltinImplementations*>(nullptr)));
        using UniqueStorages = decltype([]<class... Types>(std::tuple<Types...>*)
        {
            return uniqueTypes(static_cast<std::tuple<typename Types::type...>*>(nullptr));
        }(static_cast<Storages*>(nullptr)));
        RenderResult<void> result;
        cxx::tuple_traits::for_each_type<UniqueStorages>([&]<class Type, std::size_t>() noexcept
        {
            if (!result) return;
            auto added = registerResource<typename Type::type>();
            if (!added) result = cxx::unexpected(added.error());
        });
        cxx::tuple_traits::for_each_type<detail::BuiltinFeatures>([&]<class Feature, std::size_t>() noexcept
        {
            if (!result) return;
            auto added = registerFeature<Feature>();
            if (!added) result = cxx::unexpected(added.error());
        });
        cxx::tuple_traits::for_each_type<detail::BuiltinImplementations>(
            [&]<class Implementation, std::size_t>() noexcept
            {
                if (!result) return;
                static_assert(std::tuple_size_v<typename Implementation::Bindings> == 0);
                auto added = registerCapability<typename Implementation::Capability>();
                if (!added) result = cxx::unexpected(added.error());
            }
        );
        cxx::tuple_traits::for_each_type<detail::BuiltinImplementations>(
            [&]<class Implementation, std::size_t>() noexcept
            {
                if (!result) return;
                auto added = registerCapabilityImplementation<Implementation>();
                if (!added) result = cxx::unexpected(added.error());
            }
        );
        if (!result) return result;
        return impl_->capability_registry_.validateDeclarations();
    }

    FeatureId RenderServer::Impl::publishFeature(FeatureEntry entry) noexcept
    {
        const auto id = features_.emplace(std::move(entry));
        auto& stored = *features_.find(id);
        features_by_name_.emplace(stored.name, id);
        return id;
    }

    RenderResult<void> RenderServer::Impl::validateImplementationDescription(
        const CapabilityDescription& capability,
        const CapabilityImplementation& implementation
    ) const noexcept
    {
        if (implementation.implementation.empty()) return cxx::unexpected(RenderError{kFeatureContract});
        for (const auto prerequisite : capability.prerequisites)
        {
            if (capability_registry_.find(prerequisite).isNull())
            {
                return cxx::unexpected(RenderError{kCapabilityPrerequisite});
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
            return cxx::unexpected(RenderError{kFeatureDuplicate});
        }
        return {};
    }

    RenderResult<FeatureId> RenderServer::registerFeature(FeatureEntry entry) noexcept
    {
        if (!impl_->canAccessCatalog()) return cxx::unexpected(RenderError{kCapabilityInUse});
        auto& state = *impl_;
        if (auto valid = state.validateFeature(entry); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        return state.publishFeature(std::move(entry));
    }

    RenderResult<CapabilityId> RenderServer::registerCapability(const CapabilityDescription& description) noexcept
    {
        if (!impl_->canAccessCatalog()) return cxx::unexpected(RenderError{kCapabilityInUse});
        std::vector<ResourceTypeId> resources;
        resources.reserve(description.resources.size());
        for (const auto* resource : description.resources)
        {
            if (!resource) return cxx::unexpected(RenderError{kCapabilityDeclaration});
            const auto id = impl_->domain_->resources_.typeId(resource->type);
            const auto* factory = impl_->domain_->resources_.factory(id);
            if (!factory) return cxx::unexpected(RenderError{kStorageFactory});
            if (resource != factory->descriptor)
            {
                if (auto valid = compatibleStorage(*resource, *factory->descriptor); !valid)
                    return cxx::unexpected(valid.error());
            }
            resources.push_back(id);
        }
        return impl_->capability_registry_.add(description, std::move(resources));
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
            const auto* method = domain_->command_registry_.binding(binding);
            const auto* command = domain_->command_registry_.findCommand(binding.command);
            const bool invalid_call = !method || !method->call.invoke;
            const bool invalid_route = !command || !command->effect || (method && method->storage_type.isValid());
            if (invalid_call || invalid_route) return cxx::unexpected(RenderError{kCommandContract});
        }
        for (const auto* command : commands)
        {
            const auto id = domain_->command_registry_.findCommand(*command);
            const bool has_exact_call =
                id && std::ranges::count_if(
                          implementation.command_bindings,
                          [&](const auto& binding) noexcept { return binding.command == *id; }
                      ) == 1;
            if (!has_exact_call)
            {
                return cxx::unexpected(RenderError{kCapabilityDeclaration});
            }
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
        return {};
    }

    RenderResult<CapabilityImplementationId> RenderServer::addCapabilityImplementation(
        CapabilityId capability,
        const CapabilityImplementation& declaration
    ) noexcept
    {
        if (!impl_->canAccessCatalog()) return cxx::unexpected(RenderError{kCapabilityInUse});
        auto& state = *impl_;
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
        return impl_->capability_registry_.removeImplementation(id);
    }

    RenderResult<void> RenderServer::removeCapability(CapabilityId id) noexcept
    {
        if (!impl_->canAccessCatalog()) return cxx::unexpected(RenderError{kCapabilityInUse});
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
        if (auto valid = impl_->domain_->resources_.validateFactory(factory, configuration); !valid)
            return cxx::unexpected(valid.error());
        if (auto valid = impl_->domain_->command_registry_.validateResourceBindings(factory.commands); !valid)
            return cxx::unexpected(valid.error());
        const auto type = impl_->domain_->resources_.publishFactory(factory, std::move(configuration));
        for (const auto& command : factory.commands)
        {
            const auto location = impl_->domain_->command_registry_.publishResourceBinding(command);
            if (!impl_->domain_->resources_.bindCommand(type, location)) std::terminate();
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
        if (auto valid = state.domain_->command_registry_.validateHandler(handler, bindings); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        const auto handler_type = handler.type;
        const auto id = state.domain_->command_registry_.publishHandler(std::move(handler));
        for (const auto& binding : bindings)
        {
            if (binding.handlerType() == handler_type) state.domain_->command_registry_.publishBinding(id, binding);
        }
        return id;
    }

    RenderResult<void> RenderServer::registerCommands(std::span<const CommandBinding> bindings) noexcept
    {
        if (!impl_->canAccessCatalog()) return cxx::unexpected(RenderError{kCapabilityInUse});
        if (impl_->stop_requested_.load()) return cxx::unexpected(RenderError{kRuntimeState});
        if (auto valid = impl_->domain_->command_registry_.validateStaticBindings(bindings); !valid) return valid;
        impl_->domain_->command_registry_.reserve(bindings.size(), 0);
        impl_->domain_->command_registry_.publishStaticBindings(bindings);
        return {};
    }

    RenderResult<CapabilityId> RenderServer::capabilityId(cxx::basic_type_info type) const noexcept
    {
        if (!impl_->canAccessCatalog()) return cxx::unexpected(RenderError{kCapabilityInUse});
        const auto id = impl_->capability_registry_.find(type);
        if (id.isNull()) return cxx::unexpected(RenderError{kRuntimeIdentity});
        return id;
    }

    RenderResult<HandlerId> RenderServer::handlerId(cxx::basic_type_info type, std::string_view key) const noexcept
    {
        if (!impl_->canAccessCatalog()) return cxx::unexpected(RenderError{kCapabilityInUse});
        auto id = impl_->domain_->command_registry_.findHandler(type, key);
        if (!id) return id;
        if (id->isNull()) return cxx::unexpected(RenderError{kRuntimeIdentity});
        return id;
    }

    RenderResult<CommandBindingLocation> RenderServer::commandBinding(
        const CommandDescriptor& command, cxx::basic_type_info method, HandlerId handler
    ) const noexcept
    {
        if (!impl_->canAccessCatalog()) return cxx::unexpected(RenderError{kCapabilityInUse});
        auto id = impl_->domain_->command_registry_.findCommand(command);
        if (!id) return cxx::unexpected(id.error());
        return impl_->domain_->command_registry_.findBinding(*id, method, handler);
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

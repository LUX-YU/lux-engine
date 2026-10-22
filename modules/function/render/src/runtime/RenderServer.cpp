#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/features/BuiltinFeatures.hpp>
#include <lux/engine/render/runtime/FeatureRegistration.hpp>
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
                             config.request_work_budget == 0 || config.max_config_bytes == 0 ||
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
                    return; // Unsupported hardware is not an invalid built-in declaration.
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
                publishFeature(Feature::Descriptor::descriptor, Feature::graph(paged_instances));
            }
        );
    }

    FeatureId RenderServer::Impl::publishFeature(
        const FeatureDescriptor& descriptor,
        const FeatureGraphOps& graph
    ) noexcept
    {
        const auto found = features_by_name.find(descriptor.type.name());
        if (found != features_by_name.end())
        {
            return found->second;
        }
        const auto id = features.emplace(descriptor, graph);
        auto& entry = *features.find(id);
        entry.id = id;
        features_by_name.emplace(entry.name, id);
        return id;
    }

    RenderResult<void> RenderServer::Impl::validateFeature(
        const FeatureDescriptor& descriptor,
        const FeatureGraphOps& graph
    ) const noexcept
    {
        const bool is_invalid_declaration =
            descriptor.type.name().empty() || !descriptor.describe_ports;
        if (is_invalid_declaration)
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        const auto found = features_by_name.find(descriptor.type.name());
        if (found != features_by_name.end())
        {
            const auto& previous = *features.find(found->second);
            const bool is_incompatible = previous.descriptor.config != descriptor.config ||
                                         previous.descriptor.describe_ports != descriptor.describe_ports ||
                                         previous.graph.build != graph.build;
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
        const FeatureDescriptor& descriptor,
        const FeatureGraphOps& graph,
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
        if (auto valid = state.validateFeature(descriptor, graph); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        if (auto valid = state.validateRegistration(factories, handlers, bindings); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        state.publishRegistration(factories, handlers, bindings);
        const auto id = state.publishFeature(descriptor, graph);
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

    RenderResult<FeatureId> RenderServer::findFeature(const FeatureDescriptor& descriptor) const noexcept
    {
        const auto found = impl_->features_by_name.find(descriptor.type.name());
        if (found == impl_->features_by_name.end())
        {
            return cxx::unexpected(RenderError{kRuntimeUnsupported});
        }
        if (impl_->features.find(found->second)->descriptor.config != descriptor.config)
        {
            return cxx::unexpected(RenderError{kFeatureContract});
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

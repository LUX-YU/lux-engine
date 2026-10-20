#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>

namespace lux::render
{
    namespace
    {
        bool sameFactory(const StorageFactory& left, const StorageFactory& right) noexcept
        {
            return left.native_type == right.native_type && left.config_type == right.config_type &&
                   left.ops_type == right.ops_type && left.create == right.create && left.ops == right.ops &&
                   left.binding_ops == right.binding_ops && left.compatible_config == right.compatible_config;
        }

        bool sameFeature(FeatureDefinition left, FeatureDefinition right) noexcept
        {
            return left.descriptor->config == right.descriptor->config &&
                   left.graph->describe == right.graph->describe && left.graph->build == right.graph->build &&
                   left.programs == right.programs;
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

    RenderResult<std::unique_ptr<RenderServer>> createRenderServer(
        RenderServerConfig config,
        execution::ExecutionRuntime& runtime,
        DeviceOptions options
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
        auto state = std::make_unique<RenderServer::Impl>(std::move(config), std::move(*responder), runtime.cpu());
        if (auto initialized = state->initialize(options); !initialized)
        {
            return cxx::unexpected(initialized.error());
        }
        return std::unique_ptr<RenderServer>(new RenderServer(std::move(state)));
    }

    ResourceRegistry& RenderServer::resources() noexcept
    {
        return *impl_->resources;
    }

    bool RenderServer::pagedInstances() const noexcept
    {
        return impl_->paged_instances;
    }

    RenderResult<void> RenderServer::install(RegistrationBatch& transaction) noexcept
    {
        auto& batch = *transaction.state_;
        auto& state = *impl_;
        if (state.stop_requested.load())
        {
            return cxx::unexpected(RenderError{kRuntimeState});
        }

        // Validate the complete Command set, both against the live directory and within this batch.
        std::vector<std::vector<CommandId>> commands;
        commands.reserve(batch.handlers.size());
        std::map<std::string_view, const CommandDescriptor*, std::less<>> descriptions;
        for (const auto& handler : batch.handlers)
        {
            auto prepared = state.command_registry_.prepareHandler(*handler.record, handler.calls);
            if (!prepared)
            {
                return cxx::unexpected(prepared.error());
            }
            commands.push_back(std::move(*prepared));
            for (const auto& call : handler.calls)
            {
                const auto* description = call.descriptor();
                const auto [found, inserted] = descriptions.emplace(description->name.name(), description);
                if (!inserted)
                {
                    if (auto valid = compatibleCommands(*found->second, *description); !valid)
                    {
                        return valid;
                    }
                }
            }
        }

        // Feature entries are stable. Names only discover declarations; live requests use FeatureId.
        std::map<std::string_view, std::size_t, std::less<>> feature_names;
        for (std::size_t i = 0; i < batch.features.size(); ++i)
        {
            auto& feature = batch.features[i];
            const auto definition = feature.definition;
            const bool is_missing_definition = !definition.descriptor || !definition.graph;
            if (is_missing_definition)
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            const bool is_invalid_definition =
                definition.descriptor->type.name().empty() || !definition.graph->describe;
            if (is_invalid_definition)
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            const auto name = definition.descriptor->type.name();
            const auto installed = state.features_by_name.find(name);
            if (installed != state.features_by_name.end())
            {
                const auto& entry = *state.features.find(installed->second);
                if (!sameFeature({&entry.descriptor, &entry.graph, entry.programs}, definition))
                {
                    return cxx::unexpected(RenderError{kFeatureDuplicate});
                }
                feature.id = installed->second;
            }
            const auto [found, inserted] = feature_names.emplace(name, i);
            if (!inserted && !sameFeature(batch.features[found->second].definition, definition))
            {
                return cxx::unexpected(RenderError{kFeatureDuplicate});
            }
        }

        auto& resources = *state.resources;
        std::vector<StorageFactoryInstall> factories;
        factories.reserve(batch.storage_factories.size());
        std::map<std::string_view, std::size_t, std::less<>> storage_names;
        for (std::size_t i = 0; i < batch.storage_factories.size(); ++i)
        {
            auto& storage = batch.storage_factories[i];
            const auto& factory = storage.factory;
            storage.id = resources.typeId(factory.descriptor->type);
            if (!storage.id.isNull())
            {
                const auto& previous = *resources.factory(storage.id);
                if (auto valid = compatibleStorage(*previous.descriptor, *factory.descriptor); !valid)
                {
                    return valid;
                }
                if (!sameFactory(previous, factory))
                {
                    return cxx::unexpected(RenderError{kStorageMismatch});
                }
                if (!storage.use_existing_configuration)
                {
                    const auto* configuration = resources.configuration(storage.id);
                    if (!configuration || *configuration != storage.configuration)
                    {
                        return cxx::unexpected(RenderError{kStorageMismatch});
                    }
                }
            }
            const auto [found, inserted] = storage_names.emplace(factory.descriptor->type.name(), i);
            if (!inserted)
            {
                const auto& previous = batch.storage_factories[found->second];
                if (auto valid = compatibleStorage(*previous.factory.descriptor, *factory.descriptor); !valid)
                {
                    return valid;
                }
                const bool is_factory_mismatch = !sameFactory(previous.factory, factory);
                const bool is_config_mismatch =
                    !storage.use_existing_configuration && previous.configuration != storage.configuration;
                if (is_factory_mismatch || is_config_mismatch)
                {
                    return cxx::unexpected(RenderError{kStorageMismatch});
                }
            }
        }

        for (const auto& [name, index] : storage_names)
        {
            auto& storage = batch.storage_factories[index];
            if (storage.id.isNull())
            {
                factories.push_back({storage.factory, std::move(storage.configuration)});
            }
        }

        // This is the last recoverable boundary: Registry validates every capability/config before publication.
        // No Provider callback is called after this point. Ordinary host OOM remains fatal.
        state.command_registry_.reserve(descriptions.size(), batch.handlers.size());
        state.features.reserve(state.features.size() + feature_names.size());
        auto installed = resources.installFactories(factories);
        if (!installed)
        {
            return cxx::unexpected(installed.error());
        }
        for (std::size_t i = 0; i < batch.handlers.size(); ++i)
        {
            auto& handler = batch.handlers[i];
            state.command_registry_.registerHandler(std::move(handler.record), handler.calls, std::move(commands[i]));
        }
        for (auto& feature : batch.features)
        {
            if (!feature.id.isNull())
            {
                continue;
            }
            const auto found = state.features_by_name.find(feature.definition.descriptor->type.name());
            if (found != state.features_by_name.end())
            {
                feature.id = found->second;
                continue;
            }
            feature.id = state.features.emplace(feature.definition);
            auto& entry = *state.features.find(feature.id);
            entry.id = feature.id;
            state.features_by_name.emplace(entry.name, feature.id);
        }
        for (auto& storage : batch.storage_factories)
        {
            storage.id = resources.typeId(storage.factory.descriptor->type);
        }
        return {};
    }

    RenderResult<CommandId> RenderServer::findCommand(const CommandDescriptor& descriptor) const noexcept
    {
        return impl_->command_registry_.findCommand(descriptor);
    }

    RenderResult<HandlerId> RenderServer::findHandler(CppType type, std::string_view instance) const noexcept
    {
        if (impl_->stop_requested.load())
        {
            return cxx::unexpected(RenderError{kRuntimeState});
        }
        return impl_->command_registry_.findHandler(type, instance);
    }

    void RenderServer::stop() noexcept
    {
        impl_->stop();
    }

    RenderResult<void> RenderServer::run() noexcept
    {
        return impl_->run();
    }

    RenderServerStats RenderServer::stats() const noexcept
    {
        return impl_->stats();
    }
} // namespace lux::render

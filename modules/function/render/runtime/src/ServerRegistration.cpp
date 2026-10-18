#include "ServerImpl.hpp"

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

        bool sameFeature(feature_detail::Implementation left, feature_detail::Implementation right) noexcept
        {
            return left.descriptor->config == right.descriptor->config &&
                   left.graph->describe == right.graph->describe && left.graph->build == right.graph->build &&
                   left.programs == right.programs;
        }
    } // namespace

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
                 std::string(entry.descriptor.config.token.name()),
                 entry.descriptor.config.size,
                 entry.descriptor.config.alignment}
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
        device->resources().forEachType(
            [&](StorageTypeId id, const StorageDescriptor& type, std::uint32_t capabilities) noexcept
            { result.storages.push_back({id, std::string(type.type.name()), capabilities}); }
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

    vulkan::DeviceSession& runtime_detail::ServerBootstrap::device(RenderServer& server) noexcept
    {
        return *server.impl_->device;
    }

    RenderResult<void> RenderServer::install(RegistrationBatch& batch) noexcept
    {
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

        auto& resources = state.device->resources();
        std::vector<StorageFactoryInstall> factories;
        factories.reserve(batch.storage_factories.size());
        std::map<std::string_view, std::size_t, std::less<>> storage_names;
        for (std::size_t i = 0; i < batch.storage_factories.size(); ++i)
        {
            auto& storage = batch.storage_factories[i];
            const auto& factory = storage.factory;
            storage.id = resources.typeId(factory.description->type);
            if (!storage.id.isNull())
            {
                const auto& previous = *resources.factory(storage.id);
                if (auto valid = compatibleStorage(*previous.description, *factory.description); !valid)
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
            const auto [found, inserted] = storage_names.emplace(factory.description->type.name(), i);
            if (!inserted)
            {
                const auto& previous = batch.storage_factories[found->second];
                if (auto valid = compatibleStorage(*previous.factory.description, *factory.description); !valid)
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
            if (inserted && storage.id.isNull())
            {
                factories.push_back({factory, storage.configuration});
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
            storage.id = resources.typeId(storage.factory.description->type);
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
        command_registry_.selectHandler(input, response);
    }
} // namespace lux::render

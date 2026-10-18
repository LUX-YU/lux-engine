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

        RenderResult<void> compatibleSupport(
            std::span<const CommandBinding> left,
            std::span<const CommandBinding> right
        ) noexcept
        {
            for (std::size_t i = 0; i < left.size(); ++i)
            {
                for (std::size_t j = 0; j < right.size(); ++j)
                {
                    if (left[i].descriptor()->name.name() != right[j].descriptor()->name.name())
                    {
                        continue;
                    }
                    if (auto valid = compatibleCommands(*left[i].descriptor(), *right[j].descriptor()); !valid)
                    {
                        return valid;
                    }
                }
            }
            return {};
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

    RenderServer::HandlerRecord* RenderServer::handlerRecord(HandlerId id) noexcept
    {
        return impl_->command_registry_.findHandler(id);
    }

    vulkan::DeviceSession& runtime_detail::ServerBootstrap::device(RenderServer& server) noexcept
    {
        return *server.impl_->device;
    }

    RenderResult<HandlerId> RenderServer::installHandler(
        std::unique_ptr<HandlerRecord> owner,
        std::span<const CommandBinding> calls
    ) noexcept
    {
        auto commands = prepareHandler(*owner, calls);
        if (!commands)
        {
            return cxx::unexpected(commands.error());
        }
        return publishHandler(std::move(owner), calls, std::move(*commands));
    }

    RenderResult<StorageTypeId> RenderServer::prepareStorage(const StorageInstall& candidate, bool ensure)
        const noexcept
    {
        const auto& resources = impl_->device->resources();
        const auto& factory = candidate.factory;
        const auto id = resources.typeId(factory.description->type);
        if (id.isNull())
        {
            return id;
        }
        const auto& previous = *resources.factory(id);
        if (auto valid = compatibleStorage(*previous.description, *factory.description); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        if (!sameFactory(previous, factory))
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        // A Feature dependency honours an explicitly installed compatible configuration.
        if (!ensure)
        {
            const auto* original = resources.configuration(id);
            if (!original || *original != candidate.config)
            {
                return cxx::unexpected(RenderError{kStorageMismatch});
            }
        }
        return id;
    }

    RenderResult<StorageTypeId> RenderServer::installStorage(StorageInstall candidate) noexcept
    {
        if (impl_->stop_requested.load())
        {
            return cxx::unexpected(RenderError{kRuntimeState});
        }
        if (candidate.owner)
        {
            auto commands = prepareHandler(*candidate.handler, candidate.calls);
            if (!commands)
            {
                return cxx::unexpected(commands.error());
            }
            candidate.commands = std::move(*commands);
        }
        auto id = prepareStorage(candidate, false);
        if (!id)
        {
            return cxx::unexpected(id.error());
        }
        if (id->isNull())
        {
            id = impl_->device->resources().installFactory(std::move(candidate.factory), std::move(candidate.config));
            if (!id)
            {
                return cxx::unexpected(id.error());
            }
        }
        if (candidate.owner)
        {
            publishHandler(std::move(candidate.owner), candidate.calls, std::move(candidate.commands));
        }
        return *id;
    }

    RenderResult<FeatureId> RenderServer::installFeature(
        feature_detail::Implementation definition,
        HandlerRecord* handler,
        std::unique_ptr<HandlerRecord> owner,
        std::span<const CommandBinding> calls,
        std::vector<StorageInstall> dependencies
    ) noexcept
    {
        auto& state = *impl_;
        const bool is_invalid_definition = !definition.descriptor || !definition.graph ||
                                           (definition.descriptor && definition.descriptor->type.name().empty()) ||
                                           (definition.graph && !definition.graph->describe);
        if (state.stop_requested.load())
        {
            return cxx::unexpected(RenderError{kRuntimeState});
        }
        if (is_invalid_definition)
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        std::vector<CommandId> commands;
        if (owner)
        {
            auto prepared = prepareHandler(*handler, calls);
            if (!prepared)
            {
                return cxx::unexpected(prepared.error());
            }
            commands = std::move(*prepared);
        }
        FeatureId id;
        for (const auto& entry : state.features)
        {
            if (entry.descriptor.type.name() != definition.descriptor->type.name())
            {
                continue;
            }
            const bool is_different_definition = entry.descriptor.config != definition.descriptor->config ||
                                                 entry.graph.describe != definition.graph->describe ||
                                                 entry.graph.build != definition.graph->build ||
                                                 entry.programs != definition.programs;
            if (is_different_definition)
            {
                return cxx::unexpected(RenderError{kFeatureDuplicate});
            }
            id = entry.id;
            break;
        }
        std::vector<StorageFactoryInstall> factories;
        factories.reserve(dependencies.size());
        for (std::size_t i = 0; i < dependencies.size(); ++i)
        {
            auto& candidate = dependencies[i];
            auto storage = prepareStorage(candidate, true);
            if (!storage)
            {
                return cxx::unexpected(storage.error());
            }
            bool duplicate = false;
            for (std::size_t j = 0; j < i; ++j)
            {
                const auto& previous = dependencies[j];
                if (previous.factory.description->type != candidate.factory.description->type)
                {
                    continue;
                }
                if (auto valid = compatibleStorage(*previous.factory.description, *candidate.factory.description);
                    !valid)
                {
                    return cxx::unexpected(valid.error());
                }
                if (!sameFactory(previous.factory, candidate.factory))
                {
                    return cxx::unexpected(RenderError{kStorageMismatch});
                }
                duplicate = true;
                break;
            }
            if (storage->isNull() && !duplicate)
            {
                factories.push_back({candidate.factory, candidate.config});
            }
            if (!candidate.owner)
            {
                continue;
            }
            auto prepared = prepareHandler(*candidate.handler, candidate.calls);
            if (!prepared)
            {
                return cxx::unexpected(prepared.error());
            }
            candidate.commands = std::move(*prepared);
            if (auto valid = compatibleSupport(calls, candidate.calls); !valid)
            {
                return cxx::unexpected(valid.error());
            }
            for (std::size_t j = 0; j < i; ++j)
            {
                if (auto valid = compatibleSupport(dependencies[j].calls, candidate.calls); !valid)
                {
                    return cxx::unexpected(valid.error());
                }
            }
        }
        // Registry performs its capability/config checks for every factory before any publication.
        if (auto installed = state.device->resources().installFactories(factories); !installed)
        {
            return cxx::unexpected(installed.error());
        }
        for (auto& candidate : dependencies)
        {
            if (candidate.owner)
            {
                publishHandler(std::move(candidate.owner), candidate.calls, std::move(candidate.commands));
            }
        }
        // All recoverable checks are complete. Stable Feature entries survive later directory growth.
        if (id.isNull())
        {
            id = state.features.emplace(definition);
            state.features.find(id)->id = id;
        }
        if (owner)
        {
            publishHandler(std::move(owner), calls, std::move(commands));
        }
        return id;
    }

} // namespace lux::render

namespace lux::render
{
    RenderResult<CommandId> RenderServer::findCommand(const CommandDescriptor& descriptor) const noexcept
    {
        return impl_->command_registry_.findCommand(descriptor);
    }

    RenderResult<HandlerId> RenderServer::findHandler(CppType type, std::string_view key) const noexcept
    {
        if (impl_->stop_requested.load())
        {
            return cxx::unexpected(RenderError{kRuntimeState});
        }
        return impl_->command_registry_.findHandler(type, key);
    }

    RenderResult<std::vector<CommandId>> RenderServer::prepareHandler(
        const HandlerRecord& owner,
        std::span<const CommandBinding> calls
    ) const noexcept
    {
        if (impl_->stop_requested.load())
        {
            return cxx::unexpected(RenderError{kRuntimeState});
        }
        return impl_->command_registry_.prepareHandler(owner, calls);
    }

    HandlerId RenderServer::publishHandler(
        std::unique_ptr<HandlerRecord> owner,
        std::span<const CommandBinding> calls,
        std::vector<CommandId> commands
    ) noexcept
    {
        return impl_->command_registry_.registerHandler(std::move(owner), calls, std::move(commands));
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

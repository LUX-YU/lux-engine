#include "ServerImpl.hpp"

namespace lux::render
{
    RenderResult<CommandId> runtime_detail::CommandRegistry::findCommand(const CommandDescriptor& descriptor
    ) const noexcept
    {
        for (const auto& entry : command_entries_)
        {
            const auto& stored = *entry.cold->binding.descriptor();
            if (stored.name.name() != descriptor.name.name())
            {
                continue;
            }
            if (auto compatible = compatibleCommands(stored, descriptor); !compatible)
            {
                return cxx::unexpected(compatible.error());
            }
            return entry.cold->id;
        }
        return cxx::unexpected(RenderError{kRuntimeUnsupported});
    }

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

    CommandList runtime_detail::CommandRegistry::listCommands() const noexcept
    {
        CommandList result;
        result.commands.reserve(command_entries_.size());
        for (const auto& route : command_entries_)
        {
            const auto& entry = *route.cold;
            const auto& descriptor = *entry.binding.descriptor();
            result.commands.push_back(
                {std::string(descriptor.name.name()),
                 entry.id,
                 descriptor.request,
                 descriptor.response,
                 route.active.invoke != nullptr}
            );
        }
        return result;
    }

    void RenderServer::Impl::listCommands(
        const ListCommands::Request& _request,
        ListCommands::Response& output_response
    ) noexcept
    {
        output_response = command_registry_.listCommands();
    }

    void runtime_detail::CommandRegistry::listCommandBindings(
        const ListCommandBindings::Request& _request,
        ListCommandBindings::Response& output_response
    ) noexcept
    {
        const auto* entry = command_entries_.find(_request);
        if (!entry)
        {
            output_response = cxx::unexpected(RenderError{kRuntimeIdentity});
            return;
        }
        CommandBindings result{entry->cold->selected};
        result.handlers.reserve(entry->cold->supported.size());
        for (const auto& support : entry->cold->supported)
        {
            const auto& owner = **handler_owners_.find(support.id);
            result.handlers.push_back({support.id, owner.name, owner.key});
        }
        output_response = std::move(result);
    }

    void runtime_detail::CommandRegistry::selectHandler(
        const SelectCommandBinding::Request& _request,
        SelectCommandBinding::Response& output_response
    ) noexcept
    {
        const auto& input = _request;
        auto* entry = command_entries_.find(input.command);
        if (!entry)
        {
            output_response = cxx::unexpected(RenderError{kRuntimeIdentity});
            return;
        }
        // Runtime-private controls have no installed Handler choice. In particular, disabling
        // ListCommands would remove the only discovery path for every future Client.
        if (entry->cold->supported.empty())
        {
            output_response = cxx::unexpected(RenderError{kRuntimeUnsupported});
            return;
        }
        if (input.handler.isNull())
        {
            entry->cold->selected = {};
            entry->active = {};
            {
                output_response = Ack{};
                return;
            }
        }
        const auto& supported = entry->cold->supported;
        const auto found = std::ranges::find(supported, input.handler, &SupportedHandler::id);
        if (found == supported.end())
        {
            output_response = cxx::unexpected(RenderError{kRuntimeIdentity});
            return;
        }
        entry->cold->selected = found->id;
        entry->active = found->call;
        output_response = Ack{};
    }

    CommandId runtime_detail::CommandRegistry::registerCommand(CommandBinding binding) noexcept
    {
        // Full registration comparison proved equivalence; retain the template's static contract.
        auto metadata = std::make_unique<CommandMetadata>(CommandMetadata{{}, binding});
        auto* cold = metadata.get();
        const auto id = command_entries_.emplace(CommandEntry{binding.call(), std::move(metadata)});
        cold->id = id;
        return id;
    }

    RenderResult<HandlerId> runtime_detail::CommandRegistry::findHandler(CommandValueType type, std::string_view key)
        const noexcept
    {
        for (const auto& entry : handler_owners_)
        {
            if (entry->type.type.name() != type.type.name() || entry->key != key)
            {
                continue;
            }
            if (entry->type != type)
            {
                return cxx::unexpected(RenderError{kProtocolContract});
            }
            return entry->id;
        }
        return HandlerId{};
    }

    RenderServer::HandlerEntry* RenderServer::handlerEntry(HandlerId id) noexcept
    {
        return impl_->command_registry_.findHandler(id);
    }

    vulkan::DeviceSession& runtime_detail::ServerBootstrap::device(RenderServer& server) noexcept
    {
        return *server.impl_->device;
    }

    RenderResult<std::vector<CommandId>> runtime_detail::CommandRegistry::prepareHandler(
        const HandlerEntry& owner,
        std::span<const CommandBinding> calls
    ) const noexcept
    {
        const bool is_invalid_installation = owner.name.empty() || calls.empty();
        if (is_invalid_installation)
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        std::vector<CommandId> commands(calls.size());
        for (std::size_t i = 0; i < calls.size(); ++i)
        {
            const auto& call = calls[i];
            // Only CommandMethod::bind on this owned typed object reaches this private transaction.
            if (auto valid = validateCommand(*call.descriptor()); !valid)
            {
                return cxx::unexpected(valid.error());
            }
            for (std::size_t j = 0; j < i; ++j)
            {
                if (calls[j].descriptor()->name.name() == call.descriptor()->name.name())
                {
                    return cxx::unexpected(RenderError{kFeatureDuplicate});
                }
            }
            for (const auto& old : command_entries_)
            {
                if (old.cold->binding.descriptor()->name.name() != call.descriptor()->name.name())
                {
                    continue;
                }
                if (auto valid = compatibleCommands(*old.cold->binding.descriptor(), *call.descriptor()); !valid)
                {
                    return cxx::unexpected(valid.error());
                }
                commands[i] = old.cold->id;
                break;
            }
        }
        return commands;
    }

    HandlerId runtime_detail::CommandRegistry::registerHandler(
        std::unique_ptr<HandlerEntry> owner,
        std::span<const CommandBinding> calls,
        std::vector<CommandId> commands
    ) noexcept
    {
        // All recoverable qualification precedes publication. Ordinary heap exhaustion is fatal.
        auto* handler = owner.get();
        const auto id = handler_owners_.emplace(std::move(owner));
        handler->id = id;
        for (std::size_t i = 0; i < calls.size(); ++i)
        {
            // Another qualified dependency may have published this previously absent command.
            if (commands[i].isNull())
            {
                for (const auto& old : command_entries_)
                {
                    if (old.cold->binding.descriptor()->name.name() == calls[i].descriptor()->name.name())
                    {
                        commands[i] = old.cold->id;
                        break;
                    }
                }
            }
            const bool first = commands[i].isNull();
            if (first)
            {
                commands[i] = registerCommand(calls[i]);
            }
            auto& entry = *command_entries_.find(commands[i]);
            entry.cold->supported.push_back({id, calls[i].call()});
            if (first)
            {
                entry.cold->selected = id;
            }
            // Existing active remains unchanged, including a deliberately unbound command.
        }
        return id;
    }

    RenderResult<HandlerId> RenderServer::installHandler(
        std::unique_ptr<HandlerEntry> owner,
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
        HandlerEntry* handler,
        std::unique_ptr<HandlerEntry> owner,
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

    RenderResult<HandlerId> RenderServer::findHandler(CommandValueType type, std::string_view key) const noexcept
    {
        if (impl_->stop_requested.load())
        {
            return cxx::unexpected(RenderError{kRuntimeState});
        }
        return impl_->command_registry_.findHandler(type, key);
    }

    RenderResult<std::vector<CommandId>> RenderServer::prepareHandler(
        const HandlerEntry& owner,
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
        std::unique_ptr<HandlerEntry> owner,
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
